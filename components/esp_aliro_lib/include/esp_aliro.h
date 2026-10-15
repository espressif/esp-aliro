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
 * @file
 * @brief Authentication and document-access flow (pseudocode).
 *
 * @code
 * initialize SDK; create/configure/enable reader; create session
 *
 * check(run_expedited(session, exchange, &identity))
 *   fast match: AUTH0 -> authenticated identity
 *   otherwise: AUTH0 -> optional LOAD CERT -> AUTH1 -> authenticated identity
 *
 * if application_needs_documents(identity):
 *   check(continue_standard(session, exchange, &identity))
 *     fast: optional LOAD CERT -> AUTH1, without restarting AUTH0
 *     standard: return current authenticated identity, without I/O
 *   check(run_envelope(session, exchange, request, &response))
 *     optional step-up SELECT -> ENVELOPE / GET RESPONSE
 *   check(verify_required_documents(response, identity))
 *
 * authorize identity and any required documents
 * if authorized: check(commit_fast_transaction(session))
 * report the access outcome using EXCHANGE; delete session
 * @endcode
 */

typedef esp_err_t (*esp_aliro_lookup_credential_pubkey_cb_t)(const uint8_t *key_slot, size_t key_slot_len,
                                                             char *out_pubkey, size_t *out_pubkey_len);

typedef esp_err_t (*esp_aliro_message_exchange_cb_t)(const uint8_t *command, size_t command_len, uint8_t *response,
                                                     size_t *response_len);

/**
 * @brief Initialize the Aliro SDK.
 *
 * This function must be called before creating a reader. The SDK supports one active reader at a time. Calling this
 * while a reader exists returns ESP_ERR_INVALID_STATE.
 *
 * @param[in] cfg Optional SDK configuration. NULL uses defaults.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the configuration is invalid
 *      - ESP_ERR_INVALID_STATE if a reader already exists
 *      - Other esp_err_t values from storage initialization
 */
esp_err_t esp_aliro_init(const esp_aliro_config_t *cfg);

/**
 * @brief Create an Aliro reader handle.
 *
 * The reader is created disabled. Configure optional certificate, vendor extension, and key-slot lookup before calling
 * esp_aliro_reader_enable(). Only one reader can exist at a time.
 *
 * @param[out] handle Reader handle returned on success
 * @param[in] cfg Reader configuration
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid or the SDK is not initialized
 *      - ESP_ERR_INVALID_STATE if a reader already exists
 *      - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t esp_aliro_reader_create(esp_aliro_reader_handle_t *handle, const esp_aliro_reader_config_t *cfg);

/**
 * @brief Delete an Aliro reader handle.
 *
 * If the reader is enabled, it is disabled first. Deletion fails while sessions are active.
 *
 * @param[inout] handle Reader handle. Cleared to 0 on success.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if handle is invalid
 *      - ESP_ERR_INVALID_STATE if sessions are active
 *      - Other esp_err_t values from reader deinitialization
 */
esp_err_t esp_aliro_reader_delete(esp_aliro_reader_handle_t *handle);

/**
 * @brief Configure the optional reader certificate.
 *
 * This must be called before the reader is enabled. Passing ESP_ALIRO_CERT_POLICY_NONE clears any configured
 * certificate.
 *
 * @param[in] handle Reader handle
 * @param[in] cfg Certificate configuration
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the reader is enabled
 *      - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t esp_aliro_reader_set_cert(esp_aliro_reader_handle_t handle, const esp_aliro_reader_cert_config_t *cfg);

/**
 * @brief Configure the optional reader vendor extension.
 *
 * This must be called before the reader is enabled. Passing NULL with length 0 clears the vendor extension.
 *
 * @param[in] handle Reader handle
 * @param[in] vendor_ext Vendor extension data, or NULL when vendor_ext_len is 0
 * @param[in] vendor_ext_len Vendor extension length in bytes. Must not exceed ESP_ALIRO_READER_VENDOR_EXT_MAX_LEN.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the reader is enabled
 *      - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t esp_aliro_reader_set_vendor_ext(esp_aliro_reader_handle_t handle, const uint8_t *vendor_ext,
                                          size_t vendor_ext_len);

/**
 * @brief Configure the optional Reader Descriptor sent once per transaction.
 *
 * This must be called before the reader is enabled. The descriptor is copied into reader-owned storage. Passing NULL
 * clears the configured descriptor. Product ID and firmware version must be non-empty, and the complete DER-TLV
 * descriptor must fit both EXCHANGE Notify and CONTROL FLOW command limits.
 *
 * @param[in] handle Reader handle
 * @param[in] descriptor Reader Descriptor configuration, or NULL to clear it
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if an argument or counted pointer is invalid, or a mandatory field is empty
 *      - ESP_ERR_INVALID_SIZE if the encoded descriptor exceeds a protocol command limit
 *      - ESP_ERR_INVALID_STATE if the reader is enabled
 *      - ESP_ERR_NO_MEM if allocation fails
 */
esp_err_t esp_aliro_reader_set_descriptor(esp_aliro_reader_handle_t handle,
                                          const esp_aliro_reader_descriptor_t *descriptor);

/**
 * @brief Attach fast-transaction storage to a disabled Reader.
 *
 * The callback table and context value are copied. The pointed-to context remains caller-owned and must stay valid
 * until the Reader is deleted. All callback invocations for this Reader are serialized. A Reader without a delegate
 * does not attempt fast transactions. The delegate must not call Reader storage APIs recursively.
 *
 * @param[in] handle Reader handle
 * @param[in] delegate Complete storage callback table
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the handle or callback table is invalid
 *      - ESP_ERR_INVALID_STATE if the Reader is enabled
 *      - ESP_ERR_NO_MEM if synchronization allocation fails
 */
esp_err_t esp_aliro_reader_set_fast_transaction_storage_delegate(
    esp_aliro_reader_handle_t handle, const esp_aliro_fast_transaction_storage_delegate_t *delegate);

/**
 * @brief Clear the Reader's fast-transaction storage.
 *
 * This is an ESP_OK no-op when no delegate is attached. The delegate remains attached. Active Sessions are not
 * cancelled; an in-flight fast authentication may finish after this call and still requires application approval and
 * an explicit commit.
 *
 * @param[in] handle Reader handle
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an invalid handle, or the delegate error.
 */
esp_err_t esp_aliro_reader_clear_fast_transaction_storage(esp_aliro_reader_handle_t handle);

/**
 * @brief Enable credential key-slot lookup for the reader.
 *
 * This must be called before the reader is enabled.
 *
 * @param[in] handle Reader handle
 * @param[in] lookup_credential_pubkey_cb Callback used to resolve a credential public key from a key slot
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the reader is enabled
 */
esp_err_t esp_aliro_reader_enable_key_slot(esp_aliro_reader_handle_t handle,
                                           esp_aliro_lookup_credential_pubkey_cb_t lookup_credential_pubkey_cb);

/**
 * @brief Enable the reader.
 *
 * Enabling loads or creates the persisted reader group sub-identifier. Calling this on an already enabled Reader is a
 * no-op.
 *
 * @param[in] handle Reader handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if handle is invalid
 *      - Other esp_err_t values from storage or reader initialization
 */
esp_err_t esp_aliro_reader_enable(esp_aliro_reader_handle_t handle);

/**
 * @brief Disable the reader.
 *
 * Disabling fails while sessions are active. Calling this on an already disabled reader is a no-op.
 *
 * @param[in] handle Reader handle
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if handle is invalid
 *      - ESP_ERR_INVALID_STATE if sessions are active
 *      - Other esp_err_t values from reader deinitialization
 */
esp_err_t esp_aliro_reader_disable(esp_aliro_reader_handle_t handle);

/**
 * @brief Get the reader group identifier.
 *
 * The group_identifier buffer must be at least 16 bytes.
 *
 * @param[in] handle Reader handle
 * @param[out] group_identifier Reader group identifier buffer
 * @param[inout] group_identifier_len Input: group_identifier capacity. Output: identifier length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_SIZE if group_identifier is too small
 */
esp_err_t esp_aliro_reader_get_group_identifier(esp_aliro_reader_handle_t handle, uint8_t *group_identifier,
                                                size_t *group_identifier_len);

/**
 * @brief Get the reader group sub-identifier.
 *
 * The group_sub_identifier buffer must be at least 16 bytes.
 *
 * @param[in] handle Reader handle
 * @param[out] group_sub_identifier Reader group sub-identifier buffer
 * @param[inout] group_sub_identifier_len Input: group_sub_identifier capacity. Output: sub-identifier length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_SIZE if group_sub_identifier is too small
 *      - ESP_ERR_INVALID_STATE if reader is not enabled
 *      - Other esp_err_t values from storage initialization
 */
esp_err_t esp_aliro_reader_get_group_sub_identifier(esp_aliro_reader_handle_t handle, uint8_t *group_sub_identifier,
                                                    size_t *group_sub_identifier_len);

/**
 * @brief Get the reader public key raw data.
 *
 * The public_key_raw_data buffer must be at least 65 bytes.
 *
 * @param[in] handle Reader handle
 * @param[out] public_key_raw_data Reader public key raw-data buffer
 * @param[inout] public_key_raw_data_len Input: public_key_raw_data capacity. Output: raw-data length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_SIZE if public_key_raw_data is too small
 */
esp_err_t esp_aliro_reader_get_public_key_raw_data(esp_aliro_reader_handle_t handle, uint8_t *public_key_raw_data,
                                                   size_t *public_key_raw_data_len);

/**
 * @brief Create and initialize an Aliro transaction session.
 *
 * The reader must be enabled before creating a session.
 *
 * @param[in] reader_handle Enabled reader handle
 * @param[out] session_handle Session handle returned on success
 * @param[in] cfg Session configuration
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the reader is disabled
 *      - ESP_ERR_NO_MEM if allocation fails
 *      - Other esp_err_t values from session initialization
 */
esp_err_t esp_aliro_session_create(esp_aliro_reader_handle_t reader_handle, esp_aliro_session_handle_t *session_handle,
                                   const esp_aliro_session_config_t *cfg);

/**
 * @brief Delete an Aliro transaction session.
 *
 * @param[inout] session_handle Session handle. Cleared to 0 on success.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if session_handle is invalid
 *      - Other esp_err_t values from session deinitialization
 */
esp_err_t esp_aliro_session_delete(esp_aliro_session_handle_t *session_handle);

/**
 * @brief Get the active transaction type for a session.
 *
 * @param[in] handle Session handle
 * @param[out] out_type Active transaction type
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the active transaction type is unavailable
 */
esp_err_t esp_aliro_session_get_transaction_type(esp_aliro_session_handle_t handle,
                                                 esp_aliro_transaction_type_t *out_type);

/**
 * @brief Run the expedited transaction phase.
 *
 * Successful fast authentication may continue standard in the same transaction
 * through esp_aliro_session_continue_standard().
 *
 * @param[in] handle Session handle
 * @param[in] msg_exchange_cb Callback used to exchange APDU messages with the user device
 * @param[out] out_result Authenticated credential and optional signed timestamp; cleared before processing and on
 * failure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - Other esp_err_t values from transaction processing or transport
 */
esp_err_t esp_aliro_session_run_expedited(esp_aliro_session_handle_t handle,
                                          esp_aliro_message_exchange_cb_t msg_exchange_cb,
                                          esp_aliro_expedited_result_t *out_result);

/**
 * @brief Continue fast authentication through the standard phase in the same transaction.
 *
 * Call after expedited authentication, before EXCHANGE, ENVELOPE, or commit.
 * Already-standard sessions return their current result without I/O.
 *
 * Fast sessions reuse AUTH0 and perform optional LOAD CERT and AUTH1 for the same credential. Success
 * replaces the result (clearing any omitted timestamp) and prepares a new storage entry for explicit
 * commit. Authentication or transport failure invalidates the session; do not authorize using the
 * earlier fast result.
 *
 * @param[in] handle Session handle
 * @param[in] msg_exchange_cb Callback used to exchange APDU messages with the user device
 * @param[out] out_result Standard-authenticated result; cleared before processing and on failure
 *
 * @return
 *      - ESP_OK on success, including an already-standard session
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE before authentication, after traffic/commit, or while busy
 *      - Other esp_err_t values from authentication or transport
 */
esp_err_t esp_aliro_session_continue_standard(esp_aliro_session_handle_t handle,
                                              esp_aliro_message_exchange_cb_t msg_exchange_cb,
                                              esp_aliro_expedited_result_t *out_result);

/**
 * @brief Apply the authenticated Session's pending fast-transaction storage mutation.
 *
 * Call this exactly once after successful expedited authentication and application authorization. Standard
 * transactions store the newly derived entry; fast transactions touch the matched entry. Deleting the Session without
 * committing discards the pending entry. A Reader without a delegate performs a successful no-op commit.
 *
 * @param[in] handle Session handle
 *
 * @return
 *      - ESP_OK when committed, including when no storage delegate is attached
 *      - ESP_ERR_INVALID_ARG if handle is invalid
 *      - ESP_ERR_INVALID_STATE before successful authentication or after a prior commit
 *      - Other esp_err_t values returned by the delegate
 */
esp_err_t esp_aliro_session_commit_fast_transaction(esp_aliro_session_handle_t handle);

/**
 * @brief Run an exchange transaction without mailbox requests.
 *
 * @param[in] handle Session handle
 * @param[in] msg_exchange_cb Callback used to exchange APDU messages with the user device
 * @param[in] crypto_engine_type Crypto engine to use for the exchange
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the session is not ready for exchange
 *      - Other esp_err_t values from transaction processing or transport
 */
esp_err_t esp_aliro_session_run_exchange(esp_aliro_session_handle_t handle,
                                         esp_aliro_message_exchange_cb_t msg_exchange_cb,
                                         esp_aliro_crypto_engine_type_t crypto_engine_type);

/**
 * @brief Run an exchange transaction with mailbox requests.
 *
 * @param[in] handle Session handle
 * @param[in] msg_exchange_cb Callback used to exchange APDU messages with the user device
 * @param[in] crypto_engine_type Crypto engine to use for the exchange
 * @param[in] params Mailbox exchange parameters
 * @param[out] out_response Optional buffer for the decrypted response payload. Pass NULL with out_response_len NULL to
 *                          discard it.
 * @param[inout] out_response_len Optional buffer capacity on input, decrypted response length on output. On
 *                                ESP_ERR_INVALID_SIZE, set to required length.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the session is not ready for exchange
 *      - ESP_ERR_NO_MEM if allocation fails
 *      - ESP_ERR_INVALID_SIZE if out_response is too small
 *      - Other esp_err_t values from transaction processing or transport
 */
esp_err_t esp_aliro_session_run_exchange_with_mailbox(esp_aliro_session_handle_t handle,
                                                      esp_aliro_message_exchange_cb_t msg_exchange_cb,
                                                      esp_aliro_crypto_engine_type_t crypto_engine_type,
                                                      const esp_aliro_exchange_mailbox_params_t *params,
                                                      uint8_t *out_response, size_t *out_response_len);

/**
 * @brief Run an envelope transaction for step-up.
 *
 * This is valid only after a standard expedited transaction completes and StepUpSK is available.
 *
 * @param[in] handle Session handle
 * @param[in] msg_exchange_cb Callback used to exchange APDU messages with the user device
 * @param[in] envelope_request Envelope request payload, or NULL when envelope_request_len is 0
 * @param[in] envelope_request_len Envelope request length in bytes
 * @param[out] out_response Optional buffer for the decrypted response payload. Pass NULL with out_response_len NULL to
 *                          discard it.
 * @param[inout] out_response_len Optional buffer capacity on input, decrypted response length on output. On
 *                                ESP_ERR_INVALID_SIZE, set to required length.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid
 *      - ESP_ERR_INVALID_STATE if the session is not ready for envelope
 *      - ESP_ERR_INVALID_SIZE if out_response is too small
 *      - Other esp_err_t values from transaction processing or transport
 */
esp_err_t esp_aliro_session_run_envelope(esp_aliro_session_handle_t handle,
                                         esp_aliro_message_exchange_cb_t msg_exchange_cb,
                                         const uint8_t *envelope_request, size_t envelope_request_len,
                                         uint8_t *out_response, size_t *out_response_len);

#ifdef __cplusplus
}
#endif
