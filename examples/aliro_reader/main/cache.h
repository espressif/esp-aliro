/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <esp_aliro.h>

#define ALIRO_FAST_CACHE_CAPACITY 3

typedef struct {
    esp_aliro_fast_transaction_entry_t entries[ALIRO_FAST_CACHE_CAPACITY];
    size_t count;
} aliro_fast_cache_t;

/* Zero-initialize one cache per reader. The caller must serialize all access,
 * including synchronous SDK callbacks. Entries and secrets are copied into RAM. */
esp_aliro_fast_transaction_storage_delegate_t aliro_fast_cache_delegate(aliro_fast_cache_t *cache);
