// 比较 HTTP URL 的 scheme、host 和显式端口，不解析路径与查询参数。
#include "http_session_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

namespace {
struct HttpOriginRange {
    const char *begin = nullptr;
    size_t length = 0;
};

bool parse_origin(const char *url, HttpOriginRange *out)
{
    if (!url || !out) {
        return false;
    }
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end || scheme_end == url) {
        return false;
    }
    const char *authority = scheme_end + 3;
    if (*authority == '\0') {
        return false;
    }
    const char *end = authority;
    while (*end && *end != '/' && *end != '?' && *end != '#') {
        ++end;
    }
    if (end == authority) {
        return false;
    }
    out->begin = url;
    out->length = static_cast<size_t>(end - url);
    return true;
}

bool equal_ascii_case_insensitive(const HttpOriginRange &left,
                                  const HttpOriginRange &right)
{
    if (left.length != right.length) {
        return false;
    }
    for (size_t i = 0; i < left.length; ++i) {
        const unsigned char lhs = static_cast<unsigned char>(left.begin[i]);
        const unsigned char rhs = static_cast<unsigned char>(right.begin[i]);
        if (tolower(lhs) != tolower(rhs)) {
            return false;
        }
    }
    return true;
}
} // namespace

bool http_urls_share_origin(const char *left, const char *right)
{
    HttpOriginRange left_origin;
    HttpOriginRange right_origin;
    return parse_origin(left, &left_origin) &&
           parse_origin(right, &right_origin) &&
           equal_ascii_case_insensitive(left_origin, right_origin);
}
