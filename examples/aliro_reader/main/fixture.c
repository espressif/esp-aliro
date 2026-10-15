/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fixture.h"

#include <esp_aliro_utils.h>
#include <esp_check.h>
#include <esp_log.h>
#include <mbedtls/constant_time.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <sdkconfig.h>
#include <string.h>

/* Actuator reference: https://github.com/aliro-access-control/aliro-actuator
 * Commit: 4e501e6474be3be3b79be97295478eaedf35db3b (examples/nfc/ keys and provisioning).
 * esp_aliro_user_device.py supplies the signed floor1 Access Document.
 * Reader Rule 1 is provisioned here as an empty AccessRule {}: unconditional access for this reader.
 */
extern const char k_credential_pubkey_start[] asm("_binary_credential_pubkey_pem_start");
extern const char k_credential_pubkey_end[] asm("_binary_credential_pubkey_pem_end");
extern const char k_reader_privkey_start[] asm("_binary_reader_privkey_pem_start");
extern const char k_reader_pubkey_start[] asm("_binary_reader_pubkey_pem_start");
extern const uint8_t k_reader_cert_start[] asm("_binary_reader_cert_der_start");
extern const uint8_t k_reader_cert_end[] asm("_binary_reader_cert_der_end");
extern const char k_reader_cert_issuer_ca_pubkey_start[] asm("_binary_reader_cert_issuer_ca_pubkey_pem_start");
extern const char k_document_issuer_pubkey_start[] asm("_binary_document_issuer_pubkey_pem_start");

#if CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT
enum { k_certificate_policy = ESP_ALIRO_CERT_POLICY_LOAD_CERT, k_reader_group_suffix = 0xAB };
#elif CONFIG_READER_CERT_POLICY_SEND_VIA_AUTH1
enum { k_certificate_policy = ESP_ALIRO_CERT_POLICY_AUTH1, k_reader_group_suffix = 0xAB };
#else  // !(CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT && CONFIG_READER_CERT_POLICY_SEND_VIA_AUTH1)
enum { k_certificate_policy = ESP_ALIRO_CERT_POLICY_NONE, k_reader_group_suffix = 0xAA };
#endif // CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT || CONFIG_READER_CERT_POLICY_SEND_VIA_AUTH1

static const uint8_t k_document_element[] = "floor1";
static const uint8_t k_mailbox_write_data[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
static const esp_aliro_exchange_read_request_t k_mailbox_read = {.offset = 0, .length = 8};
static const esp_aliro_exchange_write_request_t k_mailbox_write = {
    .offset = 0,
    .data = k_mailbox_write_data,
    .data_len = sizeof(k_mailbox_write_data),
};
static const esp_aliro_exchange_set_request_t k_mailbox_set = {.offset = 8, .length = 8, .value = 0xFF};

const aliro_reader_fixture_t k_actuator_fixture = {
    .reader =
        {
            .group_identifier = {0x00, 0x11, 0x33, 0x44, 0x66, 0x77, 0x99, 0xAA, 0x00, 0x11, 0x33, 0x44, 0x66, 0x77,
                                 0x99, k_reader_group_suffix},
            .reader_pubkey = k_reader_pubkey_start,
            .reader_privkey = k_reader_privkey_start,
        },
    .certificate_policy = k_certificate_policy,
    .certificate_start = k_reader_cert_start,
    .certificate_end = k_reader_cert_end,
    .issuer_ca_public_key_pem = k_reader_cert_issuer_ca_pubkey_start,
    .credential_public_key_pem = k_credential_pubkey_start,
    .credential_public_key_pem_end = k_credential_pubkey_end,
    .document_issuer_public_key_pem = k_document_issuer_pubkey_start,
    .document_element = k_document_element,
    .document_element_size = sizeof(k_document_element) - 1,
    .document_reader_rule_id = 1,
    .mailbox =
        {
            .read_reqs = &k_mailbox_read,
            .read_reqs_count = 1,
            .write_reqs = &k_mailbox_write,
            .write_reqs_count = 1,
            .set_reqs = &k_mailbox_set,
            .set_reqs_count = 1,
        },
};

esp_err_t fixture_public_key_raw(const char *pem, uint8_t raw[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE])
{
    /* PEM parsing remains a format operation; key import/export uses PSA on IDF 5 and 6. */
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    int rc = mbedtls_pk_parse_public_key(&key, (const unsigned char *)pem, strlen(pem) + 1);
    if (rc == 0) {
        rc = mbedtls_pk_get_psa_attributes(&key, PSA_KEY_USAGE_VERIFY_HASH, &attributes);
    }
    if (rc == 0 && psa_get_key_type(&attributes) == PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1) &&
        psa_get_key_bits(&attributes) == 256) {
        rc = mbedtls_pk_import_into_psa(&key, &attributes, &id);
    } else {
        rc = -1;
    }
    size_t size = 0;
    if (rc == 0) {
        rc = psa_export_public_key(id, raw, ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE, &size);
    }
    psa_status_t destroy_status = psa_destroy_key(id);
    psa_reset_key_attributes(&attributes);
    mbedtls_pk_free(&key);
    return rc == 0 && size == ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE && destroy_status == PSA_SUCCESS
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

#if CONFIG_SUPPORT_KEYSLOT
static const char *const k_tag = "AliroFixture";

esp_err_t fixture_lookup_credential_pubkey(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                           size_t *out_pubkey_len)
{
    ESP_RETURN_ON_FALSE(key_slot && out_pubkey && out_pubkey_len, ESP_ERR_INVALID_ARG, k_tag,
                        "invalid key-slot lookup argument");
    uint8_t expected_key_slot[8];
    size_t expected_key_slot_len = sizeof(expected_key_slot);
    ESP_RETURN_ON_ERROR(esp_aliro_utils_get_key_slot_from_cred_pubkey(k_actuator_fixture.credential_public_key_pem,
                                                                      k_actuator_fixture.credential_public_key_pem_end -
                                                                          k_actuator_fixture.credential_public_key_pem,
                                                                      expected_key_slot, &expected_key_slot_len),
                        k_tag, "failed to derive credential key slot");
    if (key_slot_len != expected_key_slot_len || mbedtls_ct_memcmp(key_slot, expected_key_slot, key_slot_len) != 0) {
        ESP_LOGW(k_tag, "Key-slot lookup failed: credential not provisioned");
        return ESP_ERR_NOT_FOUND;
    }
    size_t pubkey_len = k_actuator_fixture.credential_public_key_pem_end - k_actuator_fixture.credential_public_key_pem;
    if (*out_pubkey_len < pubkey_len) {
        *out_pubkey_len = pubkey_len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_pubkey, k_actuator_fixture.credential_public_key_pem, pubkey_len);
    *out_pubkey_len = pubkey_len;
    ESP_LOGI(k_tag, "Key-slot lookup succeeded: provisioned credential resolved");
    return ESP_OK;
}
#endif // CONFIG_SUPPORT_KEYSLOT
