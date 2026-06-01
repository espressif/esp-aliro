/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ALIRO_READER_VENDOR_EXT_MAX_LEN 127

typedef uintptr_t esp_aliro_reader_handle_t;
typedef uintptr_t esp_aliro_session_handle_t;

typedef struct {
    const char *storage_partition_name; /*!< Optional SDK storage partition name, defaults to "nvs" when unset */
    size_t
        fast_transaction_storage_size; /** Fast transaction persistent-key entry count. 0 disables fast transaction */
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
