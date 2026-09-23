// 串行获取Open-Meteo模型天气与空气质量，复用现有缓存和网络保护。
#include "open_meteo_client.h"
#include "open_meteo_parser.h"
#include "weather_provider.h"
#include "weather_state_internal.h"
#include "manual_weather_city_state.h"
#include "network_http_client.h"
#include "network_json_root.h"
#include "network_url.h"
#include "network_sync_runtime.h"
#include "ip_geolocation_client.h"
#include "scoped_heap_buffer.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t kResponseBytes=8192;
struct Workspace { WeatherData weather; WeatherForecastData forecast; WeatherAirData air; };
EXT_RAM_BSS_ATTR Workspace workspace;
struct CityCache { char query[32]; WeatherData location; int64_t expires_us; };
EXT_RAM_BSS_ATTR CityCache city_cache;

bool coordinates(const char *lat,const char *lon)
{
    char *end=nullptr;
    const double a=std::strtod(lat,&end);
    if(end==lat || *end || !std::isfinite(a) || a<-90 || a>90)return false;
    const double b=std::strtod(lon,&end);
    return end!=lon && !*end && std::isfinite(b) && b>=-180 && b<=180;
}
bool resolve_location(WeatherData *out)
{
    char city[kManualWeatherCityLen]={};
    (void)manual_weather_city_snapshot(city,sizeof(city));
    if(city[0]) {
        if(city_cache.expires_us>esp_timer_get_time() && std::strcmp(city_cache.query,city)==0) {
            *out=city_cache.location;return true;
        }
        if(open_meteo_lookup_city(city,out)!=OpenMeteoCityStatus::kOk)return false;
        std::snprintf(city_cache.query,sizeof(city_cache.query),"%s",city);
        city_cache.location=*out;
        city_cache.expires_us=esp_timer_get_time()+24LL*60*60*1000000;
        return true;
    }
    char pair[40]={};
    if(!ip_geolocation_lookup_cached(pair,sizeof(pair),out->city,sizeof(out->city)))return false;
    const char *comma=std::strchr(pair,',');
    if(!comma || std::strchr(comma+1,','))return false;
    const size_t n=static_cast<size_t>(comma-pair);
    if(n==0 || n>=sizeof(out->lon) || std::strlen(comma+1)>=sizeof(out->lat))return false;
    std::memcpy(out->lon,pair,n);out->lon[n]='\0';
    std::snprintf(out->lat,sizeof(out->lat),"%s",comma+1);
    return coordinates(out->lat,out->lon);
}
bool fetch_forecast(WeatherData *weather,WeatherForecastData *forecast)
{
    if(!coordinates(weather->lat,weather->lon))return false;
    char url[640];
    const int n=std::snprintf(url,sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&timezone=Asia%%2FShanghai&wind_speed_unit=ms&forecast_days=6&current=temperature_2m,relative_humidity_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset",
        weather->lat,weather->lon);
    if(n<0 || static_cast<size_t>(n)>=sizeof(url))return false;
    ScopedHeapBuffer<char> response(kResponseBytes,HeapBufferInit::kUninitialized,HeapBufferStorage::kPsramPreferred);
    return response && http_get_text(url,response.get(),response.size())==ESP_OK &&
           parse_open_meteo_weather(response.get(),weather,forecast);
}
bool fetch_air(const WeatherData &weather,WeatherAirData *air)
{
    char url[256];
    const int n=std::snprintf(url,sizeof(url),
        "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s&current=us_aqi,pm2_5&timezone=Asia%%2FShanghai",
        weather.lat,weather.lon);
    if(n<0 || static_cast<size_t>(n)>=sizeof(url))return false;
    ScopedHeapBuffer<char> response(2048,HeapBufferInit::kUninitialized,HeapBufferStorage::kPsramPreferred);
    return response && http_get_text(url,response.get(),response.size())==ESP_OK &&
           parse_open_meteo_air(response.get(),air);
}
}

OpenMeteoCityStatus open_meteo_lookup_city(const char *city,WeatherData *location)
{
    if(!city||!*city||!location||std::strlen(city)>=sizeof(city_cache.query))return OpenMeteoCityStatus::kFailed;
    // MCP lookup owns its response; only the network task uses the location cache.
    char encoded[128],url[256];
    if(!url_encode_component(city,encoded,sizeof(encoded)))return OpenMeteoCityStatus::kFailed;
    const int n=std::snprintf(url,sizeof(url),"https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=zh&format=json",encoded);
    if(n<0||static_cast<size_t>(n)>=sizeof(url))return OpenMeteoCityStatus::kFailed;
    ScopedHeapBuffer<char> response(3072,HeapBufferInit::kUninitialized,HeapBufferStorage::kPsramPreferred);
    if(!response||http_get_text(url,response.get(),response.size())!=ESP_OK)return OpenMeteoCityStatus::kFailed;
    NetworkJsonRoot root(response.get());
    if(!root || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"error")))return OpenMeteoCityStatus::kFailed;
    const cJSON *results=cJSON_GetObjectItemCaseSensitive(root.get(),"results");
    if(!results || (cJSON_IsArray(results)&&cJSON_GetArraySize(results)==0))return OpenMeteoCityStatus::kNotFound;
    const cJSON *item=cJSON_GetArrayItem(results,0);
    const cJSON *lat=cJSON_GetObjectItemCaseSensitive(item,"latitude");
    const cJSON *lon=cJSON_GetObjectItemCaseSensitive(item,"longitude");
    const cJSON *name=cJSON_GetObjectItemCaseSensitive(item,"name");
    if(!cJSON_IsNumber(lat)||!cJSON_IsNumber(lon)||!cJSON_IsString(name))return OpenMeteoCityStatus::kFailed;
    std::snprintf(location->lat,sizeof(location->lat),"%.5f",lat->valuedouble);
    std::snprintf(location->lon,sizeof(location->lon),"%.5f",lon->valuedouble);
    if(!coordinates(location->lat,location->lon))return OpenMeteoCityStatus::kFailed;
    const char *display=std::strlen(name->valuestring)<sizeof(location->city)?name->valuestring:city;
    std::snprintf(location->city,sizeof(location->city),"%s",display);
    return OpenMeteoCityStatus::kOk;
}

bool open_meteo_validate_configuration()
{
    workspace={};
    return resolve_location(&workspace.weather) && fetch_forecast(&workspace.weather,&workspace.forecast);
}

WeatherUpdateResult perform_open_meteo_update(WeatherUpdateScope scope)
{
    const uint32_t generation=weather_provider_generation();
    if(!network_sync_continuation_allowed())return WeatherUpdateResult::kFailed;
    workspace={};
    if(!resolve_location(&workspace.weather) || !network_sync_continuation_allowed() ||
       generation!=weather_provider_generation() || !fetch_forecast(&workspace.weather,&workspace.forecast))return WeatherUpdateResult::kFailed;
    workspace.weather.open_meteo=true;
    workspace.weather.configuration_generation=generation;
    bool air_ok=false;
    if(scope==WeatherUpdateScope::kFull && network_sync_continuation_allowed() && generation==weather_provider_generation()) {
        air_ok=fetch_air(workspace.weather,&workspace.air);
    }
    if(!network_sync_continuation_allowed() || generation!=weather_provider_generation())return WeatherUpdateResult::kFailed;
    const WeatherAlertData unavailable={};
    commit_weather_update_snapshot(workspace.weather,unavailable,workspace.forecast,workspace.air,true,true,air_ok);
    return WeatherUpdateResult::kSuccess;
}
