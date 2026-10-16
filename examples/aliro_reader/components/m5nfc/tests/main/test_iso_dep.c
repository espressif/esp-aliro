/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "iso_dep.h"
#include "st25r3916.h"

#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <unity.h>

// The boundary is an RF frame (without CRC), not internal protocol calls.
typedef struct {
    uint8_t tx[254];
    size_t tx_len;
    uint8_t rx[254];
    size_t rx_len;
    esp_err_t result;
    uint32_t timeout_ms;
    uint32_t elapsed_ms;
    bool exhaust_timeout;
} frame_step_t;

static frame_step_t s_steps[16];
static size_t s_step_count;
static size_t s_step_index;
static bool s_transcript_active;
static uint32_t s_last_timeout_ms;

// Linker wrapping substitutes only the RF peer, never ESP-IDF or its clock.
esp_err_t __real_st25r3916_transceive(const uint8_t *command, size_t command_len, uint8_t *response,
                                      size_t *response_len, uint32_t timeout_ms);

void setUp(void)
{
    s_step_count = s_step_index = 0;
    s_transcript_active = false;
    s_last_timeout_ms = 0;
}

void tearDown(void)
{
    s_transcript_active = false;
}

static void peer_delay_ms(uint32_t ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks) {
        vTaskDelay(ticks);
    }

    // Finish the sub-tick remainder without faking elapsed time. Long waits
    // yield to the real RTOS, including the 60-second transaction deadline.
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us > 0) {
        esp_rom_delay_us((uint32_t)remaining_us);
    }
}

static frame_step_t *expect(const uint8_t *tx, size_t tx_len, const uint8_t *rx, size_t rx_len, esp_err_t result)
{
    TEST_ASSERT(s_step_count < sizeof(s_steps) / sizeof(s_steps[0]));
    frame_step_t *step = &s_steps[s_step_count++];
    *step = (frame_step_t){.tx_len = tx_len, .rx_len = rx_len, .result = result, .elapsed_ms = 1};
    TEST_ASSERT(tx_len <= sizeof(step->tx) && rx_len <= sizeof(step->rx));
    memcpy(step->tx, tx, tx_len);
    if (rx_len) {
        memcpy(step->rx, rx, rx_len);
    }
    return step;
}

esp_err_t __wrap_st25r3916_transceive(const uint8_t *command, size_t command_len, uint8_t *response,
                                      size_t *response_len, uint32_t timeout_ms)
{
    if (!s_transcript_active) {
        return __real_st25r3916_transceive(command, command_len, response, response_len, timeout_ms);
    }

    TEST_ASSERT(s_step_index < s_step_count);
    const frame_step_t *step = &s_steps[s_step_index++];
    TEST_ASSERT(command_len == step->tx_len);
    TEST_ASSERT(memcmp(command, step->tx, command_len) == 0);
    TEST_ASSERT(timeout_ms > 0);
    if (step->timeout_ms) {
        TEST_ASSERT(timeout_ms == step->timeout_ms);
    }

    uint32_t elapsed_ms = step->exhaust_timeout ? timeout_ms : step->elapsed_ms;
    TEST_ASSERT(elapsed_ms <= timeout_ms);
    s_last_timeout_ms = timeout_ms;
    peer_delay_ms(elapsed_ms);

    TEST_ASSERT(step->rx_len + 2 <= *response_len);
    if (step->result == ESP_OK) {
        memcpy(response, step->rx, step->rx_len);
        response[step->rx_len] = 0x12;
        response[step->rx_len + 1] = 0x34;
        *response_len = step->rx_len;
    } else {
        *response_len = 0;
    }
    return step->result;
}

static void transcript_done(void)
{
    TEST_ASSERT(s_step_index == s_step_count);
    s_step_count = 0;
    s_step_index = 0;
    s_transcript_active = false;
}

static esp_err_t activate(iso_dep_session_t *session, const uint8_t *ats, size_t ats_len)
{
    s_transcript_active = true;
    const uint8_t rats[] = {0xe0, 0x80};
    expect(rats, sizeof(rats), ats, ats_len, ESP_OK);
    return iso_dep_activate(session, 1000);
}

static iso_dep_session_t default_session(void)
{
    iso_dep_session_t session;
    const uint8_t ats[] = {1};
    TEST_ASSERT(activate(&session, ats, sizeof(ats)) == ESP_OK);
    return session;
}

TEST_CASE("ATS negotiation validates optional fields and timing", "[iso_dep]")
{
    iso_dep_session_t session = default_session();
    TEST_ASSERT(session.frame_size == 32 && session.fwt_ms == 1000);
    transcript_done();

    // Negotiated FWT can exceed the configured floor; SFGT is distinct from FWT.
    const uint8_t timed[] = {4, 0x68, 0xe2, 0x03};
    TEST_ASSERT(activate(&session, timed, sizeof(timed)) == ESP_OK);
    TEST_ASSERT(session.frame_size == 256 && session.fwt_ms == 4950 && session.sfgt_ms == 2);
    transcript_done();

    static const struct {
        uint8_t bytes[5];
        size_t len;
    } invalid[] = {
        {{0}, 0},          {{2}, 1},       {{2, 0x09}, 2},       {{2, 0x80}, 2},       {{2, 0x10}, 2},
        {{3, 0x30, 0}, 3}, {{2, 0x40}, 2}, {{3, 0x20, 0xf0}, 3}, {{3, 0x20, 0x4f}, 3},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        session.active = true;
        TEST_ASSERT(activate(&session, invalid[i].bytes, invalid[i].len) == ESP_ERR_INVALID_RESPONSE);
        TEST_ASSERT(!session.active);
        transcript_done();
    }
}

TEST_CASE("Sequential APDUs preserve block numbering", "[iso_dep]")
{
    iso_dep_session_t session = default_session();
    const uint8_t command[] = {0x00, 0xa4};
    const uint8_t first_tx[] = {0x02, 0x00, 0xa4};
    const uint8_t first_rx[] = {0x02, 0x61, 0x01, 0x90, 0x00};
    const uint8_t second_tx[] = {0x03, 0x00, 0xa4};
    const uint8_t second_rx[] = {0x03, 0x90, 0x00};
    expect(first_tx, sizeof(first_tx), first_rx, sizeof(first_rx), ESP_OK);
    expect(second_tx, sizeof(second_tx), second_rx, sizeof(second_rx), ESP_OK);
    uint8_t response[32];
    size_t len = sizeof(response);

    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == sizeof(first_rx) - 1 && memcmp(response, first_rx + 1, len) == 0);

    len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == 2 && response[0] == 0x90 && response[1] == 0);
    transcript_done();
}

TEST_CASE("Bidirectional chaining suppresses duplicate response blocks", "[iso_dep]")
{
    iso_dep_session_t session;
    const uint8_t ats[] = {2, 0}; // FSC=16, so each outgoing INF is at most 13 bytes.
    TEST_ASSERT(activate(&session, ats, sizeof(ats)) == ESP_OK);
    uint8_t command[28];
    for (size_t i = 0; i < sizeof(command); ++i) {
        command[i] = i;
    }

    uint8_t tx[14] = {0x12};
    memcpy(tx + 1, command, 13);
    const uint8_t ack0[] = {0xa2};
    const uint8_t ack1[] = {0xa3};
    expect(tx, sizeof(tx), ack0, 1, ESP_OK);

    tx[0] = 0x13;
    memcpy(tx + 1, command + 13, 13);
    expect(tx, sizeof(tx), ack1, 1, ESP_OK);

    const uint8_t last_tx[] = {0x02, 26, 27};
    const uint8_t first_rx[] = {0x12, 0x11, 0x22, 0x33};
    const uint8_t last_rx[] = {0x03, 0x90, 0x00};
    expect(last_tx, sizeof(last_tx), first_rx, sizeof(first_rx), ESP_OK);
    expect(ack1, 1, first_rx, sizeof(first_rx), ESP_OK); // Lost ACK: duplicate block.
    expect(ack1, 1, last_rx, sizeof(last_rx), ESP_OK);
    uint8_t response[8];
    size_t len = sizeof(response);
    const uint8_t expected[] = {0x11, 0x22, 0x33, 0x90, 0x00};

    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == sizeof(expected) && memcmp(response, expected, len) == 0);
    transcript_done();
}

TEST_CASE("Receive capacity follows our advertised FSD", "[iso_dep]")
{
    iso_dep_session_t session;
    const uint8_t ats[] = {2, 0}; // Peer receives 16 bytes; we advertised 256 in RATS.
    TEST_ASSERT(activate(&session, ats, sizeof(ats)) == ESP_OK);
    const uint8_t command[] = {0x00};
    const uint8_t tx[] = {0x02, 0x00};
    uint8_t rx[254] = {0x02};
    for (size_t i = 1; i < sizeof(rx); ++i) {
        rx[i] = i;
    }
    expect(tx, sizeof(tx), rx, sizeof(rx), ESP_OK);
    uint8_t response[253];
    size_t len = sizeof(response);

    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == sizeof(response) && memcmp(response, rx + 1, len) == 0);
    transcript_done();
}

TEST_CASE("Link recovery retransmits a block without replaying the APDU", "[iso_dep]")
{
    iso_dep_session_t session;
    const uint8_t ats[] = {2, 0};
    TEST_ASSERT(activate(&session, ats, sizeof(ats)) == ESP_OK);
    uint8_t command[14] = {0};
    uint8_t first[14] = {0x12};
    const uint8_t ack0[] = {0xa2};
    const uint8_t nak1[] = {0xb3};
    const uint8_t last[] = {0x03, 0x00};
    const uint8_t result[] = {0x03, 0x90, 0x00};
    expect(first, sizeof(first), ack0, 1, ESP_OK);
    expect(last, sizeof(last), NULL, 0, ESP_ERR_INVALID_CRC);
    expect(nak1, 1, ack0, 1, ESP_OK); // Peer requests the last I-block again.
    expect(last, sizeof(last), result, sizeof(result), ESP_OK);
    uint8_t response[2];
    size_t len = sizeof(response);

    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    TEST_ASSERT(len == 2 && response[0] == 0x90);
    transcript_done();
}

TEST_CASE("WTX remains bounded by the real transaction deadline", "[iso_dep][slow]")
{
    iso_dep_session_t session = default_session();
    const uint8_t command[] = {0x00};
    const uint8_t tx[] = {0x02, 0x00};
    const uint8_t wtx[] = {0xf2, 3};
    const uint8_t rx[] = {0x02, 0x90, 0x00};
    expect(tx, sizeof(tx), wtx, sizeof(wtx), ESP_OK)->timeout_ms = 1000;
    expect(wtx, sizeof(wtx), rx, sizeof(rx), ESP_OK)->timeout_ms = 3000;
    uint8_t response[8];
    size_t len = sizeof(response);

    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_OK);
    transcript_done();

    session = default_session();
    const uint8_t long_wtx[] = {0xf2, 59};
    expect(tx, sizeof(tx), long_wtx, sizeof(long_wtx), ESP_OK);
    frame_step_t *step = expect(long_wtx, 2, long_wtx, 2, ESP_OK);
    step->timeout_ms = 59000;
    step->elapsed_ms = 59000;
    step = expect(long_wtx, 2, NULL, 0, ESP_ERR_TIMEOUT);
    step->exhaust_timeout = true; // Consume the actual remaining wall-clock budget.

    len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_TIMEOUT);
    TEST_ASSERT(!session.active && len == 0);
    TEST_ASSERT_LESS_THAN_UINT32(1000, s_last_timeout_ms);
    transcript_done();

    session = default_session();
    expect(tx, sizeof(tx), wtx, 2, ESP_OK);
    for (unsigned i = 0; i < 10; ++i) {
        expect(wtx, 2, wtx, 2, ESP_OK);
    }

    len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_INVALID_RESPONSE);
    TEST_ASSERT(!session.active && len == 0);
    transcript_done();
}

TEST_CASE("Malformed frames and buffer exhaustion invalidate the session", "[iso_dep]")
{
    const uint8_t command[] = {0x00};
    const uint8_t tx[] = {0x02, 0x00};
    const uint8_t nak[] = {0xb2};
    iso_dep_session_t session = default_session();
    expect(tx, sizeof(tx), NULL, 0, ESP_ERR_TIMEOUT);
    expect(nak, 1, NULL, 0, ESP_ERR_TIMEOUT);
    expect(nak, 1, NULL, 0, ESP_ERR_TIMEOUT);
    uint8_t response[8];
    size_t len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_TIMEOUT);
    TEST_ASSERT(!session.active && len == 0);

    len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_INVALID_STATE);
    transcript_done();

    session = default_session();
    const uint8_t first_rx[] = {0x12, 1, 2, 3, 4};
    const uint8_t ack[] = {0xa3};
    const uint8_t last_rx[] = {0x03, 5, 6, 7, 8, 9};
    expect(tx, sizeof(tx), first_rx, sizeof(first_rx), ESP_OK);
    expect(ack, 1, last_rx, sizeof(last_rx), ESP_OK);
    len = sizeof(response);
    TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_INVALID_SIZE);
    TEST_ASSERT(!session.active && len == 0);
    transcript_done();

    static const struct {
        uint8_t bytes[4];
        size_t len;
    } invalid[] = {
        {{0}, 0},
        {{0x00}, 1},
        {{0x0a, 0x00, 0x90}, 3},
        {{0x06, 0x00, 0x90}, 3},
        {{0xf2, 0}, 2},
        {{0xf2, 60}, 2},
        {{0xf2}, 1},
        {{0xf2, 1, 0}, 3},
        {{0xc2}, 1},
        {{0xa2}, 1},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        session = default_session();
        expect(tx, sizeof(tx), invalid[i].bytes, invalid[i].len, ESP_OK);
        len = sizeof(response);
        TEST_ASSERT(iso_dep_exchange(&session, command, sizeof(command), response, &len) == ESP_ERR_INVALID_RESPONSE);
        TEST_ASSERT(!session.active && len == 0);
        transcript_done();
    }
}
