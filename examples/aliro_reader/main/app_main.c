/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_aliro.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <m5nfc.h>
#include <mbedtls/constant_time.h>
#include <nvs_flash.h>
#include <psa/crypto.h>
#include <sdkconfig.h>
#include <string.h>

#include "access_document.h"
#include "cache.h"
#include "fixture.h"

static const char *const k_tag = "AliroReader";
static const uint32_t k_no_card_delay_ms = 100;
static const uint32_t k_nfc_detect_task_stack_size = 8192;
static esp_aliro_reader_handle_t s_reader;
static uint8_t s_credential_public_key[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE];
#if CONFIG_SUPPORT_FAST_TRANSACTION_PHASE
static aliro_fast_cache_t s_fast_cache;
#endif // CONFIG_SUPPORT_FAST_TRANSACTION_PHASE

static void log_certificate_delivery(esp_aliro_transaction_type_t transaction)
{
    if (transaction != ESP_ALIRO_TRANSACTION_STANDARD ||
        k_actuator_fixture.certificate_policy == ESP_ALIRO_CERT_POLICY_NONE) {
        return;
    }
    ESP_LOGI(k_tag, "Reader certificate sent via %s; standard authentication succeeded",
             k_actuator_fixture.certificate_policy == ESP_ALIRO_CERT_POLICY_LOAD_CERT ? "LOAD CERT" : "AUTH1");
}

static esp_err_t authorize_and_complete(esp_aliro_session_handle_t session,
                                        esp_aliro_expedited_result_t *authentication)
{
    esp_aliro_transaction_type_t transaction;
    ESP_RETURN_ON_ERROR(esp_aliro_session_get_transaction_type(session, &transaction), k_tag,
                        "transaction type unavailable");
    log_certificate_delivery(transaction);
    if (mbedtls_ct_memcmp(s_credential_public_key, authentication->access_credential_public_key,
                          sizeof(s_credential_public_key)) != 0) {
        ESP_LOGW(k_tag, "Authenticated credential is not provisioned for this reader");
        return ESP_ERR_NOT_FOUND;
    }

    esp_aliro_crypto_engine_type_t engine = ESP_ALIRO_CRYPTO_ENGINE_EXPEDITED;
#if CONFIG_SUPPORT_STEPUP_PHASE
    /* Document authorization is reader policy; peer metadata only determines cache freshness. */
    bool needs_document = access_document_requires_refresh(authentication);
    /* Continue before any EXCHANGE, ENVELOPE or commit; never reuse the fast result on failure. */
    if (transaction == ESP_ALIRO_TRANSACTION_FAST && needs_document) {
        ESP_LOGI(k_tag, "Fast authentication requires expedited-standard continuation");
        ESP_RETURN_ON_ERROR(esp_aliro_session_continue_standard(session, m5nfc_message_exchange, authentication), k_tag,
                            "standard continuation failed");
        ESP_RETURN_ON_ERROR(esp_aliro_session_get_transaction_type(session, &transaction), k_tag,
                            "standard transaction type unavailable");
        log_certificate_delivery(transaction);
        if (mbedtls_ct_memcmp(s_credential_public_key, authentication->access_credential_public_key,
                              sizeof(s_credential_public_key)) != 0) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (needs_document) {
        if (transaction != ESP_ALIRO_TRANSACTION_STANDARD) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_RETURN_ON_ERROR(access_document_authorize(session, m5nfc_message_exchange, authentication), k_tag,
                            "document authorization denied");
        engine = ESP_ALIRO_CRYPTO_ENGINE_STEP_UP;
    } else {
        ESP_LOGI(k_tag, "Using cached document authorization; skipping ENVELOPE");
    }
#endif // CONFIG_SUPPORT_STEPUP_PHASE
#if CONFIG_SUPPORT_MAILBOX
    uint8_t response[64];
    size_t response_size = sizeof(response);
    esp_err_t err = esp_aliro_session_run_exchange_with_mailbox(session, m5nfc_message_exchange, engine,
                                                                &k_actuator_fixture.mailbox, response, &response_size);
    if (err != ESP_OK) {
        ESP_LOGW(k_tag, "Mailbox EXCHANGE failed: %s (response size %u)", esp_err_to_name(err),
                 (unsigned)response_size);
        return err;
    }
    ESP_LOGI(k_tag, "Mailbox response: %u bytes", (unsigned)response_size);
#endif // CONFIG_SUPPORT_MAILBOX
    /* Final EXCHANGE closes the session, so commit while authorization and pending storage are still available. */
    ESP_RETURN_ON_ERROR(esp_aliro_session_commit_fast_transaction(session), k_tag, "fast transaction commit failed");
    ESP_RETURN_ON_ERROR(esp_aliro_session_run_exchange(session, m5nfc_message_exchange, engine), k_tag,
                        "final EXCHANGE failed");
    return ESP_OK;
}

static void nfc_detect_task(void *params)
{
    (void)params;
    while (true) {
        m5nfc_update();
        if (!m5nfc_activate()) {
            vTaskDelay(pdMS_TO_TICKS(k_no_card_delay_ms));
            continue;
        }
        const int64_t session_start_us = esp_timer_get_time();
        esp_aliro_session_config_t config = {
            .aid_type = ESP_ALIRO_NFC_AID_EXPEDITED_PHASE,
            .auth_policy = ESP_ALIRO_AUTH_POLICY_USER_DEVICE_SETTING_SECURE_ACTION,
        };
        esp_aliro_expedited_result_t authentication;
        esp_aliro_session_handle_t session = 0;
        esp_err_t err = esp_aliro_session_create(s_reader, &session, &config);
        if (err == ESP_OK) {
            err = esp_aliro_session_run_expedited(session, m5nfc_message_exchange, &authentication);
        }
        if (err == ESP_OK) {
            err = authorize_and_complete(session, &authentication);
        }
        const esp_err_t transaction_err = err;
        if (session) {
            err = esp_aliro_session_delete(&session);
            if (err != ESP_OK) {
                ESP_LOGE(k_tag, "Session cleanup failed: %s", esp_err_to_name(err));
            }
        }
        m5nfc_deactivate();
        const int64_t duration_ms = (esp_timer_get_time() - session_start_us) / 1000;
        if (transaction_err == ESP_OK) {
            ESP_LOGI(k_tag, "Aliro transaction authorized and committed in %lld ms", (long long)duration_ms);
        } else {
            ESP_LOGW(k_tag, "Aliro transaction failed after %lld ms: %s", (long long)duration_ms,
                     esp_err_to_name(transaction_err));
        }
    }
}

static esp_err_t reader_init(void)
{
    ESP_RETURN_ON_ERROR(esp_aliro_reader_create(&s_reader, &k_actuator_fixture.reader), k_tag, "cannot create reader");
#if CONFIG_SUPPORT_FAST_TRANSACTION_PHASE
    esp_aliro_fast_transaction_storage_delegate_t delegate = aliro_fast_cache_delegate(&s_fast_cache);
    ESP_RETURN_ON_ERROR(esp_aliro_reader_set_fast_transaction_storage_delegate(s_reader, &delegate), k_tag,
                        "cannot attach fast cache");
#endif // CONFIG_SUPPORT_FAST_TRANSACTION_PHASE
    if (k_actuator_fixture.certificate_policy != ESP_ALIRO_CERT_POLICY_NONE) {
        esp_aliro_reader_cert_config_t certificate = {
            .cert_x509_der = k_actuator_fixture.certificate_start,
            .cert_x509_der_len = k_actuator_fixture.certificate_end - k_actuator_fixture.certificate_start,
            .cert_policy = k_actuator_fixture.certificate_policy,
            .issuer_ca_pubkey_x509_pem = k_actuator_fixture.issuer_ca_public_key_pem,
        };
        ESP_RETURN_ON_ERROR(esp_aliro_reader_set_cert(s_reader, &certificate), k_tag, "cannot set reader certificate");
    }
#if CONFIG_SUPPORT_KEYSLOT
    ESP_RETURN_ON_ERROR(esp_aliro_reader_enable_key_slot(s_reader, fixture_lookup_credential_pubkey), k_tag,
                        "cannot enable key-slot lookup");
#endif // CONFIG_SUPPORT_KEYSLOT
    return esp_aliro_reader_enable(s_reader);
}

void app_main(void)
{
    /* Do not erase NVS on initialization failure; existing installation data belongs to the operator. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "NVS initialization failed (no data erased): %s", esp_err_to_name(err));
        return;
    }
    err = m5nfc_init();
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "NFC reader initialization failed: %s", esp_err_to_name(err));
        return;
    }
    esp_aliro_config_t sdk_config = {.storage_partition_name = NULL};
    err = esp_aliro_init(&sdk_config);
    if (err != ESP_OK) {
        ESP_LOGE(k_tag, "Aliro initialization failed: %s", esp_err_to_name(err));
        return;
    }
    err = psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        err = fixture_public_key_raw(k_actuator_fixture.credential_public_key_pem, s_credential_public_key);
    }
#if CONFIG_SUPPORT_STEPUP_PHASE
    if (err == ESP_OK) {
        err = access_document_init();
    }
#endif // CONFIG_SUPPORT_STEPUP_PHASE
    if (err == ESP_OK) {
        err = reader_init();
    }
    if (err == ESP_OK &&
        xTaskCreate(nfc_detect_task, "nfc_detect", k_nfc_detect_task_stack_size, NULL, 18, NULL) == pdPASS) {
        return;
    }
    ESP_LOGE(k_tag, "Reader/task initialization failed: %s", err == ESP_OK ? "task allocation" : esp_err_to_name(err));
    if (s_reader) {
        (void)esp_aliro_reader_delete(&s_reader);
    }
}
