// 聚合天气主题纯判断：依据缓存天气代码区分晴、雨和雪。
#pragma once
#include <cstring>
inline int aggregate_weather_kind(const char *code) {
    if(!code || std::strlen(code)!=3) return 0;
    int value=0;
    for(int i=0;i<3;++i) {if(code[i]<'0'||code[i]>'9')return 0;value=value*10+code[i]-'0';}
    // Match the rain codes supported by the existing QWeather icon map.
    if((value>=300 && value<=318) || value==350 || value==351 || value==399)return 2;
    if((value>=400 && value<=410) || value==456 || value==457 || value==499)return 3;
    // 150 is clear night: keep its moon icon but never add solar rays.
    if(value==100)return 1;
    return 0;
}
