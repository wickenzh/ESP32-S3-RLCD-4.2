// 验证 HTTP 连接、首包和总耗时使用单调时间并保留缺失阶段哨兵。
#include "http_request_timing.h"

#include <assert.h>
#include <stdint.h>

int main()
{
    HttpRequestTimingState state;
    http_request_timing_begin(&state, 1000000);
    HttpRequestTimingSnapshot pending = http_request_timing_snapshot(
        state,
        1500000);
    assert(pending.connected_ms == kHttpRequestTimingUnavailableMs);
    assert(pending.first_data_ms == kHttpRequestTimingUnavailableMs);
    assert(pending.elapsed_ms == 500);

    http_request_timing_note_connected(&state, 1600000);
    http_request_timing_note_connected(&state, 1700000);
    http_request_timing_note_first_data(&state, 2100000);
    http_request_timing_note_first_data(&state, 2200000);
    HttpRequestTimingSnapshot complete = http_request_timing_snapshot(
        state,
        2500000);
    assert(complete.connected_ms == 600);
    assert(complete.first_data_ms == 1100);
    assert(complete.elapsed_ms == 1500);

    HttpRequestTimingState zero_start;
    http_request_timing_begin(&zero_start, 0);
    http_request_timing_note_connected(&zero_start, 5000);
    assert(http_request_timing_snapshot(zero_start, 9000).connected_ms == 5);

    HttpRequestTimingState reversed;
    http_request_timing_begin(&reversed, 2000);
    http_request_timing_note_connected(&reversed, 1000);
    http_request_timing_note_first_data(&reversed, 1000);
    HttpRequestTimingSnapshot reversed_snapshot =
        http_request_timing_snapshot(reversed, 1000);
    assert(reversed_snapshot.connected_ms == kHttpRequestTimingUnavailableMs);
    assert(reversed_snapshot.first_data_ms == kHttpRequestTimingUnavailableMs);
    assert(reversed_snapshot.elapsed_ms == 0);

    assert(http_request_timing_phase(
               true,
               false,
               kHttpRequestTimingUnavailableMs,
               kHttpRequestTimingUnavailableMs) ==
           HttpRequestTimingPhase::kOk);
    assert(http_request_timing_phase(
               false,
               false,
               kHttpRequestTimingUnavailableMs,
               kHttpRequestTimingUnavailableMs) ==
           HttpRequestTimingPhase::kConnect);
    assert(http_request_timing_phase(
               false,
               true,
               kHttpRequestTimingUnavailableMs,
               kHttpRequestTimingUnavailableMs) ==
           HttpRequestTimingPhase::kReusedConnect);
    assert(http_request_timing_phase(false,
                                     false,
                                     120,
                                     kHttpRequestTimingUnavailableMs) ==
           HttpRequestTimingPhase::kFirstData);
    assert(http_request_timing_phase(false, false, 120, 300) ==
           HttpRequestTimingPhase::kResponse);

    assert(http_request_timing_delta_ms(0,
                                        static_cast<int64_t>(UINT32_MAX) *
                                                1000LL +
                                            1000LL) == UINT32_MAX);
    return 0;
}
