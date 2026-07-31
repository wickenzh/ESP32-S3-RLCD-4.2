// 声明 OTA manifest 数据结构、纯 JSON 解析结果和解析入口。
#pragma once

#include "app_state.h"

#include <stddef.h>

struct OtaManifest {
    char version[kOtaVersionLen] = {};
    char url[kOtaUrlLen] = {};
    char sha256[kOtaSha256Len] = {};
    int size = 0;
};

enum OtaManifestParseStatus {
    kOtaManifestParseOk = 0,
    kOtaManifestParseInvalidArgument,
    kOtaManifestParseInvalidJson,
    kOtaManifestParseMissingRequiredFields,
    kOtaManifestParseInvalidSha256,
};

struct OtaManifestParseResult {
    OtaManifestParseStatus status = kOtaManifestParseInvalidArgument;
    bool have_version = false;
    bool have_url = false;
    bool have_sha256 = false;
    size_t sha256_length = 0;
};

OtaManifestParseResult ota_parse_manifest_json(const char *json, OtaManifest *manifest);
