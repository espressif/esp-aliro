/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cache.h"

#include <mbedtls/constant_time.h>
#include <mbedtls/platform_util.h>
#include <string.h>

static bool same_credential(const esp_aliro_fast_transaction_entry_t *a, const esp_aliro_fast_transaction_entry_t *b)
{
    return mbedtls_ct_memcmp(a->access_credential_public_key, b->access_credential_public_key,
                             sizeof(a->access_credential_public_key)) == 0;
}

static esp_err_t visit_entries(void *context, esp_aliro_fast_transaction_entry_visitor_t visitor, void *visitor_context)
{
    if (!context || !visitor) {
        return ESP_ERR_INVALID_ARG;
    }
    const aliro_fast_cache_t *cache = context;
    for (size_t i = 0; i < cache->count; ++i) {
        if (!visitor(&cache->entries[i], visitor_context)) {
            break;
        }
    }
    return ESP_OK;
}

static esp_err_t store_entry(void *context, const esp_aliro_fast_transaction_entry_t *entry)
{
    if (!context || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    aliro_fast_cache_t *cache = context;
    /* Copy before moving slots: callers may supply an entry borrowed from this cache. */
    esp_aliro_fast_transaction_entry_t copy = *entry;
    size_t index = 0;
    while (index < cache->count && !same_credential(&cache->entries[index], entry)) {
        ++index;
    }
    if (index == cache->count) {
        if (cache->count < ALIRO_FAST_CACHE_CAPACITY) {
            ++cache->count;
        } else {
            index = 0;
        }
    }
    mbedtls_platform_zeroize(&cache->entries[index], sizeof(cache->entries[index]));
    memmove(&cache->entries[index], &cache->entries[index + 1], (cache->count - index - 1) * sizeof(cache->entries[0]));
    cache->entries[cache->count - 1] = copy;
    mbedtls_platform_zeroize(&copy, sizeof(copy));
    return ESP_OK;
}

static esp_err_t touch_entry(void *context, const esp_aliro_fast_transaction_entry_t *entry)
{
    if (!context || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    aliro_fast_cache_t *cache = context;
    for (size_t i = 0; i < cache->count; ++i) {
        if (same_credential(&cache->entries[i], entry)) {
            /* A touch changes recency, never secret material or authorization metadata. */
            return store_entry(context, &cache->entries[i]);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t clear_entries(void *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    aliro_fast_cache_t *cache = context;
    mbedtls_platform_zeroize(cache->entries, sizeof(cache->entries));
    cache->count = 0;
    return ESP_OK;
}

esp_aliro_fast_transaction_storage_delegate_t aliro_fast_cache_delegate(aliro_fast_cache_t *cache)
{
    return (esp_aliro_fast_transaction_storage_delegate_t){
        .context = cache,
        .for_each_entry = visit_entries,
        .store_entry = store_entry,
        .touch_entry = touch_entry,
        .clear = clear_entries,
    };
}
