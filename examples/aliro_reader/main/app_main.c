/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <m5nfc.h>

#include <esp_aliro.h>
#include <esp_aliro_utils.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <string.h>

// --- Theses files/values comes from CSA aliro-actuator reference ---
extern const char k_credential_pubkey_start[] asm("_binary_credential_pubkey_pem_start");
extern const char k_credential_pubkey_end[] asm("_binary_credential_pubkey_pem_end");
extern const char k_reader_privkey_start[] asm("_binary_reader_privkey_pem_start");
extern const char k_reader_pubkey_start[] asm("_binary_reader_pubkey_pem_start");
extern const uint8_t k_reader_cert_start[] asm("_binary_reader_cert_der_start");
extern const uint8_t k_reader_cert_end[] asm("_binary_reader_cert_der_end");
extern const char k_reader_cert_issuer_ca_pubkey_start[] asm("_binary_reader_cert_issuer_ca_pubkey_pem_start");
#define EXAMPLE_MAILBOX_PAYLOAD {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}
#define EXAMPLE_STEP_UP_ENVELOPE_PAYLOAD                                                                         \
    {0xA2, 0x61, 0x31, 0x63, 0x31, 0x2E, 0x30, 0x61, 0x32, 0x82, 0xA1, 0x61, 0x31, 0xD8, 0x18, 0x58, 0x1F, 0xA2, \
     0x61, 0x35, 0x67, 0x61, 0x6C, 0x69, 0x72, 0x6F, 0x2D, 0x61, 0x61, 0x31, 0xA1, 0x67, 0x61, 0x6C, 0x69, 0x72, \
     0x6F, 0x2D, 0x61, 0xA1, 0x66, 0x66, 0x6C, 0x6F, 0x6F, 0x72, 0x31, 0xF4, 0xA1, 0x61, 0x31, 0xD8, 0x18, 0x58, \
     0x1F, 0xA2, 0x61, 0x35, 0x67, 0x61, 0x6C, 0x69, 0x72, 0x6F, 0x2D, 0x72, 0x61, 0x31, 0xA1, 0x67, 0x61, 0x6C, \
     0x69, 0x72, 0x6F, 0x2D, 0x72, 0xA1, 0x66, 0x66, 0x6C, 0x6F, 0x6F, 0x72, 0x32, 0xF5}
static const uint8_t k_reader_group_id[] = {0x00, 0x11, 0x33, 0x44, 0x66, 0x77, 0x99, 0xAA,
                                            0x00, 0x11, 0x33, 0x44, 0x66, 0x77, 0x99,
#if CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT || CONFIG_READER_CERT_POLICY_SEND_VIA_AUTH1
                                            0xAB};
#else
                                            0xAA};
#endif
// --- End ---

static const char *const k_tag = "AliroReader";
static const uint32_t k_no_card_delay_ms = 100;
static const uint32_t k_nfc_detect_task_stack_size = 8192;
#if CONFIG_SUPPORT_MAILBOX
static const size_t k_mailbox_response_max_len = 64;
#endif
#if CONFIG_SUPPORT_STEPUP_PHASE
static const size_t k_envelope_response_max_len = 2048;
#endif

static esp_aliro_reader_handle_t s_reader;

static esp_err_t nfc_message_exchange(const uint8_t *command, size_t command_len, uint8_t *response,
                                      size_t *response_len)
{
    return m5nfc_message_exchange(command, command_len, response, response_len);
}

#if CONFIG_SUPPORT_KEYSLOT
static esp_err_t lookup_credential_pubkey(const uint8_t *key_slot, size_t key_slot_len, char *out_pubkey,
                                          size_t *out_pubkey_len)
{
    ESP_RETURN_ON_FALSE(key_slot && out_pubkey && out_pubkey_len, ESP_ERR_INVALID_ARG, k_tag,
                        "invalid key-slot lookup argument");

    uint8_t expected_key_slot[8];
    size_t expected_key_slot_len = sizeof(expected_key_slot);
    ESP_RETURN_ON_ERROR(esp_aliro_get_key_slot_from_cred_pubkey(k_credential_pubkey_start,
                                                                k_credential_pubkey_end - k_credential_pubkey_start,
                                                                expected_key_slot, &expected_key_slot_len),
                        k_tag, "failed to derive credential key slot");
    if (key_slot_len != expected_key_slot_len || memcmp(key_slot, expected_key_slot, key_slot_len) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t pubkey_len = k_credential_pubkey_end - k_credential_pubkey_start;
    if (*out_pubkey_len < pubkey_len) {
        *out_pubkey_len = pubkey_len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out_pubkey, k_credential_pubkey_start, pubkey_len);
    *out_pubkey_len = pubkey_len;
    return ESP_OK;
}
#endif // CONFIG_SUPPORT_KEYSLOT

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
        esp_aliro_session_config_t session_cfg = {
            .aid_type = ESP_ALIRO_NFC_AID_EXPEDITED_PHASE,
            .auth_policy = ESP_ALIRO_AUTH_POLICY_USER_DEVICE_SETTING_SECURE_ACTION,
        };
        esp_aliro_session_handle_t session = 0;
        esp_err_t err = esp_aliro_session_create(s_reader, &session, &session_cfg);
        if (err == ESP_OK) {
            err = esp_aliro_session_run_expedited(session, nfc_message_exchange);
        }
#if CONFIG_SUPPORT_MAILBOX
        if (err == ESP_OK) {
            ESP_LOGI(k_tag, "Expedited phase finished, sending exchange command with mailbox");
            esp_aliro_exchange_read_request_t read_req = {
                .offset = 0x00,
                .length = 0x08,
            };
            const uint8_t write_data[] = EXAMPLE_MAILBOX_PAYLOAD;
            esp_aliro_exchange_write_request_t write_req = {
                .offset = 0x00,
                .data = write_data,
                .data_len = sizeof(write_data),
            };
            esp_aliro_exchange_set_request_t set_req = {
                .offset = 0x08,
                .length = 0x08,
                .value = 0xFF,
            };
            esp_aliro_exchange_mailbox_params_t mailbox_params = {
                .read_reqs = &read_req,
                .read_reqs_count = 1,
                .write_reqs = &write_req,
                .write_reqs_count = 1,
                .set_reqs = &set_req,
                .set_reqs_count = 1,
                .update_doc_req = NULL,
                .update_doc_req_len = 0,
            };
            uint8_t mailbox_response[k_mailbox_response_max_len];
            size_t mailbox_response_len = sizeof(mailbox_response);
            err = esp_aliro_session_run_exchange_with_mailbox(session, nfc_message_exchange,
                                                              ESP_ALIRO_CRYPTO_ENGINE_EXPEDITED, &mailbox_params,
                                                              mailbox_response, &mailbox_response_len);
            if (err == ESP_OK) {
                ESP_LOGI(k_tag, "Mailbox decrypted response (%u bytes)", (unsigned)mailbox_response_len);
                ESP_LOG_BUFFER_HEX(k_tag, mailbox_response, mailbox_response_len);
            } else if (err == ESP_ERR_INVALID_SIZE) {
                ESP_LOGW(k_tag, "Mailbox response buffer too small, need %u bytes", (unsigned)mailbox_response_len);
            }
        }
#endif // CONFIG_SUPPORT_MAILBOX

        esp_aliro_crypto_engine_type_t exchange_crypto_type = ESP_ALIRO_CRYPTO_ENGINE_EXPEDITED;
#if CONFIG_SUPPORT_STEPUP_PHASE
        if (err == ESP_OK) {
            esp_aliro_transaction_type_t transaction_type;
            err = esp_aliro_session_get_transaction_type(session, &transaction_type);
            if (err == ESP_OK && transaction_type == ESP_ALIRO_TRANSACTION_STANDARD) {
                const uint8_t envelope_request[] = EXAMPLE_STEP_UP_ENVELOPE_PAYLOAD;
                uint8_t envelope_response[k_envelope_response_max_len];
                size_t envelope_response_len = sizeof(envelope_response);
                err =
                    esp_aliro_session_run_envelope(session, nfc_message_exchange, envelope_request,
                                                   sizeof(envelope_request), envelope_response, &envelope_response_len);
                if (err == ESP_OK) {
                    ESP_LOGI(k_tag, "Envelope decrypted response (%u bytes)", (unsigned)envelope_response_len);
                    ESP_LOG_BUFFER_HEX(k_tag, envelope_response, envelope_response_len);
                } else if (err == ESP_ERR_INVALID_SIZE) {
                    ESP_LOGW(k_tag, "Envelope response buffer too small, need %u bytes",
                             (unsigned)envelope_response_len);
                }
                exchange_crypto_type = ESP_ALIRO_CRYPTO_ENGINE_STEP_UP;
            }
        }
#endif // CONFIG_SUPPORT_STEPUP_PHASE
        if (err == ESP_OK) {
            err = esp_aliro_session_run_exchange(session, nfc_message_exchange, exchange_crypto_type);
        }

        const esp_err_t transaction_err = err;
        if (session) {
            (void)esp_aliro_session_delete(&session);
        }
        const int64_t session_time_ms = (esp_timer_get_time() - session_start_us) / 1000;
        if (transaction_err == ESP_OK) {
            ESP_LOGI(k_tag, "Aliro NFC transaction completed successfully in %lld ms", (long long)session_time_ms);
        } else {
            ESP_LOGW(k_tag, "Aliro NFC transaction failed after %lld ms: %s", (long long)session_time_ms,
                     esp_err_to_name(transaction_err));
        }
        m5nfc_deactivate();
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (m5nfc_init() != ESP_OK) {
        ESP_LOGE(k_tag, "NFC reader initialization failed");
        return;
    }

    esp_aliro_config_t sdk_cfg = {
        .storage_partition_name = NULL,
#if CONFIG_SUPPORT_FAST_TRANSACTION_PHASE
        .fast_transaction_storage_size = 20,
#else
        .fast_transaction_storage_size = 0,
#endif
    };
    ESP_ERROR_CHECK(esp_aliro_init(&sdk_cfg));

    esp_aliro_reader_config_t reader_cfg = {0};
    memcpy(reader_cfg.group_identifier, k_reader_group_id, sizeof(reader_cfg.group_identifier));
    reader_cfg.reader_pubkey = k_reader_pubkey_start;
    reader_cfg.reader_privkey = k_reader_privkey_start;

    ESP_ERROR_CHECK(esp_aliro_reader_create(&s_reader, &reader_cfg));

#if CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT || CONFIG_READER_CERT_POLICY_SEND_VIA_AUTH1
    esp_aliro_reader_cert_config_t cert_cfg = {
        .cert_x509_der = k_reader_cert_start,
        .cert_x509_der_len = (size_t)(k_reader_cert_end - k_reader_cert_start),
#if CONFIG_READER_CERT_POLICY_SEND_VIA_LOAD_CERT
        .cert_policy = ESP_ALIRO_CERT_POLICY_LOAD_CERT,
#else
        .cert_policy = ESP_ALIRO_CERT_POLICY_AUTH1,
#endif
        .issuer_ca_pubkey_x509_pem = k_reader_cert_issuer_ca_pubkey_start,
    };
    ESP_ERROR_CHECK(esp_aliro_reader_set_cert(s_reader, &cert_cfg));
#endif
#if CONFIG_SUPPORT_KEYSLOT
    ESP_ERROR_CHECK(esp_aliro_reader_enable_key_slot(s_reader, lookup_credential_pubkey));
#endif
    ESP_ERROR_CHECK(esp_aliro_reader_enable(s_reader));

    xTaskCreate(nfc_detect_task, "nfc_detect", k_nfc_detect_task_stack_size, NULL, 18, NULL);
}
