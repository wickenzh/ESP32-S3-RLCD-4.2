// 复用天气、传感器和农历快照刷新聚合时钟，隐藏时不更新页面对象。
#include "ui_aggregate_clock.h"
#include "ui_aggregate_clock_view.h"
#include "ui_aggregate_weather_policy.h"
#include "ui_page_state.h"
#include "ui_battery.h"
#include "ui_work_status.h"
#include "ui_progress.h"
#include "ui_widgets.h"
#include "ui_work_page_layout.h"
#include "ui_canvas_primitives.h"
#include "work_page_ids.h"
#include "active_work_page_state.h"
#include "weather_state.h"
#include "qweather_icons.h"
#include "local_sensor_state.h"
#include "calendar_lunar.h"
#include "sensor_time.h"
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
LV_FONT_DECLARE(zh_font_16);
LV_FONT_DECLARE(zh_flip_lunar_22);

namespace {
AggregateClockView s_view;
lv_color_t *s_buffers[3] = {};
uint32_t s_weather_version=0, s_sensor_version=0;
bool s_weather_valid=false, s_sensor_valid=false;
int s_date_key=-1;
int64_t s_buffer_retry_us=0;
}

void clear_aggregate_clock_refs() {
    s_view={}; s_weather_valid=false; s_sensor_valid=false; s_date_key=-1;
}

void build_aggregate_clock_page() {
    if(work_page_root(kWorkPageAggregateClock)) return;
    lv_obj_t *root=create_page_root();
    if(!root) return;
    set_work_page_root(kWorkPageAggregateClock,root);
    lv_obj_add_flag(root,LV_OBJ_FLAG_HIDDEN);
    clear_aggregate_clock_refs();
    build_work_page_battery_icon(root,kWorkPageAggregateClock);
    build_work_page_status_bar(root,kWorkPageAggregateClock,false,false);
    make_black_bar(root,18,54,364,4);
    build_work_page_day_progress(root,kWorkPageAggregateClock);
    for(auto &buffer:s_buffers) ensure_canvas_buffer(&buffer,kAggregateDigitWidth,kAggregateDigitHeight);
    s_buffer_retry_us=esp_timer_get_time()+5000000;
    aggregate_clock_view_build(root,s_view,s_buffers);
}

bool update_aggregate_clock_page(const struct tm &local) {
    if(active_work_page_load()!=kWorkPageAggregateClock || !work_page_root(kWorkPageAggregateClock)) return false;
    bool changed=false;
    if (esp_timer_get_time() >= s_buffer_retry_us) {
        s_buffer_retry_us=esp_timer_get_time()+5000000;
        for (int i=0;i<3;++i) if (!s_buffers[i] &&
            ensure_canvas_buffer(&s_buffers[i],kAggregateDigitWidth,kAggregateDigitHeight)) {
            lv_canvas_set_buffer(s_view.digits[i],s_buffers[i],kAggregateDigitWidth,
                                 kAggregateDigitHeight,LV_IMG_CF_TRUE_COLOR);
            s_view.values[i]=-2;
        }
    }
    const bool valid_time=is_tm_plausible(local);
    changed |= aggregate_clock_view_time(s_view,valid_time?local.tm_hour:-1,
                                         valid_time?local.tm_min:-1,valid_time?local.tm_sec:-1);
    const int date_key=valid_time?(local.tm_year*400+local.tm_yday):-1;
    if(!valid_time && s_date_key!=-1) {
        changed |= aggregate_clock_set_text(s_view.day,"--");
        changed |= aggregate_clock_set_text(s_view.month,"--月");
        changed |= aggregate_clock_set_text(s_view.lunar,"--");
        s_date_key=-1; s_weather_valid=false;
    }
    if(valid_time && date_key!=s_date_key) {
        s_weather_valid=false;
        char day[8],month[16];
        std::snprintf(day,sizeof(day),"%d",local.tm_mday);
        CalendarDayInfo lunar={};
        const bool lunar_ok=calendar_day_info(local,&lunar);
        std::snprintf(month,sizeof(month),"%s%s",lunar_ok && lunar.lunar_leap?"闰":"",
                      lunar_ok?calendar_lunar_month_text(lunar):"--");
        lv_obj_set_style_text_font(s_view.month,lunar_ok && lunar.lunar_leap?
                                  &zh_font_16:&zh_flip_lunar_22,0);
        changed |= aggregate_clock_set_text(s_view.day,day);
        changed |= aggregate_clock_set_text(s_view.month,month);
        changed |= aggregate_clock_set_text(s_view.lunar,lunar_ok?calendar_lunar_day_text(lunar):"--");
        s_date_key=date_key;
    }
    const uint32_t sensor_version=local_sensor_state_version();
    if(!s_sensor_valid || sensor_version!=s_sensor_version) {
        LocalSensorStateSnapshot sensor;
        if(local_sensor_state_snapshot_load(&sensor)) {
            char temp[24]="--.- C",humi[24]="--%";
            if(sensor.available) {
                std::snprintf(temp,sizeof(temp),"%.1f C",sensor.temperature);
                std::snprintf(humi,sizeof(humi),"%.0f%%",sensor.humidity);
            }
            changed |= aggregate_clock_set_text(s_view.local_temp,temp);
            changed |= aggregate_clock_set_text(s_view.humidity,humi);
            s_sensor_version=sensor.version; s_sensor_valid=true;
        }
    }
    const uint32_t weather_version=weather_state_version_load();
    if(!s_weather_valid || weather_version!=s_weather_version) {
        WeatherData weather={}; WeatherForecastData forecast={};
        if(get_weather_full_snapshot(&weather,nullptr,&forecast,nullptr)) {
            char temp[24]="-- C",range[64]="最高 -- C  最低 -- C";
            if(weather.temp[0]) std::snprintf(temp,sizeof(temp),"%s C",weather.temp);
            lv_obj_set_style_text_font(s_view.temperature,
                                      std::strlen(temp)>5?&lv_font_montserrat_24:&lv_font_montserrat_48,0);
            char today[12]={};
            if(valid_time) strftime(today,sizeof(today),"%Y-%m-%d",&local);
            for(int i=0;i<forecast.count && i<kWeatherForecastDays;++i) {
                const auto &day=forecast.days[i];
                if(day.valid && std::strcmp(day.date,today)==0) {
                    std::snprintf(range,sizeof(range),"最高 %s C  最低 %s C",day.temp_max,day.temp_min); break;
                }
            }
            changed |= aggregate_clock_set_text(s_view.city,weather.city[0]?weather.city:"等待数据");
            changed |= aggregate_clock_set_text(s_view.weather,weather.text[0]?weather.text:"--");
            changed |= aggregate_clock_set_text(s_view.temperature,temp);
            changed |= aggregate_clock_set_text(s_view.icon,weather_icon_text(weather.icon).c_str());
            changed |= aggregate_clock_set_text(s_view.range,range);
            changed |= aggregate_clock_weather_theme(s_view,aggregate_weather_kind(weather.icon));
            s_weather_version=weather_version; s_weather_valid=true;
        }
    }
    return changed;
}
