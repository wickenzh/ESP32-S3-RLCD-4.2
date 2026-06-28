// 声明日历页使用的农历和节日计算接口。
#pragma once

#include <stddef.h>
#include <time.h>

struct CalendarDayInfo {
    int year = 0;
    int month = 0;
    int day = 0;
    int lunar_year = 0;
    int lunar_month = 0;
    int lunar_day = 0;
    bool lunar_leap = false;
    char subtext[16] = {};
};

bool calendar_day_info(const struct tm &local, CalendarDayInfo *info);
int calendar_days_in_month(int year, int month);
int calendar_first_weekday(int year, int month);
