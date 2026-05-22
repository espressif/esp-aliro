/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_aliro_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the Aliro key slot for a credential public key.
 *
 * The key slot is derived from the X.509 PEM-encoded credential public key.
 *
 * @param[in] cred_pubkey_pem Credential public key in X.509 PEM format
 * @param[in] cred_pubkey_len Credential public key length in bytes
 * @param[out] key_slot Output key-slot buffer
 * @param[inout] key_slot_len Input: key_slot capacity. Output: key-slot length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_ALIRO_NO_BUFFER if key_slot is too small
 *      - ESP_FAIL if the credential public key cannot be parsed
 */
esp_err_t esp_aliro_get_key_slot_from_cred_pubkey(const char *cred_pubkey_pem, size_t cred_pubkey_len,
                                                  uint8_t *key_slot, size_t *key_slot_len);

/**
 * @brief Clear fast-transaction persistent storage.
 *
 * This clears stored fast-transaction keys only. It does not clear the reader group sub-identifier. When fast
 * transaction storage is disabled by configuration, this function is a no-op.
 */
void esp_aliro_clear_fast_transaction_storage(void);

#ifdef __cplusplus
}
#endif
