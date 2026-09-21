// 验证Open-Meteo天气映射、数组边界及US AQI数据语义。
#include "open_meteo_parser.h"
#include "weather_provider.h"
#include <cassert>
#include <cstring>
#include <string>
#include <fstream>
#include <iterator>

int main(int argc,char **argv)
{
    WeatherProvider provider=WeatherProvider::kQweather;
    assert(parse_weather_provider("open_meteo",&provider));
    assert(provider==WeatherProvider::kOpenMeteo);
    assert(!parse_weather_provider("unknown",&provider));
    assert(!parse_weather_provider(nullptr,&provider));
    const auto generation=weather_provider_generation();
    weather_provider_store(WeatherProvider::kOpenMeteo);
    assert(weather_provider_load()==WeatherProvider::kOpenMeteo);
    assert(generation!=weather_provider_generation());
    weather_provider_store(WeatherProvider::kQweather);
    const std::string good=R"({"utc_offset_seconds":28800,"current":{"time":"2026-09-19T12:00","temperature_2m":-2,"relative_humidity_2m":85,"weather_code":0,"is_day":0,"wind_speed_10m":5,"wind_direction_10m":45},"daily":{"time":["2026-09-19"],"temperature_2m_max":[5],"temperature_2m_min":[-3],"weather_code":[73],"sunrise":["2026-09-19T06:15"],"sunset":["2026-09-19T18:10"]}})";
    WeatherData w;WeatherForecastData f;
    assert(parse_open_meteo_weather(good.c_str(),&w,&f));
    assert(std::strcmp(w.temp,"-2")==0 && std::strcmp(w.icon,"150")==0);
    assert(f.ready && f.count==1 && std::strcmp(f.days[0].icon,"401")==0);
    assert(std::strcmp(f.days[0].wind_dir,"东北风")==0);
    assert(std::strcmp(f.days[0].wind_scale,"3")==0);
    assert(std::strcmp(f.days[0].sunrise,"06:15")==0);
    for(const auto &change : {std::pair<const char*,const char*>{"\"is_day\":0","\"is_day\":0.5"},
        {"\"weather_code\":0","\"weather_code\":null"},
        {"\"relative_humidity_2m\":85","\"relative_humidity_2m\":101"},
        {"\"temperature_2m_min\":[-3]","\"temperature_2m_min\":[]"},
        {"T18:10","T28:10"}, {"2026-09-19","2026-02-31"}}) {
        auto broken=good;broken.replace(broken.find(change.first),std::strlen(change.first),change.second);
        assert(!parse_open_meteo_weather(broken.c_str(),&w,&f));
    }
    assert(!parse_open_meteo_weather("{}",&w,&f));
    assert(!parse_open_meteo_weather("{\"error\":true}",&w,&f));
    WeatherAirData air;
    assert(parse_open_meteo_air("{\"current\":{\"us_aqi\":32,\"pm2_5\":8.2}}",&air));
    assert(air.ready && air.us_aqi && std::strcmp(air.aqi,"32")==0);
    assert(!parse_open_meteo_air("{\"current\":{\"us_aqi\":null,\"pm2_5\":8.2}}",&air));
    char text[32],icon[8];
    assert(open_meteo_weather_code(999,true,text,sizeof(text),icon,sizeof(icon)));
    assert(std::strcmp(icon,"999")==0);
    assert(open_meteo_weather_code(61,true,text,sizeof(text),icon,sizeof(icon)));
    assert(std::strcmp(icon,"305")==0);
    assert(!open_meteo_weather_code(95,true,text,1,icon,sizeof(icon)));
    if(argc==2) {
        std::ifstream file(argv[1]);
        assert(file.good());
        std::string live((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
        assert(parse_open_meteo_weather(live.c_str(),&w,&f));
        assert(f.count==kWeatherForecastDays);
    }
}
