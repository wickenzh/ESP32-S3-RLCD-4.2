// 声明第三方 IP 定位查询的轻量客户端接口。
#pragma once

#include <stddef.h>

bool ip_geolocation_lookup(char *location,
                           size_t location_len,
                           char *city,
                           size_t city_len);
bool ip_geolocation_lookup_cached(char *location,
                                  size_t location_len,
                                  char *city,
                                  size_t city_len);
void ip_geolocation_cache_invalidate();
