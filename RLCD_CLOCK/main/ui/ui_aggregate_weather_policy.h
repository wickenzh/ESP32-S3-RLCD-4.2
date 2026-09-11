// 聚合天气主题纯判断：只依据缓存天气代码区分晴雨。
#pragma once
#include <cstring>
inline int aggregate_weather_kind(const char *code) {
    if(!code || std::strlen(code)!=3) return 0;
    int value=0;
    for(int i=0;i<3;++i) {if(code[i]<'0'||code[i]>'9')return 0;value=value*10+code[i]-'0';}
    if(value>=300 && value<400)return 2;
    if(value==100 || value==150)return 1;
    return 0;
}
