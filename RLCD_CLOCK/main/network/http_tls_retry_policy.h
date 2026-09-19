// 定义 HTTPS 请求的证书信任顺序和连接失败重试边界。
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class HttpTlsTrustMode : uint8_t {
    kCertificateBundle = 0,
    kQweatherLegacyCa,
};

constexpr size_t http_tls_attempt_count(bool qweather_url)
{
    return qweather_url ? 2U : 1U;
}

constexpr HttpTlsTrustMode http_tls_trust_mode(bool qweather_url, size_t attempt)
{
    return qweather_url && attempt > 0
               ? HttpTlsTrustMode::kQweatherLegacyCa
               : HttpTlsTrustMode::kCertificateBundle;
}

constexpr bool http_tls_should_retry(bool qweather_url,
                                     size_t attempt,
                                     bool connection_failed,
                                     bool certificate_verification_failed)
{
    return connection_failed && certificate_verification_failed &&
           attempt < http_tls_attempt_count(qweather_url) - 1U;
}
