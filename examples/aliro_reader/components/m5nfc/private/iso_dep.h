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

// FSD=256 includes the PCB and two CRC bytes. CID and NAD are not negotiated.
#define ISO_DEP_FRAME_SIZE 256

typedef struct {
    uint16_t frame_size;
    uint32_t fwt_ms;
    uint32_t sfgt_ms;
    uint8_t block_number;
    bool active;
} iso_dep_session_t;

esp_err_t iso_dep_activate(iso_dep_session_t *session, uint32_t minimum_fwt_ms);

// Failure invalidates the session; the caller must remove the RF field.
esp_err_t iso_dep_exchange(iso_dep_session_t *session, const uint8_t *command, size_t command_len, uint8_t *response,
                           size_t *response_len);
