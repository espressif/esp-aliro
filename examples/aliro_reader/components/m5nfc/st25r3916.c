/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "st25r3916.h"
#include "st25r3916_regs.h"

#include <driver/i2c_master.h>
#include <esp_check.h>
#include <esp_idf_version.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "st25r3916";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static bool s_field_on;

static esp_timer_handle_t s_poll_timer;
static StaticSemaphore_t s_poll_wakeup_storage;
static SemaphoreHandle_t s_poll_wakeup;

#define I2C_TIMEOUT_MS 100
// 256 bytes plus parity at 106 kbit/s take <22 ms; allow framing overhead.
#define FRAME_TRANSFER_TIMEOUT_MS 30

static void poll_wakeup(void *arg)
{
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

static esp_err_t direct_command(uint8_t command)
{
    return i2c_master_transmit(s_device, &command, 1, I2C_TIMEOUT_MS);
}

static size_t register_header(uint16_t reg, bool read, uint8_t header[2])
{
    size_t len = 0;
    if (reg & 0x100) {
        header[len++] = 0xfb;
    }
    header[len++] = (reg & 0x3f) | (read ? 0x40 : 0);
    return len;
}

static esp_err_t read_registers(uint16_t reg, uint8_t *value, size_t len)
{
    uint8_t header[2];
    size_t header_len = register_header(reg, true, header);
    return i2c_master_transmit_receive(s_device, header, header_len, value, len, I2C_TIMEOUT_MS);
}

static esp_err_t write_register(uint16_t reg, uint8_t value)
{
    uint8_t data[3];
    size_t len = register_header(reg, false, data);
    data[len++] = value;
    return i2c_master_transmit(s_device, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t modify_register(uint16_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    ESP_RETURN_ON_ERROR(read_registers(reg, &current, 1), TAG, "read register");
    return write_register(reg, (current & ~mask) | value);
}

static esp_err_t read_interrupts(uint32_t *status)
{
    uint8_t bytes[4];
    ESP_RETURN_ON_ERROR(read_registers(ST25R3916_REG_IRQ, bytes, sizeof(bytes)), TAG, "read interrupts");
    *status = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
    return ESP_OK;
}

static esp_err_t clear_interrupts(void)
{
    uint32_t ignored;
    return read_interrupts(&ignored);
}

static esp_err_t wait_interrupt(uint32_t mask, uint32_t timeout_ms, uint32_t *status)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    *status = 0;

    while (true) {
        uint32_t current;
        ESP_RETURN_ON_ERROR(read_interrupts(&current), TAG, "poll interrupts");
        *status |= current;
        if (*status & mask) {
            return ESP_OK;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= deadline_us) {
            return ESP_ERR_TIMEOUT;
        }

        // A 10 ms RTOS tick can exceed the peer's WTX/R-block reply window.
        // Use the existing ESP timer task for sub-tick wakeups, not a busy loop
        // that could starve the idle task during a long WTX wait.
        uint64_t delay_us = deadline_us - now_us < 200 ? (uint64_t)(deadline_us - now_us) : 200;
        ESP_RETURN_ON_ERROR(esp_timer_start_once(s_poll_timer, delay_us), TAG, "poll timer");
        xSemaphoreTake(s_poll_wakeup, portMAX_DELAY);
    }
}

static esp_err_t write_fifo(const uint8_t *data, size_t len)
{
    if (len > 254) {
        return ESP_ERR_INVALID_SIZE;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    uint8_t command = 0x80;
    // IDF 5.4 uses non-const descriptor pointers, but only reads these buffers.
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = &command, .buffer_size = 1},
        {.write_buffer = (uint8_t *)data, .buffer_size = len},
    };
    return i2c_master_multi_buffer_transmit(s_device, buffers, 2, I2C_TIMEOUT_MS);
#else  // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 4, 0)
    uint8_t buffer[255];
    buffer[0] = 0x80;
    memcpy(buffer + 1, data, len);
    return i2c_master_transmit(s_device, buffer, len + 1, I2C_TIMEOUT_MS);
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
}

static esp_err_t read_fifo(uint8_t *data, size_t capacity, size_t *len, uint8_t *bits)
{
    uint8_t status[2];
    ESP_RETURN_ON_ERROR(read_registers(ST25R3916_REG_FIFO_STATUS, status, 2), TAG, "FIFO status");
    *len = status[0] | ((size_t)(status[1] >> 6) << 8);
    *bits = (status[1] >> 1) & 7;
    if ((status[1] & 0x30) || *len > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (*len == 0) {
        return ESP_OK;
    }

    const uint8_t command = 0x9f;
    return i2c_master_transmit_receive(s_device, &command, 1, data, *len, I2C_TIMEOUT_MS);
}

static esp_err_t set_frame_timer(uint32_t timeout_ms)
{
    // Timer ticks are 64/fc, or 4096/fc with nrt_step. Beyond its range,
    // disable the hardware timer and retain the bounded software deadline.
    uint64_t ticks = ((uint64_t)timeout_ms * 13560 + 63) / 64;
    uint8_t control = 0;
    if (ticks > UINT16_MAX) {
        control = 1;
        ticks = ((uint64_t)timeout_ms * 13560 + 4095) / 4096;
    }
    if (ticks > UINT16_MAX) {
        ticks = 0;
    }

    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_TIMER_CONTROL, 3, control), TAG, "timer mode");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_NO_RESPONSE_TIMER, ticks >> 8), TAG, "timer high");
    return write_register(ST25R3916_REG_NO_RESPONSE_TIMER + 1, ticks & 0xff);
}

// Raw RF exchange also serves the short WUPA and partial-bit anticollision frames.
// A collision is reported separately so selection can extend the known UID prefix.
static esp_err_t exchange_raw(const uint8_t *tx, size_t tx_bits, uint8_t direct_tx, uint8_t *rx, size_t *rx_len,
                              uint8_t *rx_bits, bool anticollision, bool *collision, uint32_t timeout_ms)
{
    size_t capacity = *rx_len;
    *rx_len = 0;
    *collision = false;

    ESP_RETURN_ON_ERROR(direct_command(ST25R3916_CMD_STOP), TAG, "stop");
    ESP_RETURN_ON_ERROR(set_frame_timer(timeout_ms), TAG, "frame timer");
    ESP_RETURN_ON_ERROR(clear_interrupts(), TAG, "clear interrupts");
    if (tx_bits) {
        ESP_RETURN_ON_ERROR(write_fifo(tx, (tx_bits + 7) / 8), TAG, "write FIFO");
        ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_TX_BYTES, tx_bits >> 8), TAG, "TX length high");
        ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_TX_BYTES + 1, tx_bits & 0xff), TAG, "TX length low");
    }

    ESP_RETURN_ON_ERROR(direct_command(direct_tx), TAG, "transmit");

    const uint32_t terminal = ST25R3916_IRQ_RX_END | ST25R3916_IRQ_COLLISION | ST25R3916_IRQ_NO_RESPONSE |
        ST25R3916_IRQ_CRC | ST25R3916_IRQ_RX_ERROR;
    uint32_t status;
    esp_err_t err = wait_interrupt(ST25R3916_IRQ_TX_END | terminal, FRAME_TRANSFER_TIMEOUT_MS, &status);
    if (err != ESP_OK) {
        return err;
    }

    if (!(status & (ST25R3916_IRQ_RX_START | terminal))) {
        uint32_t current;
        // FWT is a wait for RX start after TX end, not a whole-exchange deadline.
        err = wait_interrupt(ST25R3916_IRQ_RX_START | terminal, timeout_ms, &current);
        status |= current;
    }

    if (err == ESP_OK && !(status & terminal)) {
        uint32_t current;
        // A response that starts just before FWT expires may finish after it.
        err = wait_interrupt(terminal, FRAME_TRANSFER_TIMEOUT_MS, &current);
        status |= current;
    }
    if (err != ESP_OK) {
        return err;
    }

    *collision = (status & ST25R3916_IRQ_COLLISION) != 0;
    if (*collision && !anticollision) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!*collision && (status & ST25R3916_IRQ_CRC)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (!*collision && (status & ST25R3916_IRQ_RX_ERROR)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!(status & (ST25R3916_IRQ_RX_END | ST25R3916_IRQ_COLLISION))) {
        // Only the chip's no-response IRQ proves RF absence, not a bus/software timeout.
        return (status & ST25R3916_IRQ_NO_RESPONSE) ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
    }

    return read_fifo(rx, capacity, rx_len, rx_bits);
}

static esp_err_t initialize_chip(void)
{
    ESP_RETURN_ON_ERROR(direct_command(ST25R3916_CMD_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10) + 1);

    uint8_t identity;
    ESP_RETURN_ON_ERROR(read_registers(ST25R3916_REG_IC_IDENTITY, &identity, 1), TAG, "identity at I2C 0x50");
    if ((identity >> 3) != 5) {
        ESP_LOGE(TAG, "unexpected IC identity 0x%02x", identity);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "ST25R3916 revision %u", identity & 7);

    // Disable pin interrupts; status registers are still polled and clear on read.
    for (unsigned reg = ST25R3916_REG_IRQ_MASK; reg < ST25R3916_REG_IRQ_MASK + 4; ++reg) {
        ESP_RETURN_ON_ERROR(write_register(reg, 0xff), TAG, "mask IRQ");
    }

    // The Unit NFC schematic supplies VDD and VDD_TX from its 3.3 V LDO.
    // sup3V belongs to IO configuration register 2 (DS12484, table 20).
    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_IO2, 0x80, 0x80), TAG, "3.3 V supply");
    ESP_RETURN_ON_ERROR(clear_interrupts(), TAG, "clear interrupts");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OPERATION, 0x80), TAG, "oscillator on");
    uint32_t status;
    ESP_RETURN_ON_ERROR(wait_interrupt(ST25R3916_IRQ_OSC, 10, &status), TAG, "oscillator ready");

    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_REGULATOR, 0xf0, 0), TAG, "automatic regulators");
    ESP_RETURN_ON_ERROR(clear_interrupts(), TAG, "clear interrupts");
    ESP_RETURN_ON_ERROR(direct_command(ST25R3916_CMD_ADJUST_REGULATORS), TAG, "adjust regulators");
    // DS12484 section 4.4.17 prohibits bus activity during this 5 ms calibration.
    vTaskDelay(pdMS_TO_TICKS(6) + 1);
    ESP_RETURN_ON_ERROR(wait_interrupt(ST25R3916_IRQ_DIRECT_COMMAND, 10, &status), TAG, "regulators ready");

    // Reader mode, ISO-A, 106 kbit/s, OOK. AAT stays disabled: this board has
    // a fixed matching network, not varactors connected to AAT_A/AAT_B.
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_MODE, 1U << 3), TAG, "NFC-A reader mode");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_BIT_RATE, 0), TAG, "106 kbit/s");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_ISO14443A, 0), TAG, "standard framing");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_AUX, 0), TAG, "correlator/CRC");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_TX_DRIVER, 0), TAG, "OOK driver");

    // DS12484 tables 40-43: AM peak detector, automatic squelch, continuous
    // AGC with preset and ratio 6, full initial receiver gain.
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_RX1, 0), TAG, "receiver filters");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_RX2, (1U << 5) | (1U << 3) | (1U << 2) | 1), TAG, "receiver AGC");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_RX3, 0), TAG, "receiver gain 1");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_RX4, 0), TAG, "receiver gain 2");
    ESP_RETURN_ON_ERROR(direct_command(ST25R3916_CMD_RESET_RX_GAIN), TAG, "load receiver gain");

    // Correlator BPSK thresholds: first stage 4, next stages 3 (table 46).
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_CORRELATOR1, (1U << 6) | (1U << 4) | 1), TAG,
                        "correlator thresholds");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_CORRELATOR2, 0), TAG, "correlator summation");

    // Mask reception for 14 * 64/fc = 66 us after TX (table 48).
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_MASK_RX_TIMER, 14), TAG, "receive guard");

    // Drive from VDD_AM for the first two carrier cycles at each OOK edge
    // (section 4.2.12 and tables 99-102). Other cycles use the OOK settings.
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OVERSHOOT1, 1U << 6), TAG, "overshoot mode");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OVERSHOOT2, 3), TAG, "overshoot pattern");
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_UNDERSHOOT1, 1U << 6), TAG, "undershoot mode");
    return write_register(ST25R3916_REG_UNDERSHOOT2, 3);
}

esp_err_t st25r3916_init(void)
{
    if (s_bus != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CONFIG_ST25R3916_I2C_PORT,
        .sda_io_num = CONFIG_ST25R3916_PIN_SDA,
        .scl_io_num = CONFIG_ST25R3916_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_bus), TAG, "create I2C bus");

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x50,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);

    if (err == ESP_OK) {
        if (s_poll_wakeup == NULL) {
            s_poll_wakeup = xSemaphoreCreateBinaryStatic(&s_poll_wakeup_storage);
        }
        while (xSemaphoreTake(s_poll_wakeup, 0)) {
        }

        const esp_timer_create_args_t timer_args = {
            .callback = poll_wakeup,
            .arg = s_poll_wakeup,
            .name = "nfc_poll",
        };
        err = esp_timer_create(&timer_args, &s_poll_timer);
    }

    if (err == ESP_OK) {
        err = initialize_chip();
    }

    if (err != ESP_OK) {
        if (s_poll_timer != NULL) {
            esp_timer_delete(s_poll_timer);
            s_poll_timer = NULL;
        }

        if (s_device != NULL) {
            // Best effort: do not leave an RF field enabled after a failed init.
            write_register(ST25R3916_REG_OPERATION, 0x80);
            i2c_master_bus_rm_device(s_device);
            s_device = NULL;
        }

        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return err;
}

esp_err_t st25r3916_field(bool enabled)
{
    if (s_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enabled) {
        esp_err_t err = write_register(ST25R3916_REG_OPERATION, 0x80);
        s_field_on = false;
        return err;
    }
    if (s_field_on) {
        return ESP_OK;
    }

    // Guarantee a card reset between sessions, including after an uncertain I2C
    // failure while disabling the previous field.
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OPERATION, 0x80), TAG, "RF field off");
    vTaskDelay(pdMS_TO_TICKS(5) + 1);

    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OPERATION, 0xc1), TAG, "enable receiver/field detector");
    ESP_RETURN_ON_ERROR(clear_interrupts(), TAG, "clear interrupts");
    ESP_RETURN_ON_ERROR(direct_command(ST25R3916_CMD_INITIAL_FIELD_ON), TAG, "RF collision avoidance");
    uint32_t status;
    esp_err_t err =
        wait_interrupt(ST25R3916_IRQ_FIELD_ON | ST25R3916_IRQ_RF_GUARD | ST25R3916_IRQ_RF_COLLISION, 10, &status);
    if (err != ESP_OK || (status & ST25R3916_IRQ_RF_COLLISION)) {
        write_register(ST25R3916_REG_OPERATION, 0x80);
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }

    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_OPERATION, 0xc9), TAG, "enable RF field");
    s_field_on = true;
    vTaskDelay(pdMS_TO_TICKS(5) + 1); // ISO 14443-A field-on guard time.
    return ESP_OK;
}

esp_err_t st25r3916_transceive(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len,
                               uint32_t timeout_ms)
{
    if (response_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t capacity = *response_len;
    *response_len = 0;
    if (command == NULL || command_len == 0 || command_len > 254 || response == NULL || capacity < 2 ||
        timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_field_on) {
        return ESP_ERR_INVALID_STATE;
    }

    // The caller reserves CRC headroom; the RF frame still cannot exceed 256 bytes.
    size_t raw_len = capacity < 256 ? capacity : 256;
    uint8_t bits;
    bool collision;
    esp_err_t err = exchange_raw(command, command_len * 8, ST25R3916_CMD_TX_CRC, response, &raw_len, &bits, false,
                                 &collision, timeout_ms);
    if (err != ESP_OK) {
        // Keep the ISO-DEP timeout contract; discovery needs the finer distinction.
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_TIMEOUT : err;
    }
    if (bits != 0 || raw_len < 2) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *response_len = raw_len - 2;
    return ESP_OK;
}

static esp_err_t select_cascade(uint8_t level, bool *more)
{
    uint8_t frame[7] = {0x93 + level * 2, 0x20};
    unsigned known_bits = 0;
    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_ISO14443A, 0x01), TAG, "anticollision mode");
    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_AUX, 0x80, 0x80), TAG, "disable RX CRC");

    for (unsigned attempt = 0; attempt < 33; ++attempt) {
        size_t byte = 2 + known_bits / 8;
        unsigned bits = known_bits % 8;
        frame[1] = (byte << 4) | bits;
        uint8_t saved = frame[byte] & ((1U << bits) - 1);
        size_t len = sizeof(frame) - byte;
        uint8_t received_bits;
        bool collision;
        esp_err_t err = exchange_raw(frame, 16 + known_bits, ST25R3916_CMD_TX_NO_CRC, frame + byte, &len,
                                     &received_bits, true, &collision, 5);
        if (err != ESP_OK) {
            return err;
        }

        // The chip aligns a partial-byte reply with the transmitted UID prefix.
        frame[byte] = (frame[byte] & ~((1U << bits) - 1)) | saved;
        if (!collision) {
            if (len != sizeof(frame) - byte || received_bits != 0) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            break;
        }

        uint8_t position;
        ESP_RETURN_ON_ERROR(read_registers(ST25R3916_REG_COLLISION, &position, 1), TAG, "collision position");
        unsigned collision_byte = position >> 4;
        unsigned collision_bit = (position >> 1) & 7;
        if ((position & 1) || collision_byte < 2 || collision_byte > 5) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        unsigned prefix_bits = (collision_byte - 2) * 8 + collision_bit;
        if (prefix_bits < known_bits) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        frame[collision_byte] |= 1U << collision_bit; // Select one deterministic branch.
        known_bits = prefix_bits + 1;
        if (attempt == 32) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if ((frame[2] ^ frame[3] ^ frame[4] ^ frame[5]) != frame[6]) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_ISO14443A, 0), TAG, "normal framing");
    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_AUX, 0x80, 0), TAG, "enable RX CRC");
    frame[1] = 0x70;
    uint8_t sak[3]; // SAK plus CRC.
    size_t len = sizeof(sak);
    esp_err_t err = st25r3916_transceive(frame, sizeof(frame), sak, &len, 5);
    if (err != ESP_OK) {
        return err;
    }
    if (len != 1) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *more = (sak[0] & 0x04) != 0;
    if (*more) {
        return level < 2 && frame[2] == 0x88 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    return (sak[0] & 0x20) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t st25r3916_probe(void)
{
    if (!s_field_on) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(write_register(ST25R3916_REG_ISO14443A, 0), TAG, "normal framing");
    ESP_RETURN_ON_ERROR(modify_register(ST25R3916_REG_AUX, 0x80, 0x80), TAG, "disable RX CRC for ATQA");

    uint8_t atqa[2];
    size_t len = sizeof(atqa);
    uint8_t bits;
    bool collision;
    // WUPA also wakes cards halted by an earlier reader session.
    esp_err_t err = exchange_raw(NULL, 0, ST25R3916_CMD_WUPA, atqa, &len, &bits, true, &collision, 5);
    if (err != ESP_OK) {
        return err;
    }
    if (!collision && (len != sizeof(atqa) || bits != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t st25r3916_select(void)
{
    esp_err_t err = st25r3916_probe();
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t level = 0; level < 3; ++level) {
        bool more;
        err = select_cascade(level, &more);
        if (err != ESP_OK || !more) {
            return err;
        }
    }
    return ESP_ERR_INVALID_RESPONSE;
}
