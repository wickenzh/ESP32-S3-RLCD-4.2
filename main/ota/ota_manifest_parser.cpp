// 实现 OTA manifest 的纯 JSON 字段解析和 SHA256 格式校验。
#include "ota_manifest_parser.h"

#include "network_json.h"
#include "network_json_root.h"
#include "ota_validation.h"

#include "cJSON.h"
#include <string.h>

namespace {
constexpr const char *kVersionField = "version";
constexpr const char *kUrlField = "url";
constexpr const char *kSha256Field = "sha256";
constexpr const char *kSizeField = "size";
static_assert(kOtaSha256HexLen + 1 == kOtaSha256Len,
              "OTA SHA256 storage must fit hex text and NUL");
} // namespace

OtaManifestParseResult ota_parse_manifest_json(const char *json, OtaManifest *manifest)
{
    OtaManifestParseResult result = {};
    if (!json || !manifest) {
        return result;
    }

    *manifest = OtaManifest{};

    NetworkJsonRoot root(json);
    if (!root) {
        result.status = kOtaManifestParseInvalidJson;
        return result;
    }

    result.have_version = json_copy_string(root.get(),
                                           kVersionField,
                                           manifest->version,
                                           sizeof(manifest->version)) &&
                          manifest->version[0] != '\0';
    result.have_url = json_copy_string(root.get(),
                                       kUrlField,
                                       manifest->url,
                                       sizeof(manifest->url)) &&
                      manifest->url[0] != '\0';
    result.have_sha256 = json_copy_string(root.get(),
                                          kSha256Field,
                                          manifest->sha256,
                                          sizeof(manifest->sha256));
    const cJSON *size = cJSON_GetObjectItem(root.get(), kSizeField);
    if (cJSON_IsNumber(size)) {
        manifest->size = size->valueint;
    }

    if (!result.have_version || !result.have_url || !result.have_sha256) {
        result.status = kOtaManifestParseMissingRequiredFields;
        return result;
    }
    result.sha256_length = strlen(manifest->sha256);
    if (!ota_valid_sha256_string(manifest->sha256)) {
        result.status = kOtaManifestParseInvalidSha256;
        return result;
    }
    result.status = kOtaManifestParseOk;
    return result;
}
