// 记录单次 HTTP 请求的连接与首包单调时间，并生成可测试的耗时快照。
#pragma once

#include <stdint.h>

inline constexpr uint32_t kHttpRequestTimingUnavailableMs = UINT32_MAX;

struct HttpRequestTimingState {
    int64_t started_us = 0;
    int64_t connected_us = 0;
    int64_t first_data_us = 0;
    bool connected = false;
    bool first_data = false;
};

struct HttpRequestTimingSnapshot {
    uint32_t connected_ms = kHttpRequestTimingUnavailableMs;
    uint32_t first_data_ms = kHttpRequestTimingUnavailableMs;
    uint32_t elapsed_ms = 0;
};

enum class HttpRequestTimingPhase : uint8_t {
    kOk = 0,
    kConnect,
    kReusedConnect,
    kFirstData,
    kResponse,
};

inline void http_request_timing_begin(HttpRequestTimingState *state,
                                      int64_t now_us)
{
    if (!state) {
        return;
    }
    *state = {};
    state->started_us = now_us;
}

inline void http_request_timing_note_connected(HttpRequestTimingState *state,
                                               int64_t now_us)
{
    if (!state || state->connected || now_us < state->started_us) {
        return;
    }
    state->connected = true;
    state->connected_us = now_us;
}

inline void http_request_timing_note_first_data(HttpRequestTimingState *state,
                                                int64_t now_us)
{
    if (!state || state->first_data || now_us < state->started_us) {
        return;
    }
    state->first_data = true;
    state->first_data_us = now_us;
}

constexpr uint32_t http_request_timing_delta_ms(int64_t started_us,
                                                int64_t event_us)
{
    if (event_us <= started_us) {
        return 0;
    }
    const uint64_t delta_us = static_cast<uint64_t>(event_us - started_us);
    const uint64_t delta_ms = delta_us / 1000U;
    return delta_ms > UINT32_MAX ? UINT32_MAX
                                 : static_cast<uint32_t>(delta_ms);
}

inline HttpRequestTimingSnapshot http_request_timing_snapshot(
    const HttpRequestTimingState &state,
    int64_t finished_us)
{
    HttpRequestTimingSnapshot snapshot;
    if (state.connected) {
        snapshot.connected_ms = http_request_timing_delta_ms(
            state.started_us,
            state.connected_us);
    }
    if (state.first_data) {
        snapshot.first_data_ms = http_request_timing_delta_ms(
            state.started_us,
            state.first_data_us);
    }
    snapshot.elapsed_ms = http_request_timing_delta_ms(state.started_us,
                                                       finished_us);
    return snapshot;
}

constexpr HttpRequestTimingPhase http_request_timing_phase(
    bool success,
    bool reused_client,
    uint32_t connected_ms,
    uint32_t first_data_ms)
{
    if (success) {
        return HttpRequestTimingPhase::kOk;
    }
    if (connected_ms == kHttpRequestTimingUnavailableMs) {
        return reused_client ? HttpRequestTimingPhase::kReusedConnect
                             : HttpRequestTimingPhase::kConnect;
    }
    return first_data_ms == kHttpRequestTimingUnavailableMs
               ? HttpRequestTimingPhase::kFirstData
               : HttpRequestTimingPhase::kResponse;
}
