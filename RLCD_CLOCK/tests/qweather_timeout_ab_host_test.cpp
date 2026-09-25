// 验证 QWeather 首请求超时 A/B 的 12 小时边界、交替模式和资格门控。
#include "qweather_timeout_ab_test.h"

#include <assert.h>

int main()
{
    assert(qweather_timeout_ab_slot(-1) == -1);
    assert(qweather_timeout_ab_slot(0) == 0);
    assert(qweather_timeout_ab_slot(kQweatherTimeoutAbSlotDurationUs - 1) ==
           0);
    assert(qweather_timeout_ab_slot(kQweatherTimeoutAbSlotDurationUs) == 1);
    assert(qweather_timeout_ab_slot(kQweatherTimeoutAbDurationUs - 1) ==
           static_cast<int>(kQweatherTimeoutAbDurationUs /
                            kQweatherTimeoutAbSlotDurationUs) -
               1);
    assert(qweather_timeout_ab_slot(kQweatherTimeoutAbDurationUs) == -1);

    const QweatherTimeoutAbDecision baseline =
        qweather_timeout_ab_decision(0, true);
    assert(baseline.slot == 0);
    assert(!baseline.candidate);
    assert(baseline.first_request_timeout_ms == 0);

    const QweatherTimeoutAbDecision candidate =
        qweather_timeout_ab_decision(kQweatherTimeoutAbSlotDurationUs, true);
    assert(candidate.slot == 1);
    assert(candidate.candidate);
    assert(candidate.first_request_timeout_ms ==
           kQweatherTimeoutAbCandidateMs);

    const QweatherTimeoutAbDecision ineligible =
        qweather_timeout_ab_decision(kQweatherTimeoutAbSlotDurationUs, false);
    assert(ineligible.slot == 1);
    assert(ineligible.candidate);
    assert(ineligible.first_request_timeout_ms == 0);

    const QweatherTimeoutAbDecision finished =
        qweather_timeout_ab_decision(kQweatherTimeoutAbDurationUs, true);
    assert(finished.slot == -1);
    assert(!finished.candidate);
    assert(finished.first_request_timeout_ms == 0);
    return 0;
}
