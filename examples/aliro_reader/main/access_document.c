/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "access_document.h"
#include "fixture.h"

#include <cbor.h>
#include <esp_aliro_utils.h>
#include <esp_check.h>
#include <esp_log.h>
#include <mbedtls/constant_time.h>
#include <sdkconfig.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const k_tag = "AliroDocument";
/* Frozen time is for reproducible fixture validation, never a production clock. */
static const int64_t k_validation_unix_time_seconds = CONFIG_EXAMPLE_DOCUMENT_VALIDATION_TIME;
static uint8_t s_issuer[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE];
static uint8_t s_issuer_ca[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE];
static uint64_t s_issuer_access_iteration;
static bool s_has_cached_document;
static uint64_t s_cached_document_iteration;
static uint8_t s_cached_signed_timestamp[ESP_ALIRO_TIMESTAMP_SIZE];
static uint8_t s_cached_valid_from[ESP_ALIRO_TIMESTAMP_SIZE];
static uint8_t s_cached_valid_until[ESP_ALIRO_TIMESTAMP_SIZE];

esp_err_t access_document_init(void)
{
    s_issuer_access_iteration = 0;
    s_has_cached_document = false;
    s_cached_document_iteration = 0;
    ESP_RETURN_ON_ERROR(fixture_public_key_raw(k_actuator_fixture.document_issuer_public_key_pem, s_issuer), k_tag,
                        "invalid issuer provisioning");
    return fixture_public_key_raw(k_actuator_fixture.issuer_ca_public_key_pem, s_issuer_ca);
}

bool access_document_requires_refresh(const esp_aliro_expedited_result_t *authentication)
{
    if (!authentication || !s_has_cached_document || s_cached_document_iteration < s_issuer_access_iteration ||
        !authentication->has_credential_signed_timestamp ||
        mbedtls_ct_memcmp(authentication->credential_signed_timestamp, s_cached_signed_timestamp,
                          sizeof(s_cached_signed_timestamp)) != 0) {
        return true;
    }
    time_t seconds = (time_t)k_validation_unix_time_seconds;
    struct tm utc;
    char current[ESP_ALIRO_TIMESTAMP_SIZE + 1];
    if (!gmtime_r(&seconds, &utc) ||
        strftime(current, sizeof(current), "%Y-%m-%dT%H:%M:%SZ", &utc) != ESP_ALIRO_TIMESTAMP_SIZE) {
        return true;
    }
    /* The SDK already validated canonical UTC tdates, whose byte order is chronological. */
    return memcmp(current, s_cached_signed_timestamp, ESP_ALIRO_TIMESTAMP_SIZE) < 0 ||
        memcmp(current, s_cached_valid_from, ESP_ALIRO_TIMESTAMP_SIZE) < 0 ||
        memcmp(current, s_cached_valid_until, ESP_ALIRO_TIMESTAMP_SIZE) > 0;
}

static bool container_length(const CborValue *value, bool map, size_t expected)
{
    size_t length;
    if (map) {
        return cbor_value_is_map(value) && cbor_value_get_map_length(value, &length) == CborNoError &&
            length == expected;
    }
    return cbor_value_is_array(value) && cbor_value_get_array_length(value, &length) == CborNoError &&
        length == expected;
}

static esp_err_t evaluate_reader_rule(const uint8_t *encoded, size_t size)
{
    if (!encoded || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    CborParser parser;
    CborValue root, cursor, rule;
    if (cbor_parser_init(encoded, size, 0, &parser, &root) != CborNoError || !container_length(&root, true, 2) ||
        cbor_value_enter_container(&root, &cursor) != CborNoError) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    /* The supported AccessData profile has canonical integer keys 0 (Version), then 4 (ReaderRuleIds). */
    uint64_t field, version, rule_id;
    if (!cbor_value_is_unsigned_integer(&cursor) || cbor_value_get_uint64(&cursor, &field) != CborNoError ||
        field != 0 || cbor_value_advance(&cursor) != CborNoError || !cbor_value_is_unsigned_integer(&cursor) ||
        cbor_value_get_uint64(&cursor, &version) != CborNoError || version != 1 ||
        cbor_value_advance(&cursor) != CborNoError || !cbor_value_is_unsigned_integer(&cursor) ||
        cbor_value_get_uint64(&cursor, &field) != CborNoError || field != 4 ||
        cbor_value_advance(&cursor) != CborNoError) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!container_length(&cursor, false, 1) || cbor_value_enter_container(&cursor, &rule) != CborNoError ||
        !cbor_value_is_unsigned_integer(&rule) || cbor_value_get_uint64(&rule, &rule_id) != CborNoError ||
        rule_id != k_actuator_fixture.document_reader_rule_id) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (cbor_value_advance(&root) != CborNoError || cbor_value_get_next_byte(&root) != encoded + size) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t access_document_authorize(esp_aliro_session_handle_t session, esp_aliro_message_exchange_cb_t exchange,
                                    const esp_aliro_expedited_result_t *authentication)
{
    ESP_RETURN_ON_FALSE(authentication, ESP_ERR_INVALID_ARG, k_tag, "missing authentication result");
    esp_aliro_document_verification_params_t params = {
        .trusted_issuer_public_key = s_issuer,
        .trusted_issuer_public_key_size = sizeof(s_issuer),
        .trusted_issuer_ca_public_key = s_issuer_ca,
        .trusted_issuer_ca_public_key_size = sizeof(s_issuer_ca),
        .authenticated_credential_public_key = authentication->access_credential_public_key,
        .authenticated_credential_public_key_size = sizeof(authentication->access_credential_public_key),
        .trusted_current_unix_time_seconds = &k_validation_unix_time_seconds,
        .access_iteration = s_issuer_access_iteration,
    };
    uint8_t request[256];
    size_t request_size = sizeof(request);
    ESP_RETURN_ON_ERROR(esp_aliro_utils_access_document_request_encode(k_actuator_fixture.document_element,
                                                                       k_actuator_fixture.document_element_size, false,
                                                                       request, &request_size),
                        k_tag, "cannot encode Access Document request");
    const size_t response_capacity = 4096;
    uint8_t *response = malloc(response_capacity);
    ESP_RETURN_ON_FALSE(response, ESP_ERR_NO_MEM, k_tag, "cannot allocate ENVELOPE response");
    size_t response_size = response_capacity;
    esp_err_t ret = esp_aliro_session_run_envelope(session, exchange, request, request_size, response, &response_size);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_SIZE) {
            if (response_size > response_capacity) {
                ESP_LOGW(k_tag, "Decrypted ENVELOPE exceeds %u-byte output capacity (need %u)",
                         (unsigned)response_capacity, (unsigned)response_size);
            } else {
                ESP_LOGW(k_tag, "ENVELOPE size check failed; required output size was not reported");
            }
        }
        goto cleanup;
    }
    esp_aliro_access_document_t document;
    ESP_GOTO_ON_ERROR(esp_aliro_utils_access_document_parse(response, response_size, &document), cleanup, k_tag,
                      "invalid document response");
    esp_aliro_verified_access_document_t verified;
    ESP_GOTO_ON_ERROR(esp_aliro_utils_access_document_verify(&document, k_actuator_fixture.document_element,
                                                             k_actuator_fixture.document_element_size, &params,
                                                             &verified),
                      cleanup, k_tag, "document verification failed");
    /* Issuer high-water marks only increase, including when the reader rule denies access. */
    if (verified.has_document_access_iteration && verified.document_access_iteration > s_issuer_access_iteration) {
        s_issuer_access_iteration = verified.document_access_iteration;
    }
    if (authentication->has_credential_signed_timestamp &&
        mbedtls_ct_memcmp(authentication->credential_signed_timestamp, verified.signed_timestamp,
                          sizeof(verified.signed_timestamp)) != 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    /* This demo profile does not support time-based AccessRules. Never downgrade to an allowlist. */
    if (verified.time_verification_required) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    ESP_GOTO_ON_ERROR(evaluate_reader_rule(verified.encoded_element, verified.encoded_element_size), cleanup, k_tag,
                      "reader rule denied access");
    /* Copy authenticated metadata, never a borrowed CBOR view or the issuer high-water mark. */
    s_cached_document_iteration = verified.has_document_access_iteration ? verified.document_access_iteration : 0;
    memcpy(s_cached_signed_timestamp, verified.signed_timestamp, sizeof(s_cached_signed_timestamp));
    memcpy(s_cached_valid_from, verified.valid_from, sizeof(s_cached_valid_from));
    memcpy(s_cached_valid_until, verified.valid_until, sizeof(s_cached_valid_until));
    s_has_cached_document = true;
    ESP_LOGI(k_tag, "Reader rule %u permits access", (unsigned)k_actuator_fixture.document_reader_rule_id);
cleanup:
    free(response);
    return ret;
}
