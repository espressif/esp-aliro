/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "iso_dep.h"
#include "st25r3916.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <string.h>

// Limits apply to a whole APDU, including all chained frames and WTX requests.
#define EXCHANGE_TIMEOUT_MS 60000
#define MAX_RECOVERY_ATTEMPTS 2
#define MAX_WTX_REQUESTS 10

#define FRAME_CAPACITY (ISO_DEP_FRAME_SIZE - 2)

static uint32_t frame_wait_ms(uint8_t exponent)
{
    // ISO 14443-4: (256 * 16 / 13.56 MHz) * 2^exponent, rounded up.
    return (uint32_t)(((4096ULL << exponent) * 1000 + 13560000 - 1) / 13560000);
}

esp_err_t iso_dep_activate(iso_dep_session_t *session, uint32_t minimum_fwt_ms)
{
    if (session == NULL || minimum_fwt_ms == 0 || minimum_fwt_ms > EXCHANGE_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(session, 0, sizeof(*session));
    const uint8_t rats[] = {0xe0, 0x80}; // FSDI=8 (256 bytes), CID=0.
    uint8_t ats[22];                     // ATS (at most 20 bytes) plus CRC.
    size_t ats_len = sizeof(ats);
    esp_err_t err = st25r3916_transceive(rats, sizeof(rats), ats, &ats_len, minimum_fwt_ms);
    if (err != ESP_OK) {
        return err;
    }
    if (ats_len == 0 || ats_len > sizeof(ats) - 2 || ats[0] != ats_len) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    static const uint16_t frame_sizes[] = {16, 24, 32, 40, 48, 64, 96, 128, 256};
    uint8_t fsci = 2; // A TL-only ATS uses the default FSC of 32 bytes.
    uint8_t fwi = 4;
    uint8_t sfgi = 0;
    size_t offset = 1;
    if (ats_len > 1) {
        uint8_t format = ats[offset++];
        fsci = format & 0x0f;
        if ((format & 0x80) || fsci > 8) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        if (format & 0x10) {
            if (offset == ats_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            ++offset; // TA: stay at the mandatory 106 kbit/s; no PPS needed.
        }

        if (format & 0x20) {
            if (offset == ats_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            uint8_t timing = ats[offset++];
            fwi = timing >> 4;
            sfgi = timing & 0x0f;
            if (fwi == 15 || sfgi == 15) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        }

        if (format & 0x40) {
            if (offset == ats_len) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            ++offset; // TC advertises support, not use, of CID/NAD.
        }
    }
    if (ats_len - offset > 15) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    session->frame_size = frame_sizes[fsci];
    session->fwt_ms = frame_wait_ms(fwi);
    if (session->fwt_ms < minimum_fwt_ms) {
        session->fwt_ms = minimum_fwt_ms;
    }
    session->sfgt_ms = sfgi ? frame_wait_ms(sfgi) : 0;
    session->active = true;
    return ESP_OK;
}

esp_err_t iso_dep_exchange(iso_dep_session_t *session, const uint8_t *command, size_t command_len, uint8_t *response,
                           size_t *response_len)
{
    if (response_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t capacity = *response_len;
    *response_len = 0;
    if (session == NULL || command == NULL || command_len == 0 || response == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!session->active) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t frame[FRAME_CAPACITY];
    uint8_t received[ISO_DEP_FRAME_SIZE]; // Includes CRC headroom.
    uint8_t control[2];
    size_t command_offset = 0;
    size_t response_offset = 0;
    size_t chunk_len = 0;
    size_t frame_len = 0;
    bool tx_chaining = false;
    bool rx_chaining = false;
    unsigned retries = 0;
    unsigned wtx_count = 0;
    uint32_t wait_ms = session->fwt_ms;
    int64_t deadline_us = esp_timer_get_time() + (int64_t)EXCHANGE_TIMEOUT_MS * 1000;
    const uint8_t *send = NULL;
    size_t send_len = 0;
    esp_err_t err;

    while (true) {
        if (send == NULL) {
            // FSC includes PCB and CRC. Preserve this I-block for link-level retransmission.
            size_t max_inf = session->frame_size - 3;
            chunk_len = command_len - command_offset;
            if (chunk_len > max_inf) {
                chunk_len = max_inf;
            }
            tx_chaining = chunk_len < command_len - command_offset;
            frame[0] = 0x02 | session->block_number | (tx_chaining ? 0x10 : 0);
            memcpy(frame + 1, command + command_offset, chunk_len);
            frame_len = chunk_len + 1;
            send = frame;
            send_len = frame_len;
        }

        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        uint32_t remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        uint32_t timeout_ms = wait_ms < remaining_ms ? wait_ms : remaining_ms;
        size_t received_len = sizeof(received);
        err = st25r3916_transceive(send, send_len, received, &received_len, timeout_ms);
        if (esp_timer_get_time() >= deadline_us) {
            err = ESP_ERR_TIMEOUT;
            break;
        }

        wait_ms = session->fwt_ms;
        if (err != ESP_OK) {
            if ((err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_RESPONSE) ||
                ++retries > MAX_RECOVERY_ATTEMPTS) {
                break;
            }

            // Recover at the block layer, never replay a whole (possibly executed) APDU.
            control[0] = (rx_chaining ? 0xa2 : 0xb2) | session->block_number;
            send = control;
            send_len = 1;
            continue;
        }

        if (received_len == 0 || received_len > FRAME_CAPACITY) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }

        uint8_t pcb = received[0];
        if (pcb == 0xf2) {
            uint8_t multiplier = received_len == 2 ? (received[1] & 0x3f) : 0;
            if (multiplier == 0 || multiplier > 59 || ++wtx_count > MAX_WTX_REQUESTS) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            control[0] = 0xf2;
            control[1] = multiplier;
            send = control;
            send_len = 2;
            wait_ms = session->fwt_ms * multiplier;
            continue;
        }

        if ((pcb & ~0x11) == 0xa2 && received_len == 1 && !rx_chaining) {
            if ((pcb & 0x10) || (pcb & 1) != session->block_number) {
                if (++retries > MAX_RECOVERY_ATTEMPTS) {
                    err = ESP_ERR_INVALID_RESPONSE;
                    break;
                }
                send = frame;
                send_len = frame_len;
                continue;
            }
            if (!tx_chaining) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }

            session->block_number ^= 1;
            command_offset += chunk_len;
            retries = 0;
            send = NULL;
            continue;
        }

        // With CID=0 and no NAD selected, only PCB + INF is accepted.
        if ((pcb & ~0x11) != 0x02 || tx_chaining) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if ((pcb & 1) != session->block_number) {
            if (++retries > MAX_RECOVERY_ATTEMPTS) {
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            // A duplicate chained I-block is acknowledged without appending it twice.
            control[0] = (rx_chaining ? 0xa2 : 0xb2) | session->block_number;
            send = control;
            send_len = 1;
            continue;
        }

        size_t inf_len = received_len - 1;
        if (inf_len > capacity - response_offset) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        memcpy(response + response_offset, received + 1, inf_len);
        response_offset += inf_len;
        session->block_number ^= 1;
        retries = 0;

        if (!(pcb & 0x10)) {
            *response_len = response_offset;
            return ESP_OK;
        }

        rx_chaining = true;
        control[0] = 0xa2 | session->block_number;
        send = control;
        send_len = 1;
    }

    ESP_LOGW("iso_dep", "Exchange failed (err=0x%x, FWT=%" PRIu32 " ms, recovery=%u, WTX=%u)", (unsigned)err,
             session->fwt_ms, retries, wtx_count);
    session->active = false;
    return err;
}
