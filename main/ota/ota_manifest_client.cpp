// 获取自定义、R2 与 GitHub OTA manifest，并维护安装前的运行态清单缓存。
#include "ota_manifest_client.h"

#include "app_constexpr.h"
#include "app_text_format.h"
#include "custom_assets.h"
#include "network_services.h"
#include "ota_validation.h"
#include "scoped_heap_buffer.h"

namespace {
constexpr size_t kManifestResponseBufferSize = 2048;
constexpr const char *kManifestSourceR2 = "R2";
constexpr const char *kManifestSourceGithub = "GitHub";
constexpr const char *kManifestSourceCustom = "Custom";
constexpr int kBuiltInManifestSourceCount = 2;
constexpr int kBackupManifestSourceIndex = 1;
constexpr OtaManifestSource kBuiltInManifestSources[] = {
    {kManifestSourceR2, kOtaManifestUrl},
    {kManifestSourceGithub, kOtaBackupManifestUrl},
};
OtaManifest s_cached_manifest;
constexpr const char *kManifestParseInvalidArgLog = "OTA manifest parse invalid arg";
constexpr const char *kManifestJsonParseFailedLog = "OTA manifest JSON parse failed";
#define MANIFEST_MISSING_REQUIRED_FIELDS_FORMAT "OTA manifest missing required fields version=%d url=%d sha=%d"
#define MANIFEST_SHA_INVALID_FORMAT "OTA manifest sha invalid len=%u"
#define MANIFEST_SOURCE_SKIPPED_FORMAT "OTA manifest source skipped: %s"
constexpr const char *kManifestResponseAllocFailedLog = "OTA manifest response alloc failed";
#define MANIFEST_FETCH_FAILED_FORMAT "OTA manifest failed source=%s err=%s"
#define MANIFEST_PARSE_FAILED_FORMAT "OTA manifest parse failed source=%s"
#define MANIFEST_LOADED_FORMAT "OTA manifest loaded source=%s version=%s"
#define BACKUP_MANIFEST_MISMATCH_FORMAT "OTA backup manifest mismatch current=%s backup=%s"
constexpr bool manifest_source_name_fits(const char *text)
{
    return cstr_nonempty(text) && cstr_length(text) < kOtaManifestSourceNameLen;
}

static_assert(kManifestResponseBufferSize > 1,
              "OTA manifest response buffer must fit text and NUL");
static_assert(array_count(kBuiltInManifestSources) == kBuiltInManifestSourceCount,
              "OTA built-in manifest source list must cover R2 and GitHub");
static_assert(kBackupManifestSourceIndex >= 0 &&
                  kBackupManifestSourceIndex < kBuiltInManifestSourceCount,
              "OTA backup manifest source index must stay within built-in source list");
static_assert(manifest_source_name_fits(kManifestSourceR2),
              "R2 OTA manifest source name must fit UI storage");
static_assert(manifest_source_name_fits(kManifestSourceGithub),
              "GitHub OTA manifest source name must fit UI storage");
static_assert(manifest_source_name_fits(kManifestSourceCustom),
              "custom OTA manifest source name must fit UI storage");
static_assert(manifest_source_name_fits(kOtaUnknownManifestSource),
              "unknown OTA manifest source name must fit UI storage");

void notify_manifest_failure(OtaManifestFailureCallback callback)
{
    if (callback) {
        callback();
    }
}

bool parse_manifest_with_log(const char *json, OtaManifest *manifest)
{
    OtaManifestParseResult result = ota_parse_manifest_json(json, manifest);
    switch (result.status) {
    case kOtaManifestParseOk:
        return true;
    case kOtaManifestParseInvalidArgument:
        ESP_LOGW(TAG, "%s", kManifestParseInvalidArgLog);
        break;
    case kOtaManifestParseInvalidJson:
        ESP_LOGW(TAG, "%s", kManifestJsonParseFailedLog);
        break;
    case kOtaManifestParseMissingRequiredFields:
        ESP_LOGW(TAG,
                 MANIFEST_MISSING_REQUIRED_FIELDS_FORMAT,
                 result.have_version,
                 result.have_url,
                 result.have_sha256);
        break;
    case kOtaManifestParseInvalidSha256:
        ESP_LOGW(TAG, MANIFEST_SHA_INVALID_FORMAT, (unsigned)result.sha256_length);
        break;
    }
    return false;
}

bool fetch_manifest_from_source(const OtaManifestSource &source,
                                OtaManifest *manifest,
                                OtaManifestFailureCallback failure_callback)
{
    if (!manifest) {
        notify_manifest_failure(failure_callback);
        return false;
    }
    if (!ota_manifest_source_valid(source)) {
        ESP_LOGW(TAG,
                 MANIFEST_SOURCE_SKIPPED_FORMAT,
                 ota_manifest_source_name_or_unknown(source.name));
        return false;
    }
    ScopedHeapBuffer<char> response(kManifestResponseBufferSize,
                                    HeapBufferInit::kCString);
    if (!response) {
        ESP_LOGW(TAG, "%s", kManifestResponseAllocFailedLog);
        notify_manifest_failure(failure_callback);
        return false;
    }
    esp_err_t err = http_get_text(source.url, response.data(), response.size());
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 MANIFEST_FETCH_FAILED_FORMAT,
                 ota_manifest_source_name_or_unknown(source.name),
                 esp_err_to_name(err));
        return false;
    }
    if (!parse_manifest_with_log(response.data(), manifest)) {
        ESP_LOGW(TAG,
                 MANIFEST_PARSE_FAILED_FORMAT,
                 ota_manifest_source_name_or_unknown(source.name));
        return false;
    }
    ESP_LOGI(TAG,
             MANIFEST_LOADED_FORMAT,
             ota_manifest_source_name_or_unknown(source.name),
             manifest->version);
    return true;
}

void store_manifest_source_name(char *out, size_t out_len, const char *name)
{
    if (!app_text::output_buffer_available(out, out_len)) {
        return;
    }
    strlcpy(out, ota_manifest_source_name_or_unknown(name), out_len);
}
} // namespace

void ota_manifest_load_cached(OtaManifest *manifest)
{
    if (!manifest) {
        return;
    }
    strlcpy(manifest->version, s_cached_manifest.version, sizeof(manifest->version));
    strlcpy(manifest->url, s_cached_manifest.url, sizeof(manifest->url));
    strlcpy(manifest->sha256, s_cached_manifest.sha256, sizeof(manifest->sha256));
    manifest->size = s_cached_manifest.size;
}

void ota_manifest_store_cached(const OtaManifest &manifest)
{
    strlcpy(s_cached_manifest.version, manifest.version, sizeof(s_cached_manifest.version));
    strlcpy(s_cached_manifest.url, manifest.url, sizeof(s_cached_manifest.url));
    strlcpy(s_cached_manifest.sha256, manifest.sha256, sizeof(s_cached_manifest.sha256));
    s_cached_manifest.size = manifest.size;
}

bool ota_manifest_fetch(OtaManifest *manifest,
                        char *source_name,
                        size_t source_name_len,
                        OtaManifestFailureCallback failure_callback)
{
    char custom_url[kOtaUrlLen] = {};
    if (custom_assets_read_ota_manifest_url(custom_url, sizeof(custom_url))) {
        OtaManifestSource custom_source = {kManifestSourceCustom, custom_url};
        if (fetch_manifest_from_source(custom_source, manifest, failure_callback)) {
            store_manifest_source_name(source_name, source_name_len, custom_source.name);
            return true;
        }
    }
    for (const OtaManifestSource &source : kBuiltInManifestSources) {
        if (fetch_manifest_from_source(source, manifest, failure_callback)) {
            store_manifest_source_name(source_name, source_name_len, source.name);
            return true;
        }
    }
    notify_manifest_failure(failure_callback);
    return false;
}

bool ota_manifest_fetch_backup_for_install(const OtaManifest &current,
                                           OtaManifest *backup,
                                           OtaManifestFailureCallback failure_callback)
{
    if (!backup || current.version[0] == '\0' ||
        !ota_valid_sha256_string(current.sha256)) {
        return false;
    }
    OtaManifest candidate;
    const OtaManifestSource &backup_source =
        kBuiltInManifestSources[kBackupManifestSourceIndex];
    if (!fetch_manifest_from_source(backup_source, &candidate, failure_callback)) {
        return false;
    }
    if (!ota_backup_manifest_metadata_matches(current.version,
                                              current.sha256,
                                              current.size,
                                              candidate.version,
                                              candidate.sha256,
                                              candidate.size)) {
        ESP_LOGW(TAG,
                 BACKUP_MANIFEST_MISMATCH_FORMAT,
                 current.version,
                 candidate.version);
        return false;
    }
    *backup = candidate;
    return true;
}
