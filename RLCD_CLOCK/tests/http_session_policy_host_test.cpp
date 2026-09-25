#include "http_session_policy.h"

#include <cassert>

int main()
{
    assert(http_urls_share_origin("https://api.example.com/a",
                                  "https://API.example.com/b?x=1"));
    assert(http_urls_share_origin("https://api.example.com:443/a",
                                  "https://api.example.com:443/b"));
    assert(!http_urls_share_origin("https://api.example.com/a",
                                   "http://api.example.com/a"));
    assert(!http_urls_share_origin("https://api.example.com/a",
                                   "https://other.example.com/a"));
    assert(!http_urls_share_origin("https://api.example.com/a",
                                   "https://api.example.com:8443/a"));
    assert(!http_urls_share_origin("not-a-url", "https://api.example.com"));

    assert(http_session_retry_action(
               true, true, false, true, false, false,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kReconnectSameTrust);
    assert(http_session_retry_action(
               true, false, false, true, false, true,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kRetryQweatherLegacyCa);
    assert(http_session_retry_action(
               true, false, true, true, false, true,
               HttpTlsTrustMode::kQweatherLegacyCa) ==
           HttpSessionRetryAction::kStop);
    assert(http_session_retry_action(
               false, false, false, true, false, true,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kStop);

    assert(http_session_retry_action(
               true, false, false, true, true, false,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kRetryQweatherTransient);
    assert(http_session_retry_action(
               true, false, false, false, true, false,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kRetryQweatherTransient);
    assert(http_session_retry_action(
               true, false, true, true, true, false,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kStop);
    assert(http_session_retry_action(
               false, false, false, true, true, false,
               HttpTlsTrustMode::kCertificateBundle) ==
           HttpSessionRetryAction::kStop);

    assert(http_session_attempt_timeout_ms(10000, false) == 10000);
    assert(http_session_attempt_timeout_ms(10000, true) ==
           kQweatherTransientRetryTimeoutMs);
    assert(http_session_attempt_timeout_ms(2000, true) == 2000);
    assert(http_session_first_qweather_timeout_ms(10000, false, 5000) ==
           10000);
    assert(http_session_first_qweather_timeout_ms(4900, true, 5000) == 4900);
    assert(http_session_first_qweather_timeout_ms(5000, true, 5000) == 5000);
    assert(http_session_first_qweather_timeout_ms(5100, true, 5000) == 5000);
    assert(http_session_first_qweather_timeout_ms(10000, true, 0) == 10000);
    return 0;
}
