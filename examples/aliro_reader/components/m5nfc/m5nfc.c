/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "m5nfc.h"
#include "iso_dep.h"
#include "st25r3916.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

static const char *TAG = "m5nfc";

typedef enum {
    READER_UNINITIALIZED,
    READER_READY,
    READER_ACTIVE,
    READER_WAIT_REMOVAL,
} reader_phase_t;

static reader_phase_t s_phase;
static iso_dep_session_t s_session;
static int64_t s_next_probe_us;
static int64_t s_absent_since_us;

esp_err_t m5nfc_init(void)
{
    esp_err_t err = st25r3916_init();
    if (err == ESP_OK) {
        s_phase = READER_READY;
    }
    return err;
}

void m5nfc_update(void)
{
    if (s_phase != READER_WAIT_REMOVAL || esp_timer_get_time() < s_next_probe_us) {
        return;
    }

    esp_err_t err = st25r3916_field(true);
    if (err != ESP_OK) {
        s_absent_since_us = 0;
        s_next_probe_us = esp_timer_get_time() + 100000;
        return;
    }

    // WUPA only: do not issue RATS or start another application session.
    err = st25r3916_probe();
    int64_t now_us = esp_timer_get_time();
    s_next_probe_us = now_us + 100000;

    if (err != ESP_ERR_NOT_FOUND) {
        // A reply, collision, or uncertain bus/RF error is not proof of removal.
        s_absent_since_us = 0;
        return;
    }
    if (s_absent_since_us == 0) {
        s_absent_since_us = now_us;
        return;
    }

    // Debounce RF absence: a single missed reply must not authorize a second round.
    if (now_us - s_absent_since_us >= 200000) {
        s_phase = READER_READY;
        m5nfc_deactivate();
        ESP_LOGI(TAG, "Device removed; ready for next tap");
    }
}

bool m5nfc_activate(void)
{
    if (s_phase == READER_ACTIVE) {
        return true;
    }
    if (s_phase == READER_WAIT_REMOVAL) {
        m5nfc_update();
        return false;
    }
    if (s_phase != READER_READY) {
        return false;
    }

    esp_err_t err = st25r3916_field(true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RF field activation failed: %s", esp_err_to_name(err));
        m5nfc_deactivate();
        return false;
    }

    err = st25r3916_select();
    if (err != ESP_OK) {
        // No WUPA response is the normal no-card case, not a warning.
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "NFC-A selection failed: %s", esp_err_to_name(err));
        }
        m5nfc_deactivate();
        return false;
    }

    err = iso_dep_activate(&s_session, CONFIG_ST25R3916_TRANSCEIVE_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ISO-DEP activation failed: %s", esp_err_to_name(err));
        m5nfc_deactivate();
        return false;
    }
    if (s_session.sfgt_ms != 0) {
        vTaskDelay(pdMS_TO_TICKS(s_session.sfgt_ms) + 1);
    }

    s_phase = READER_ACTIVE;
    ESP_LOGI(TAG, "ISO-DEP activated");
    return true;
}

void m5nfc_deactivate(void)
{
    if (s_phase == READER_UNINITIALIZED || s_phase == READER_WAIT_REMOVAL) {
        return;
    }

    if (s_phase == READER_ACTIVE) {
        s_phase = READER_WAIT_REMOVAL;
        s_next_probe_us = 0;
        s_absent_since_us = 0;
    }
    s_session.active = false;

    esp_err_t err = st25r3916_field(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to disable RF field: %s", esp_err_to_name(err));
    }
}

esp_err_t m5nfc_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len)
{
    esp_err_t err = iso_dep_exchange(&s_session, command, command_len, response, response_len);
    if (err != ESP_OK && !s_session.active) {
        m5nfc_deactivate();
    }
    return err;
}
