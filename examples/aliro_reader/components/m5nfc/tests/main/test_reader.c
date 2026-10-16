/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "m5nfc.h"
#include "st25r3916.h"

#include <driver/i2c_master.h>
#include <esp_idf_version.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <unity.h>

// Only the external chip is modeled; ESP-IDF timers, semaphores and scheduling
// run unmodified on the single device. C0/C1 reset chip configuration; C2/C3
// preserve it.
// TX end, RX start/end and NRT expiration are separate hardware events.
static uint8_t s_registers[2][64];
static uint8_t s_tx_fifo[256];
static uint8_t s_rx_fifo[256];
static size_t s_tx_len;
static size_t s_rx_len;
static size_t s_pending_rx_len;

static bool s_calibrated;
static bool s_tx_pending;
static bool s_rx_start_pending;
static bool s_rx_end_pending;
static bool s_timeout_pending;

static int64_t s_tx_end_us;
static int64_t s_rx_start_us;
static int64_t s_rx_end_us;
static int64_t s_timeout_us;
static int64_t s_last_rx_end_us;
static int64_t s_calibration_until_us;

typedef struct {
    uint8_t command;
    uint8_t tx[254];
    size_t tx_len;
    uint8_t rx[256]; // CRC bytes included for hardware-checked frames.
    size_t rx_len;
    uint32_t wait_us;
    uint32_t reply_within_us;
    esp_err_t bus_error;
} rf_frame_t;

static rf_frame_t s_frames[40];
static size_t s_frame_count;
static size_t s_frame_index;

static rf_frame_t *queue_frame(rf_frame_t frame)
{
    TEST_ASSERT(s_frame_count < sizeof(s_frames) / sizeof(s_frames[0]));
    s_frames[s_frame_count] = frame;
    return &s_frames[s_frame_count++];
}

static void queue_tap(void)
{
    // Keep the large transcript in flash rather than on the test task stack.
    static const rf_frame_t tap[] = {
        {.command = 0xc7, .rx = {0x04, 0x00}, .rx_len = 2},
        {.command = 0xc5, .tx = {0x93, 0x20}, .tx_len = 2, .rx = {1, 2, 3, 4, 4}, .rx_len = 5},
        {.command = 0xc4, .tx = {0x93, 0x70, 1, 2, 3, 4, 4}, .tx_len = 7, .rx = {0x20, 0, 0}, .rx_len = 3},
        {.command = 0xc4, .tx = {0xe0, 0x80}, .tx_len = 2, .rx = {2, 8, 0, 0}, .rx_len = 4},
        {.command = 0xc4,
         .tx = {0x02, 0x00, 0xa4, 0x04, 0x00},
         .tx_len = 5,
         .rx = {0xf2, 2, 0, 0},
         .rx_len = 4,
         .wait_us = 3500},
        // The peer needs a WTX reply within its nominal 4.83 ms FWT; the
        // configured receive-timeout floor does not extend that deadline.
        {.command = 0xc4,
         .tx = {0xf2, 2},
         .tx_len = 2,
         .rx = {0x02, 0x90, 0x00, 0, 0},
         .rx_len = 5,
         .reply_within_us = 4800},
    };

    for (size_t i = 0; i < sizeof(tap) / sizeof(tap[0]); ++i) {
        queue_frame(tap[i]);
    }
}

static void queue_presence(bool present)
{
    queue_frame((rf_frame_t){.command = 0xc7, .rx = {0x04, 0x00}, .rx_len = present ? 2 : 0});
}

esp_err_t __wrap_i2c_new_master_bus(const i2c_master_bus_config_t *config, i2c_master_bus_handle_t *bus)
{
    TEST_ASSERT(config != NULL);
    *bus = (i2c_master_bus_handle_t)s_registers;
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                           i2c_master_dev_handle_t *device)
{
    TEST_ASSERT(bus != NULL && config->device_address == 0x50 && config->scl_speed_hz == 400000);
    *device = (i2c_master_dev_handle_t)s_registers;
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{
    TEST_ASSERT(device != NULL);
    return ESP_OK;
}

esp_err_t __wrap_i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    TEST_ASSERT(bus != NULL);
    return ESP_OK;
}

static void clear_transfer(void)
{
    s_tx_len = s_rx_len = 0;
    s_tx_pending = s_rx_start_pending = s_rx_end_pending = s_timeout_pending = false;
    memset(s_registers[0] + 0x1a, 0, 4);
}

static void reset_chip(void)
{
    memset(s_registers, 0, sizeof(s_registers));
    s_registers[0][0x3f] = 0x28;
    s_calibrated = false;
    clear_transfer();
}

static void advance_bus(size_t bytes)
{
    // 400 kHz, address + nine clocks per byte. Let events occur during I2C.
    esp_rom_delay_us((uint32_t)(((bytes + 1) * 9 * 1000000 + 399999) / 400000));

    if (s_tx_pending && esp_timer_get_time() >= s_tx_end_us) {
        s_registers[0][0x1a] |= 0x08;
        s_tx_pending = false;
    }

    if (s_rx_start_pending && esp_timer_get_time() >= s_rx_start_us) {
        s_registers[0][0x1a] |= 0x20;
        s_rx_start_pending = false;
    }

    if (s_rx_end_pending && esp_timer_get_time() >= s_rx_end_us) {
        s_registers[0][0x1a] |= 0x10;
        s_rx_end_pending = false;
        s_rx_len = s_pending_rx_len;
        s_last_rx_end_us = s_rx_end_us;
    }

    if (s_timeout_pending && esp_timer_get_time() >= s_timeout_us) {
        s_registers[0][0x1b] |= 0x40;
        s_timeout_pending = false;
    }
}

static esp_err_t start_transmission(uint8_t command)
{
    if ((s_registers[0][0x02] & 0xc8) != 0xc8 || !s_calibrated) {
        return ESP_OK; // A reset chip cannot transmit.
    }

    TEST_ASSERT(s_frame_index < s_frame_count);
    const rf_frame_t *frame = &s_frames[s_frame_index++];
    TEST_ASSERT(command == frame->command);
    TEST_ASSERT(s_registers[0][0x03] == 0x08 && s_registers[0][0x04] == 0);
    if (frame->bus_error != ESP_OK) {
        return frame->bus_error;
    }
    if (frame->reply_within_us) {
        TEST_ASSERT(esp_timer_get_time() - s_last_rx_end_us <= frame->reply_within_us);
    }

    size_t tx_bits = 7;
    if (command != 0xc7) {
        tx_bits = ((size_t)s_registers[0][0x22] << 8) | s_registers[0][0x23];
        TEST_ASSERT(tx_bits == frame->tx_len * 8 && s_tx_len == frame->tx_len);
        TEST_ASSERT(memcmp(s_tx_fifo, frame->tx, frame->tx_len) == 0);
        tx_bits += frame->tx_len + (command == 0xc4 ? 18 : 0);
    }

    s_tx_end_us = esp_timer_get_time() + (tx_bits * 1000000 + 105999) / 106000;
    s_tx_pending = true;

    uint32_t ticks = ((uint32_t)s_registers[0][0x10] << 8) | s_registers[0][0x11];
    uint32_t step = s_registers[0][0x12] & 1 ? 4096 : 64;
    uint32_t fwt_us = (uint32_t)(((uint64_t)ticks * step * 1000000 + 13559999) / 13560000);
    uint32_t wait_us = frame->wait_us ? frame->wait_us : 2000;

    s_rx_start_us = s_tx_end_us + wait_us;
    s_rx_end_us = s_rx_start_us + (frame->rx_len * 9 * 1000000 + 105999) / 106000;
    bool responds = frame->rx_len && (!ticks || wait_us < fwt_us);
    s_rx_start_pending = s_rx_end_pending = responds;
    s_timeout_pending = !responds && ticks;
    s_timeout_us = s_tx_end_us + fwt_us;
    s_pending_rx_len = frame->rx_len;
    memcpy(s_rx_fifo, frame->rx, frame->rx_len);
    return ESP_OK;
}

esp_err_t __wrap_i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *data, size_t len, int timeout_ms)
{
    TEST_ASSERT(device != NULL && len > 0 && timeout_ms > 0 && esp_timer_get_time() >= s_calibration_until_us);
    advance_bus(len);

    if (len == 1 && data[0] >= 0xc0) {
        switch (data[0]) {
        case 0xc0:
        case 0xc1:
            reset_chip();
            break;
        case 0xc2:
        case 0xc3:
            clear_transfer();
            break;
        case 0xc8:
            TEST_ASSERT(s_registers[0][0x02] & 0x80);
            s_registers[0][0x02] |= 0x08;
            s_registers[0][0x1d] |= 0x20;
            break;
        case 0xd6:
            s_calibrated = true;
            s_calibration_until_us = esp_timer_get_time() + 5000;
            s_registers[0][0x1b] |= 0x80;
            break;
        case 0xd5:
            break;
        case 0xc4:
        case 0xc5:
        case 0xc7:
            return start_transmission(data[0]);
        default:
            TEST_ASSERT(false);
        }
        return ESP_OK;
    }

    if (data[0] == 0x80) {
        TEST_ASSERT(len - 1 <= sizeof(s_tx_fifo));
        s_tx_len = len - 1;
        memcpy(s_tx_fifo, data + 1, s_tx_len);
        return ESP_OK;
    }

    size_t offset = data[0] == 0xfb ? 1 : 0;
    size_t bank = offset ? 1 : 0;
    uint8_t reg = data[offset];
    TEST_ASSERT(reg < 64 && offset + 2 == len);
    if (bank == 0 && reg == 0x02 && !(s_registers[0][reg] & 0x80) && (data[offset + 1] & 0x80)) {
        s_registers[0][0x1a] |= 0x80;
    }
    s_registers[bank][reg] = data[offset + 1];
    return ESP_OK;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
esp_err_t __wrap_i2c_master_multi_buffer_transmit(i2c_master_dev_handle_t device,
                                                  i2c_master_transmit_multi_buffer_info_t *buffers, size_t buffer_count,
                                                  int timeout_ms)
{
    // The chip observes one transaction containing the concatenated buffers.
    uint8_t transaction[255];
    size_t len = 0;
    for (size_t i = 0; i < buffer_count; ++i) {
        TEST_ASSERT(buffers[i].write_buffer != NULL && buffers[i].buffer_size <= sizeof(transaction) - len);
        memcpy(transaction + len, buffers[i].write_buffer, buffers[i].buffer_size);
        len += buffers[i].buffer_size;
    }
    return __wrap_i2c_master_transmit(device, transaction, len, timeout_ms);
}
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)

esp_err_t __wrap_i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *tx, size_t tx_len,
                                             uint8_t *rx, size_t rx_len, int timeout_ms)
{
    TEST_ASSERT(device != NULL && tx_len > 0 && timeout_ms > 0 && esp_timer_get_time() >= s_calibration_until_us);
    advance_bus(tx_len + rx_len + 1);

    if (tx[0] == 0x9f) {
        TEST_ASSERT(tx_len == 1 && rx_len == s_rx_len);
        memcpy(rx, s_rx_fifo, rx_len);
        s_rx_len = 0;
        return ESP_OK;
    }

    size_t offset = tx[0] == 0xfb ? 1 : 0;
    size_t bank = offset ? 1 : 0;
    size_t reg = tx[offset] & 0x3f;
    TEST_ASSERT(tx_len == offset + 1 && (tx[offset] & 0x40) && reg + rx_len <= 64);
    s_registers[0][0x1e] = s_rx_len & 0xff;
    s_registers[0][0x1f] = (s_rx_len >> 8) << 6;
    memcpy(rx, s_registers[bank] + reg, rx_len);
    if (bank == 0 && reg == 0x1a && rx_len == 4) {
        memset(s_registers[0] + 0x1a, 0, 4);
    }
    return ESP_OK;
}

static void exchange_apdu(void)
{
    const uint8_t command[] = {0x00, 0xa4, 0x04, 0x00};
    uint8_t response[2];
    size_t len = sizeof(response);

    TEST_ASSERT(m5nfc_message_exchange(command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == 2 && response[0] == 0x90 && response[1] == 0x00);
    TEST_ASSERT((s_registers[0][0x02] & 0xc8) == 0xc8 && s_calibrated);
}

static void held_poll(void)
{
    vTaskDelay(pdMS_TO_TICKS(110) + 1);
    m5nfc_update();
    TEST_ASSERT(!m5nfc_activate());
}

TEST_CASE("Reader preserves RF setup, meets WTX timing and gates held devices", "[reader]")
{
    queue_tap();
    rf_frame_t *late = queue_frame((rf_frame_t){
        .command = 0xc4, .tx = {0x03, 0x00, 0xca, 0x00, 0x00}, .tx_len = 5, .rx_len = 256, .wait_us = 99000});
    late->rx[0] = 0x03;
    late->rx[252] = 0x90;
    late->rx[254] = 0x12;
    late->rx[255] = 0x34;

    rf_frame_t *max_tx =
        queue_frame((rf_frame_t){.command = 0xc4, .tx_len = 254, .rx = {0x02, 0x90, 0x00, 0x12, 0x34}, .rx_len = 5});
    max_tx->tx[0] = 0x02;
    for (size_t i = 1; i < max_tx->tx_len; ++i) {
        max_tx->tx[i] = i;
    }

    static const struct {
        size_t capacity;
        size_t wire_len;
        esp_err_t result;
    } receive_cases[] = {
        {4, 4, ESP_OK},
        {3, 4, ESP_ERR_INVALID_SIZE},
        {4, 1, ESP_ERR_INVALID_RESPONSE},
        {2, 2, ESP_OK},
    };
    for (size_t i = 0; i < sizeof(receive_cases) / sizeof(receive_cases[0]); ++i) {
        queue_frame((rf_frame_t){.command = 0xc4,
                                 .tx = {0x02, 0x00},
                                 .tx_len = 2,
                                 .rx = {0x12, 0x34, 0x56, 0x78},
                                 .rx_len = receive_cases[i].wire_len});
    }

    for (unsigned i = 0; i < 3; ++i) {
        queue_presence(true);
    }

    queue_presence(false); // One missed reply is not removal.
    queue_presence(true);
    queue_frame((rf_frame_t){.command = 0xc7, .bus_error = ESP_FAIL}); // Nor is an I2C failure.

    // Bus timeouts spanning the removal debounce must reset, not extend, RF absence.
    queue_presence(false);
    for (unsigned i = 0; i < 3; ++i) {
        queue_frame((rf_frame_t){.command = 0xc7, .bus_error = ESP_ERR_TIMEOUT});
    }
    queue_presence(false);
    queue_presence(true);

    queue_presence(false);
    queue_presence(false);
    queue_presence(false);

    queue_tap();
    queue_frame((rf_frame_t){.command = 0xc4, .tx = {0x03, 0x00, 0xca, 0x00, 0x00}, .tx_len = 5});
    queue_frame((rf_frame_t){.command = 0xc4, .tx = {0xb3}, .tx_len = 1});
    queue_frame((rf_frame_t){.command = 0xc4, .tx = {0xb3}, .tx_len = 1});
    queue_presence(true);

    TEST_ASSERT(m5nfc_init() == ESP_OK);
    TEST_ASSERT(m5nfc_activate());
    exchange_apdu();

    // A 256-byte response starts before the 100 ms FWT but ends after it.
    const uint8_t command[] = {0x00, 0xca, 0x00, 0x00};
    uint8_t response[253];
    size_t len = sizeof(response);
    TEST_ASSERT(m5nfc_message_exchange(command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == 253 && response[251] == 0x90 && response[252] == 0x00);

    uint8_t large_command[253];
    for (size_t i = 0; i < sizeof(large_command); ++i) {
        large_command[i] = i + 1;
    }
    len = sizeof(response);
    TEST_ASSERT(m5nfc_message_exchange(large_command, sizeof(large_command), response, &len) == ESP_OK);
    TEST_ASSERT(len == 2 && response[0] == 0x90 && response[1] == 0x00);

    const uint8_t raw_command[] = {0x02, 0x00};
    for (size_t i = 0; i < sizeof(receive_cases) / sizeof(receive_cases[0]); ++i) {
        uint8_t guarded[6];
        memset(guarded, 0xa5, sizeof(guarded));
        size_t capacity = receive_cases[i].capacity;
        len = capacity;
        TEST_ASSERT(st25r3916_transceive(raw_command, sizeof(raw_command), guarded + 1, &len, 100) ==
                    receive_cases[i].result);
        TEST_ASSERT(guarded[0] == 0xa5 && guarded[capacity + 1] == 0xa5);
        if (receive_cases[i].result == ESP_OK) {
            TEST_ASSERT(len == receive_cases[i].wire_len - 2);
            TEST_ASSERT(guarded[1] == 0x12 && guarded[2] == 0x34);
        } else {
            TEST_ASSERT(len == 0);
        }
    }

    m5nfc_deactivate();
    m5nfc_deactivate(); // Caller cleanup after an already-closed round is idempotent.
    TEST_ASSERT(!(s_registers[0][0x02] & 0x08));
    TEST_ASSERT(!m5nfc_activate());

    for (unsigned i = 0; i < 13; ++i) {
        held_poll();
    }

    vTaskDelay(pdMS_TO_TICKS(110) + 1);
    m5nfc_update(); // Third consecutive absence: arm the next presentation.

    TEST_ASSERT(m5nfc_activate());
    exchange_apdu();

    len = sizeof(response);
    TEST_ASSERT(m5nfc_message_exchange(command, sizeof(command), response, &len) == ESP_ERR_TIMEOUT);
    TEST_ASSERT(len == 0);
    m5nfc_deactivate();
    held_poll(); // A failed round also cannot retry while the device stays present.
    TEST_ASSERT(s_frame_index == s_frame_count);
}
