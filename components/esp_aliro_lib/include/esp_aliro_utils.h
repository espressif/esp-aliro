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
 * @brief Structurally parse a plaintext mdoc DeviceResponse containing one Aliro Access Document.
 *
 * The output borrows the input buffer; keep it unchanged while using the parsed document. This
 * function validates supported CBOR, COSE, and Access Document structure only. It does not verify
 * signatures, issuer trust, digests, time validity, freshness, device binding, or application
 * authorization. Accompanying Revocation Documents are skipped without interpreting or verifying
 * their contents; revocation checking remains unsupported.
 *
 * An absent or empty documents array is accepted. A nonempty array must contain exactly one Access
 * Document and no document types other than Access or Revocation. On every failure, out_document is cleared.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_RESPONSE.
 */
esp_err_t esp_aliro_utils_access_document_parse(const uint8_t *device_response, size_t device_response_size,
                                                esp_aliro_access_document_t *out_document);

/**
 * @brief Encode a canonical one-element Aliro Access Document DeviceRequest.
 *
 * The application supplies the access-system-defined element identifier and whether the User Device
 * may store the request. Pass NULL for out_request to query the required size. On
 * ESP_ERR_INVALID_SIZE, out_request_size receives the required size.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_ERR_INVALID_SIZE.
 */
esp_err_t esp_aliro_utils_access_document_request_encode(const uint8_t *element_identifier,
                                                         size_t element_identifier_size, bool intent_to_store,
                                                         uint8_t *out_request, size_t *out_request_size);

/**
 * @brief Verify an Access Document and one requested element.
 *
 * This function reparses the borrowed document and verifies every returned IssuerSignedItem digest,
 * the IssuerAuth ES256 signature, Credential Issuer identity, session credential binding, document
 * validity time, and AccessIteration. The application remains responsible for selecting the trusted
 * Credential Issuer key, maintaining per-issuer AccessIteration, interpreting the element, and
 * making the authorization decision. After successful verification, advance the stored AccessIteration
 * only when `has_document_access_iteration` is true and `document_access_iteration` exceeds the stored
 * value. An absent or older document iteration must not reset or decrease stored state. Cached documents
 * retain their own authenticated iteration, not the per-issuer high-water mark.
 *
 * If `x5chain` is absent, `trusted_issuer_public_key` authenticates IssuerAuth and `kid` must
 * identify it. If `x5chain` is present, the certificate is authenticated by
 * `trusted_issuer_ca_public_key`, its leaf key authenticates IssuerAuth, and any `kid` must identify
 * that leaf key. Certificate chains and revocation checking are intentionally unsupported.
 *
 * `trusted_current_unix_time_seconds == NULL` represents unavailable trusted time. A document
 * requiring time verification is rejected in that case. On every failure, `out_verified` is cleared.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, or ESP_ERR_INVALID_RESPONSE.
 */
esp_err_t esp_aliro_utils_access_document_verify(const esp_aliro_access_document_t *document,
                                                 const uint8_t *element_identifier, size_t element_identifier_size,
                                                 const esp_aliro_document_verification_params_t *params,
                                                 esp_aliro_verified_access_document_t *out_verified);

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
esp_err_t esp_aliro_utils_get_key_slot_from_cred_pubkey(const char *cred_pubkey_pem, size_t cred_pubkey_len,
                                                        uint8_t *key_slot, size_t *key_slot_len);

/**
 * @brief Convert public key raw data to X.509 PEM format.
 *
 * @param[in] raw_data Public key raw data
 * @param[in] raw_data_len Public key raw-data length in bytes
 * @param[out] pub_key_pem Output X.509 PEM buffer
 * @param[inout] pub_key_pem_len Input: pub_key_pem capacity. Output: PEM length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_SIZE if pub_key_pem is too small
 *      - ESP_FAIL if the public key cannot be parsed
 */
esp_err_t esp_aliro_utils_get_pubkey_pem_from_raw_data(const uint8_t *raw_data, size_t raw_data_len, char *pub_key_pem,
                                                       size_t *pub_key_pem_len);

/**
 * @brief Convert private key raw data to X.509 PEM format.
 *
 * @param[in] raw_data Private key raw data
 * @param[in] raw_data_len Private key raw-data length in bytes
 * @param[out] priv_key_pem Output X.509 PEM buffer
 * @param[inout] priv_key_pem_len Input: priv_key_pem capacity. Output: PEM length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_SIZE if priv_key_pem is too small
 *      - ESP_FAIL if the private key cannot be parsed
 */
esp_err_t esp_aliro_utils_get_privkey_pem_from_raw_data(const uint8_t *raw_data, size_t raw_data_len,
                                                        char *priv_key_pem, size_t *priv_key_pem_len);

#ifdef __cplusplus
}
#endif
