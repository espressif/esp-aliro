/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_aliro.h>

esp_err_t access_document_init(void);
bool access_document_requires_refresh(const esp_aliro_expedited_result_t *authentication);

/* The caller authorizes the authenticated credential first. All SDK views are consumed before returning. */
esp_err_t access_document_authorize(esp_aliro_session_handle_t session, esp_aliro_message_exchange_cb_t exchange,
                                    const esp_aliro_expedited_result_t *authentication);
