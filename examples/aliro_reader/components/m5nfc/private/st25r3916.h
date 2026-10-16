/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Private, single-owner driver for the fixed M5Stack Unit NFC I2C connection.
esp_err_t st25r3916_init(void);
esp_err_t st25r3916_field(bool enabled);

// WUPA presence check only; does not select or activate ISO-DEP.
// ESP_ERR_NOT_FOUND means the chip signaled RF no-response; bus errors are unchanged.
esp_err_t st25r3916_probe(void);
esp_err_t st25r3916_select(void);

// CRC is generated/checked by hardware. Command and returned length exclude CRC.
// On entry *response_len is buffer capacity INCLUDING two bytes of CRC headroom.
// On failure it is zero; response contents are unspecified.
esp_err_t st25r3916_transceive(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len,
                               uint32_t timeout_ms);
