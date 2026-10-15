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

#define ESP_ALIRO_READER_VENDOR_EXT_MAX_LEN 127
#define ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE 65
#define ESP_ALIRO_TIMESTAMP_SIZE 20
#define ESP_ALIRO_FAST_TRANSACTION_KEY_SIZE 32

typedef struct {
    /** Key used to derive the fast-transaction session keys. */
    uint8_t key[ESP_ALIRO_FAST_TRANSACTION_KEY_SIZE];
    /** Uncompressed SEC1 P-256 credential public key: 0x04 || X[32] || Y[32]. */
    uint8_t access_credential_public_key[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE];
} esp_aliro_fast_transaction_entry_t;

/**
 * Visit one fast-transaction entry.
 *
 * @return true to continue iteration, false to stop successfully.
 */
typedef bool (*esp_aliro_fast_transaction_entry_visitor_t)(const esp_aliro_fast_transaction_entry_t *entry,
                                                           void *visitor_context);

typedef struct {
    /** Borrowed callback context. It must remain valid until the Reader is deleted. */
    void *context;
    esp_err_t (*for_each_entry)(void *context, esp_aliro_fast_transaction_entry_visitor_t visitor,
                                void *visitor_context);
    esp_err_t (*store_entry)(void *context, const esp_aliro_fast_transaction_entry_t *entry);
    esp_err_t (*touch_entry)(void *context, const esp_aliro_fast_transaction_entry_t *entry);
    esp_err_t (*clear)(void *context);
} esp_aliro_fast_transaction_storage_delegate_t;

typedef struct {
    /** Uncompressed SEC1 P-256 encoding: 0x04 || X[32] || Y[32]. */
    uint8_t access_credential_public_key[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE];
    /** Whether credential_signed_timestamp contains an authenticated Aliro tdate. */
    bool has_credential_signed_timestamp;
    uint8_t credential_signed_timestamp[ESP_ALIRO_TIMESTAMP_SIZE];
} esp_aliro_expedited_result_t;

typedef struct {
    uint64_t device_response_status;
    /** Complete borrowed CBOR document, or NULL when the response contains no document. */
    const uint8_t *document;
    size_t document_size;
    /** Borrowed unverified Credential Issuer key identifier, when present. */
    const uint8_t *issuer_key_id;
    size_t issuer_key_id_size;
} esp_aliro_access_document_t;

/** Inputs supplied by the application when verifying an Access Document. */
typedef struct {
    /** Credential authenticated by the current Aliro session, as uncompressed SEC1 P-256. */
    const uint8_t *authenticated_credential_public_key;
    size_t authenticated_credential_public_key_size;
    /** Directly trusted Credential Issuer key, as uncompressed SEC1 P-256; optional with `x5chain`. */
    const uint8_t *trusted_issuer_public_key;
    size_t trusted_issuer_public_key_size;
    /** Trusted Credential Issuer CA key, as uncompressed SEC1 P-256; required with `x5chain`. */
    const uint8_t *trusted_issuer_ca_public_key;
    size_t trusted_issuer_ca_public_key_size;
    /** Trusted current UTC time as Unix seconds, or NULL when unavailable. */
    const int64_t *trusted_current_unix_time_seconds;
    /** Current AccessIteration stored by the application for this Credential Issuer. */
    uint64_t access_iteration;
} esp_aliro_document_verification_params_t;

/** Borrowed verified element and authenticated document metadata. */
typedef struct {
    const uint8_t *encoded_element;
    size_t encoded_element_size;
    /** Fixed 20-byte Aliro UTC tdates authenticated by IssuerAuth. */
    uint8_t signed_timestamp[ESP_ALIRO_TIMESTAMP_SIZE];
    uint8_t valid_from[ESP_ALIRO_TIMESTAMP_SIZE];
    uint8_t valid_until[ESP_ALIRO_TIMESTAMP_SIZE];
    bool time_verification_required;
    /** Whether IssuerAuth contains an authenticated document ValidityIteration. */
    bool has_document_access_iteration;
    /** Document's own iteration, not the application's per-issuer high-water mark; zero when absent. */
    uint64_t document_access_iteration;
} esp_aliro_verified_access_document_t;

typedef struct {
    uint8_t vendor_id[3];
    const uint8_t *product_id;
    size_t product_id_len;
    const uint8_t *firmware_version;
    size_t firmware_version_len;
} esp_aliro_reader_descriptor_t;

typedef uintptr_t esp_aliro_reader_handle_t;
typedef uintptr_t esp_aliro_session_handle_t;

typedef struct {
    const char *storage_partition_name; /*!< Optional SDK storage partition name, defaults to "nvs" when unset */
} esp_aliro_config_t;

typedef enum {
    ESP_ALIRO_CERT_POLICY_NONE = 0,  /*!< Do not send the reader certificate */
    ESP_ALIRO_CERT_POLICY_LOAD_CERT, /*!< Send the reader certificate with the LOAD CERT command */
    ESP_ALIRO_CERT_POLICY_AUTH1,     /*!< Send the reader certificate with the AUTH1 command */
} esp_aliro_cert_policy_t;

typedef struct {
    uint8_t group_identifier[16]; /*!< Reader Group Identifier */
    const char *reader_pubkey;    /*!< Reader key pair public key, X.509 PEM encoded */
    const char *reader_privkey;   /*!< Reader key pair private key, X.509 PEM encoded */
} esp_aliro_reader_config_t;

typedef struct {
    const uint8_t *cert_x509_der;          /*!< Reader certificate, X.509 DER encoded */
    size_t cert_x509_der_len;              /*!< Reader certificate length in bytes */
    esp_aliro_cert_policy_t cert_policy;   /*!< Reader certificate transmission policy */
    const char *issuer_ca_pubkey_x509_pem; /*!< Reader certificate issuer CA public key, X.509 PEM encoded */
} esp_aliro_reader_cert_config_t;

typedef enum {
    ESP_ALIRO_NFC_AID_EXPEDITED_PHASE = 0, /*!< Expedited phase AID */
    ESP_ALIRO_NFC_AID_STEP_UP_PHASE = 1,   /*!< Step-up phase AID */
    ESP_ALIRO_NFC_AID_TYPE_MAX = 2,        /*!< AID type count. Not a valid AID type */
} esp_aliro_nfc_aid_type_t;

typedef enum {
    ESP_ALIRO_AUTH_POLICY_USER_DEVICE_SETTING = 1,               /*!< Use the user device setting */
    ESP_ALIRO_AUTH_POLICY_USER_DEVICE_SETTING_SECURE_ACTION = 2, /*!< Use the user device setting for secure action */
    ESP_ALIRO_AUTH_POLICY_FORCE_USER_AUTHENTICATION = 3          /*!< Force user authentication */
} esp_aliro_auth_policy_t;

typedef struct {
    esp_aliro_nfc_aid_type_t aid_type;   /*!< NFC AID type */
    esp_aliro_auth_policy_t auth_policy; /*!< Authentication policy */
    bool extended_apdu;                  /*!< Local NFC transport supports extended command APDUs */
} esp_aliro_session_config_t;

typedef enum {
    ESP_ALIRO_CRYPTO_ENGINE_EXPEDITED = 0, /*!< Use the expedited crypto engine */
    ESP_ALIRO_CRYPTO_ENGINE_STEP_UP,       /*!< Use the step-up crypto engine */
} esp_aliro_crypto_engine_type_t;

typedef struct {
    uint16_t offset; /*!< Mailbox read offset */
    uint16_t length; /*!< Number of bytes to read */
} esp_aliro_exchange_read_request_t;

typedef struct {
    uint16_t offset;     /*!< Mailbox write offset */
    const uint8_t *data; /*!< Data to write */
    size_t data_len;     /*!< Length of data in bytes */
} esp_aliro_exchange_write_request_t;

typedef struct {
    uint16_t offset; /*!< Mailbox set offset */
    uint16_t length; /*!< Number of bytes to set */
    uint8_t value;   /*!< Value to write */
} esp_aliro_exchange_set_request_t;

typedef struct {
    const esp_aliro_exchange_read_request_t *read_reqs;   /*!< Mailbox read requests */
    size_t read_reqs_count;                               /*!< Number of mailbox read requests */
    const esp_aliro_exchange_write_request_t *write_reqs; /*!< Mailbox write requests */
    size_t write_reqs_count;                              /*!< Number of mailbox write requests */
    const esp_aliro_exchange_set_request_t *set_reqs;     /*!< Mailbox set requests */
    size_t set_reqs_count;                                /*!< Number of mailbox set requests */
    const uint8_t *update_doc_req;                        /*!< Update document request data */
    size_t update_doc_req_len;                            /*!< Length of update_doc_req in bytes */
} esp_aliro_exchange_mailbox_params_t;

typedef enum {
    ESP_ALIRO_TRANSACTION_STANDARD = 0, /*!< Standard transaction */
    ESP_ALIRO_TRANSACTION_FAST = 1,     /*!< Fast transaction */
} esp_aliro_transaction_type_t;

#ifdef __cplusplus
}
#endif
