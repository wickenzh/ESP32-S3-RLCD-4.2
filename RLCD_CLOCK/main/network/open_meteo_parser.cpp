// 解析模型天气与US AQI，拒绝无效数值及不一致的每日数组。
#include "open_meteo_parser.h"
#include "network_json_root.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
bool number(const cJSON *object, const char *key, double low, double high, double *out)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < low || value->valuedouble > high) return false;
    *out = value->valuedouble;
    return true;
}
bool array_number(const cJSON *array, int i, double low, double high, double *out)
{
    const cJSON *value = cJSON_GetArrayItem(array, i);
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < low || value->valuedouble > high) return false;
    *out = value->valuedouble;
    return true;
}
bool date_valid(const char *s)
{
    if (!s || std::strlen(s) != 10 || s[4] != '-' || s[7] != '-') return false;
    for (int i=0;i<10;++i) if(i!=4 && i!=7 && (s[i]<'0'||s[i]>'9')) return false;
    int year=0,month=0,day=0;
    if(std::sscanf(s,"%d-%d-%d",&year,&month,&day)!=3 || year<2024 || year>2099 || month<1 || month>12) return false;
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    const int max_day=days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0));
    return day>=1 && day<=max_day;
}
int date_ordinal(const char *date)
{
    int year=0,month=0,day=0;
    std::sscanf(date,"%d-%d-%d",&year,&month,&day);
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int total=day;
    for(int y=2024;y<year;++y)total+=365+(y%4==0 && (y%100!=0||y%400==0));
    for(int m=1;m<month;++m)total+=days[m-1]+(m==2 && year%4==0 && (year%100!=0||year%400==0));
    return total;
}
bool sun_time(const cJSON *array,int i,const char *date,char *out,size_t size,int8_t *day_offset)
{
    const cJSON *value=cJSON_GetArrayItem(array,i);
    if(cJSON_IsNull(value)) {std::snprintf(out,size,"--:--");return true;}
    if(!cJSON_IsString(value) || std::strlen(value->valuestring)!=16 || value->valuestring[10]!='T') return false;
    char date_prefix[11]={};
    std::memcpy(date_prefix,value->valuestring,10);
    if(!date_valid(date_prefix))return false;
    const int offset=date_ordinal(date_prefix)-date_ordinal(date);
    if(offset < -1 || offset > 1)return false;
    *day_offset=static_cast<int8_t>(offset);
    const char *s=value->valuestring+11;
    if(s[2]!=':' || s[0]<'0'||s[0]>'2'||s[1]<'0'||s[1]>'9'||s[3]<'0'||s[3]>'5'||s[4]<'0'||s[4]>'9')return false;
    if((s[0]-'0')*10+s[1]-'0'>23)return false;
    std::snprintf(out,size,"%s",s);return true;
}
}

bool open_meteo_weather_code(int code,bool daylight,char *text,size_t text_size,char *icon,size_t icon_size)
{
    const char *label="未知"; int mapped=999;
    switch(code){
    case 0:label="晴";mapped=daylight?100:150;break;
    case 1:case 2:label="多云";mapped=daylight?101:151;break;
    case 3:label="阴";mapped=104;break;
    case 45:case 48:label="雾";mapped=501;break;
    case 51:case 53:case 55:label="毛毛雨";mapped=309;break;
    case 56:case 57:case 66:case 67:label="冻雨";mapped=313;break;
    case 61:case 80:label="小雨";mapped=305;break;
    case 63:case 81:label="中雨";mapped=306;break;
    case 65:case 82:label="大雨";mapped=307;break;
    case 71:case 77:case 85:label="小雪";mapped=400;break;
    case 73:label="中雪";mapped=401;break;
    case 75:case 86:label="大雪";mapped=402;break;
    case 95:label="雷阵雨";mapped=302;break;
    case 96:case 99:label="雷阵雨伴冰雹";mapped=304;break;
    default:break;
    }
    if(!text||!text_size||!icon||!icon_size)return false;
    const int n=std::snprintf(text,text_size,"%s",label);
    const int m=std::snprintf(icon,icon_size,"%d",mapped);
    return n>=0 && static_cast<size_t>(n)<text_size && m>=0 && static_cast<size_t>(m)<icon_size;
}

bool parse_open_meteo_weather(const char *json,WeatherData *weather,WeatherForecastData *forecast)
{
    if(!weather||!forecast)return false;
    NetworkJsonRoot root(json);
    if(!root || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"error")))return false;
    const cJSON *current=cJSON_GetObjectItemCaseSensitive(root.get(),"current");
    const cJSON *daily=cJSON_GetObjectItemCaseSensitive(root.get(),"daily");
    const cJSON *current_time=cJSON_GetObjectItemCaseSensitive(current,"time");
    double offset;
    if(!number(root.get(),"utc_offset_seconds",28800,28800,&offset) ||
       !cJSON_IsString(current_time) || std::strlen(current_time->valuestring)<16)return false;
    double temp,humidity,code,daylight,wind,direction;
    if(!number(current,"temperature_2m",-100,70,&temp) ||
       !number(current,"relative_humidity_2m",0,100,&humidity) ||
       !number(current,"weather_code",0,999,&code) || std::floor(code)!=code ||
       !number(current,"is_day",0,1,&daylight) || std::floor(daylight)!=daylight ||
       !number(current,"wind_speed_10m",0,150,&wind) ||
       !number(current,"wind_direction_10m",0,360,&direction))return false;
    const char *keys[]={"time","temperature_2m_max","temperature_2m_min","weather_code","sunrise","sunset"};
    const cJSON *arrays[6]={};
    int count=0;
    for(int j=0;j<6;++j){
        arrays[j]=cJSON_GetObjectItemCaseSensitive(daily,keys[j]);
        if(!cJSON_IsArray(arrays[j]))return false;
        const int n=cJSON_GetArraySize(arrays[j]);
        if(j==0)count=n;
        if(n!=count || n<1 || n>16)return false;
    }
    *forecast={};
    forecast->count=count<kWeatherForecastDays?count:kWeatherForecastDays;
    for(int i=0;i<forecast->count;++i){
        auto &out=forecast->days[i];
        const cJSON *date=cJSON_GetArrayItem(arrays[0],i);
        if(!cJSON_IsString(date)||!date_valid(date->valuestring))return false;
        if(i==0 && std::strncmp(date->valuestring,current_time->valuestring,10)!=0)return false;
        if(i>0 && std::strcmp(forecast->days[i-1].date,date->valuestring)>=0)return false;
        double high,low,c;
        if(!array_number(arrays[1],i,-100,70,&high)||!array_number(arrays[2],i,-100,70,&low)||high<low||
           !array_number(arrays[3],i,0,999,&c)||std::floor(c)!=c)return false;
        std::snprintf(out.date,sizeof(out.date),"%s",date->valuestring);
        std::snprintf(out.temp_max,sizeof(out.temp_max),"%.0f",high);
        std::snprintf(out.temp_min,sizeof(out.temp_min),"%.0f",low);
        if(!sun_time(arrays[4],i,out.date,out.sunrise,sizeof(out.sunrise),&out.sunrise_day_offset)||
           !sun_time(arrays[5],i,out.date,out.sunset,sizeof(out.sunset),&out.sunset_day_offset)||
           !open_meteo_weather_code(static_cast<int>(c),true,out.text,sizeof(out.text),out.icon,sizeof(out.icon)))return false;
        out.valid=true;
    }
    std::snprintf(weather->temp,sizeof(weather->temp),"%.0f",temp);
    std::snprintf(weather->humidity,sizeof(weather->humidity),"%.0f",humidity);
    if(!open_meteo_weather_code(static_cast<int>(code),daylight!=0,weather->text,sizeof(weather->text),weather->icon,sizeof(weather->icon)))return false;
    auto &today=forecast->days[0];
    std::snprintf(today.humidity,sizeof(today.humidity),"%.0f",humidity);
    const char *dirs[]={"北风","东北风","东风","东南风","南风","西南风","西风","西北风"};
    std::snprintf(today.wind_dir,sizeof(today.wind_dir),"%s",dirs[static_cast<int>((direction+22.5)/45.0)%8]);
    const double thresholds[]={0.3,1.6,3.4,5.5,8.0,10.8,13.9,17.2,20.8,24.5,28.5,32.7};
    int scale=0;while(scale<12 && wind>=thresholds[scale])++scale;
    std::snprintf(today.wind_scale,sizeof(today.wind_scale),"%d",scale);
    forecast->ready=true;
    return true;
}

bool parse_open_meteo_air(const char *json,WeatherAirData *air)
{
    if(!air)return false;
    NetworkJsonRoot root(json);
    if(!root || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"error")))return false;
    const cJSON *current=cJSON_GetObjectItemCaseSensitive(root.get(),"current");
    double aqi,pm;
    if(!number(current,"us_aqi",0,1000,&aqi)||!number(current,"pm2_5",0,5000,&pm))return false;
    *air={};
    air->us_aqi=true;
    std::snprintf(air->aqi,sizeof(air->aqi),"%.0f",aqi);
    std::snprintf(air->pm2p5,sizeof(air->pm2p5),"%.1f",pm);
    const char *category=aqi<=50?"优":aqi<=100?"中等":aqi<=150?"敏感":aqi<=200?"不健康":aqi<=300?"很差":"危险";
    std::snprintf(air->category,sizeof(air->category),"%s",category);
    air->ready=true;
    return true;
}
