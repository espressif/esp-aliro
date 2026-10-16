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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @brief Synchronous NFC-A/ISO-DEP reader for M5Stack Unit NFC.
 *
 * All API calls must be serialized by one task. The component supports one reader
 * and owns the configured I2C bus for the application lifetime.
 */

/**
 * @brief Initialize the reader and its I2C bus.
 *
 * Call once before other operations. Enable external-port power first if required
 * by the controller. Initialization leaves the RF field disabled.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the reader is already initialized
 *      - ESP_ERR_NOT_SUPPORTED if the IC identity is unsupported
 *      - Other esp_err_t values from I2C, timer or hardware initialization
 */
esp_err_t m5nfc_init(void);

/**
 * @brief Poll for removal after a completed or failed round.
 *
 * May enable the RF field to probe presence without activating ISO-DEP. At least
 * 200 ms of consecutive RF absence re-arms the next tap. Bus errors do not count
 * as removal. No-op outside the removal wait; also called by m5nfc_activate().
 */
void m5nfc_update(void);

/**
 * @brief Select an NFC-A peer and activate ISO-DEP.
 *
 * Requires successful initialization. An already active session is unchanged.
 * After a round ends, activation remains blocked until removal is detected.
 *
 * @return true if a session is active; false if uninitialized, no compatible
 *         peer responds, removal is pending, or activation fails.
 */
bool m5nfc_activate(void);

/**
 * @brief End the active round and disable the RF field.
 *
 * Ends successful and failed rounds alike. Subsequent polling may enable RF to
 * detect removal; another round requires removal and re-presentation. Repeated
 * calls during the removal wait are no-ops. RF-disable errors are logged.
 */
void m5nfc_deactivate(void);

/**
 * @brief Exchange one APDU over an active ISO-DEP session.
 *
 * Performs ISO-DEP fragmentation and reassembly, never whole-APDU replay.
 * Transport or protocol failure ends the round and requires removal/re-tap.
 * Invalid arguments do not end an active session. Discard partial response data
 * on error.
 *
 * @param[in] command Command APDU bytes.
 * @param[in] command_len Command length in bytes; must be nonzero.
 * @param[out] response Caller-owned response buffer.
 * @param[inout] response_len Input: nonzero response capacity. Output: received
 *                           length in bytes on success, zero on error.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if a pointer is NULL or a length is zero
 *      - ESP_ERR_INVALID_STATE if no session is active
 *      - ESP_ERR_INVALID_SIZE if a response exceeds buffer or frame capacity
 *      - ESP_ERR_TIMEOUT if frame waiting fails after recovery, or the 60-second deadline expires
 *      - ESP_ERR_INVALID_RESPONSE or ESP_ERR_INVALID_CRC for protocol/RF errors
 *      - Other esp_err_t values from I2C or timer operations
 */
esp_err_t m5nfc_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response, size_t *response_len);

#ifdef __cplusplus
}
#endif
