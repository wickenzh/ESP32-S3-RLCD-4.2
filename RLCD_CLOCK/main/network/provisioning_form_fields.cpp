// 解析配网页字段别名并规范化需要去除空白的文本。
#include "provisioning_form_fields.h"

#include "ascii_text.h"
#include "network_form.h"

namespace {
constexpr size_t max_size(size_t a, size_t b)
{
    return a > b ? a : b;
}

constexpr size_t kMaxProvisioningFormFieldSize =
    max_size(max_size(max_size(kProvisioningManualTimeFieldSize, kProvisioningSsidFieldSize),
                      max_size(kProvisioningPasswordFieldSize,
                               max_size(kProvisioningApiKeyFieldSize,
                                        kProvisioningApiHostFieldSize))),
             kProvisioningWeatherCityFieldSize);
constexpr const char *kFormManualTimeKey = "manual_time";
constexpr const char *kFormManualTimeFallbackKey = "datetime";
constexpr const char *kFormSsidKey = "ssid";
constexpr const char *kFormPasswordKey = "pass";
constexpr const char *kFormPasswordFallbackKey = "password";
constexpr const char *kFormBackupSsidKey = "backup_ssid";
constexpr const char *kFormBackupPasswordKey = "backup_pass";
constexpr const char *kFormApiKeyKey = "api_key";
constexpr const char *kFormApiKeyFallbackKey = "weather";
constexpr const char *kFormApiHostKey = "api_host";
constexpr const char *kFormApiHostFallbackKey = "weather_host";
constexpr const char *kFormWeatherCityKey = "weather_city";
constexpr const char *kFormWeatherCityFallbackKey = "city";
void form_value_fallback_trimmed(const char *body,
                                 const char *primary_key,
                                 const char *fallback_key,
                                 char *out,
                                 size_t out_len)
{
    form_value_fallback(body, primary_key, fallback_key, out, out_len);
    trim_ascii_whitespace(out);
}

static_assert(kNetworkFormEncodedBufferSize >= kMaxProvisioningFormFieldSize,
              "form encoded scratch buffer must fit the largest setup field");
static_assert(kProvisioningManualTimeFieldSize > 1, "manual time field must fit text and NUL");
static_assert(kProvisioningSsidFieldSize > 1, "setup SSID field must fit text and NUL");
static_assert(kProvisioningPasswordFieldSize > 1, "setup password field must fit text and NUL");
static_assert(kProvisioningApiKeyFieldSize > 1, "setup API key field must fit text and NUL");
static_assert(kProvisioningApiHostFieldSize == kQweatherApiHostLen,
              "setup API Host field must match runtime Host buffer");
static_assert(kProvisioningWeatherCityFieldSize == kManualWeatherCityLen,
              "setup weather city field must match runtime city buffer");
} // namespace

void read_provisioning_form_fields(const char *body, ProvisioningFormFields *fields)
{
    if (!fields) {
        return;
    }
    form_value(body, kFormSsidKey, fields->ssid, sizeof(fields->ssid));
    form_value(body, "weather_provider", fields->weather_provider, sizeof(fields->weather_provider));
    form_value_fallback(body, kFormPasswordKey, kFormPasswordFallbackKey, fields->pass, sizeof(fields->pass));
    form_value(body,
               kFormBackupSsidKey,
               fields->backup_ssid,
               sizeof(fields->backup_ssid));
    form_value(body,
               kFormBackupPasswordKey,
               fields->backup_pass,
               sizeof(fields->backup_pass));
    form_value_fallback_trimmed(body,
                                kFormApiKeyKey,
                                kFormApiKeyFallbackKey,
                                fields->api_key,
                                sizeof(fields->api_key));
    form_value_fallback_trimmed(body,
                                kFormApiHostKey,
                                kFormApiHostFallbackKey,
                                fields->api_host,
                                sizeof(fields->api_host));
    form_value_fallback_trimmed(body,
                                kFormWeatherCityKey,
                                kFormWeatherCityFallbackKey,
                                fields->weather_city,
                                sizeof(fields->weather_city));
}

void read_provisioning_manual_time(const char *body, char *out, size_t out_len)
{
    form_value_fallback_trimmed(body, kFormManualTimeKey, kFormManualTimeFallbackKey, out, out_len);
}
