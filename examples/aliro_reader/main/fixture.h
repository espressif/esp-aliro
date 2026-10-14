/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_aliro.h>

typedef struct {
    esp_aliro_reader_config_t reader;
    esp_aliro_cert_policy_t certificate_policy;
    const uint8_t *certificate_start;
    const uint8_t *certificate_end;
    const char *issuer_ca_public_key_pem;
    const char *credential_public_key_pem;
    const char *credential_public_key_pem_end;
    const char *document_issuer_public_key_pem;
    const uint8_t *document_element;
    size_t document_element_size;
    uint16_t document_reader_rule_id;
    esp_aliro_exchange_mailbox_params_t mailbox;
} aliro_reader_fixture_t;

extern const aliro_reader_fixture_t k_actuator_fixture;

esp_err_t fixture_public_key_raw(const char *pem, uint8_t raw[ESP_ALIRO_CREDENTIAL_PUBLIC_KEY_SIZE]);
esp_err_t fixture_lookup_credential_pubkey(const uint8_t *key_slot, size_t key_slot_size, char *out_pubkey,
                                           size_t *out_pubkey_size);
