// 定义 HTTP keep-alive 同源判断和有界重建策略。
#pragma once

#include "http_tls_retry_policy.h"

enum class HttpSessionRetryAction {
    kStop = 0,
    kReconnectSameTrust,
    kRetryQweatherTransient,
    kRetryQweatherLegacyCa,
};

inline constexpr int kQweatherTransientRetryTimeoutMs = 3000;
inline constexpr int kQweatherTimeoutAbCandidateMs = 5000;

constexpr int http_session_attempt_timeout_ms(int timeout_ms,
                                              bool transient_retry)
{
    return transient_retry && timeout_ms > kQweatherTransientRetryTimeoutMs
               ? kQweatherTransientRetryTimeoutMs
               : timeout_ms;
}

constexpr int http_session_first_qweather_timeout_ms(
    int timeout_ms,
    bool first_qweather_attempt,
    int configured_cap_ms)
{
    return first_qweather_attempt && configured_cap_ms > 0 &&
                   timeout_ms > configured_cap_ms
               ? configured_cap_ms
               : timeout_ms;
}

static_assert(kQweatherTimeoutAbCandidateMs >
                  kQweatherTransientRetryTimeoutMs,
              "candidate first timeout must exceed the recovery retry cap");

bool http_urls_share_origin(const char *left, const char *right);

constexpr HttpSessionRetryAction http_session_retry_action(
    bool qweather_url,
    bool used_existing_client,
    bool reconnect_attempted,
    bool connection_failed,
    bool transient_timeout,
    bool certificate_verification_failed,
    HttpTlsTrustMode trust_mode)
{
    if (connection_failed && used_existing_client && !reconnect_attempted) {
        return HttpSessionRetryAction::kReconnectSameTrust;
    }
    if (qweather_url && transient_timeout && !reconnect_attempted) {
        return HttpSessionRetryAction::kRetryQweatherTransient;
    }
    if (qweather_url && connection_failed && certificate_verification_failed &&
        trust_mode == HttpTlsTrustMode::kCertificateBundle) {
        return HttpSessionRetryAction::kRetryQweatherLegacyCa;
    }
    return HttpSessionRetryAction::kStop;
}
