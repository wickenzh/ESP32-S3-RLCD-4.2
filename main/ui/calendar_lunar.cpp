// 计算公历、农历、节日和节气等日历页面数据。
#include "calendar_lunar.h"

#include "app_state.h"

struct LunarYearInfo {
    int year;
    uint32_t data;
};

static const LunarYearInfo kLunarYears[] = {
    {2023, 0x0d2b2},
    {2024, 0x0a950},
    {2025, 0x0b557},
    {2026, 0x056a0},
    {2027, 0x0a5b0},
    {2028, 0x152b5},
    {2029, 0x052b0},
    {2030, 0x0a930},
    {2031, 0x07954},
    {2032, 0x06aa0},
    {2033, 0x0ad50},
    {2034, 0x05b52},
    {2035, 0x04b60},
};

static const char *const kLunarDayNames[] = {
    "",
    "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
    "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
    "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
};

static const char *const kLunarMonthNames[] = {
    "",
    "正月", "二月", "三月", "四月", "五月", "六月",
    "七月", "八月", "九月", "十月", "冬月", "腊月",
};

static const char *const kSolarTermNames[] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
    "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分",
    "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

static constexpr int kTmYearOffset = 1900;
static constexpr int kTmMonthOffset = 1;
static constexpr int kSolarTermBaseYear = 1900;
static constexpr double kMsPerMinute = 60000.0;

static const int kSolarTermMinutes[] = {
    0, 21208, 42467, 63836, 85337, 107014,
    128867, 150921, 173149, 195551, 218072, 240693,
    263343, 285989, 308563, 331033, 353350, 375494,
    397447, 419210, 440795, 462224, 483532, 504758,
};

static int days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int z, int *year, unsigned *month, unsigned *day)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    *year = y;
    *month = m;
    *day = d;
}

static const LunarYearInfo *find_lunar_year(int year)
{
    for (const auto &item : kLunarYears) {
        if (item.year == year) {
            return &item;
        }
    }
    return nullptr;
}

static int leap_month(uint32_t data)
{
    return (int)(data & 0x0f);
}

static int leap_month_days(uint32_t data)
{
    if (leap_month(data) == 0) {
        return 0;
    }
    return (data & 0x10000) ? 30 : 29;
}

static int lunar_month_days(uint32_t data, int month)
{
    return (data & (0x10000 >> month)) ? 30 : 29;
}

static int lunar_year_days(uint32_t data)
{
    int days = 348;
    for (int mask = 0x8000; mask > 0x8; mask >>= 1) {
        if (data & mask) {
            ++days;
        }
    }
    return days + leap_month_days(data);
}

int calendar_days_in_month(int year, int month)
{
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return month_days[month - 1];
}

int calendar_first_weekday(int year, int month)
{
    int days = days_from_civil(year, (unsigned)month, 1);
    int weekday = (days + 4) % 7;
    return weekday < 0 ? weekday + 7 : weekday;
}

static bool lunar_from_date(int year, int month, int day, CalendarDayInfo *info)
{
    int offset = days_from_civil(year, (unsigned)month, (unsigned)day) -
                 days_from_civil(2023, 1, 22);
    if (offset < 0) {
        return false;
    }

    int lunar_year = 2023;
    const LunarYearInfo *year_info = find_lunar_year(lunar_year);
    while (year_info) {
        int days = lunar_year_days(year_info->data);
        if (offset < days) {
            break;
        }
        offset -= days;
        ++lunar_year;
        year_info = find_lunar_year(lunar_year);
    }
    if (!year_info) {
        return false;
    }

    int lunar_month = 1;
    bool is_leap = false;
    int leap = leap_month(year_info->data);
    for (;;) {
        int days = is_leap ? leap_month_days(year_info->data) : lunar_month_days(year_info->data, lunar_month);
        if (offset < days) {
            break;
        }
        offset -= days;
        if (leap == lunar_month && !is_leap) {
            is_leap = true;
        } else {
            is_leap = false;
            ++lunar_month;
        }
        if (lunar_month > 12) {
            return false;
        }
    }

    info->lunar_year = lunar_year;
    info->lunar_month = lunar_month;
    info->lunar_day = offset + 1;
    info->lunar_leap = is_leap;
    return true;
}

static const char *gregorian_festival(int month, int day)
{
    if (month == 1 && day == 1) return "元旦";
    if (month == 2 && day == 14) return "情人节";
    if (month == 3 && day == 8) return "妇女节";
    if (month == 5 && day == 1) return "劳动节";
    if (month == 6 && day == 1) return "儿童节";
    if (month == 9 && day == 10) return "教师节";
    if (month == 10 && day == 1) return "国庆";
    if (month == 12 && day == 25) return "圣诞";
    return nullptr;
}

static const char *lunar_festival(int lunar_month, int lunar_day)
{
    if (lunar_month == 1 && lunar_day == 1) return "春节";
    if (lunar_month == 1 && lunar_day == 15) return "元宵";
    if (lunar_month == 5 && lunar_day == 5) return "端午";
    if (lunar_month == 7 && lunar_day == 7) return "七夕";
    if (lunar_month == 8 && lunar_day == 15) return "中秋";
    if (lunar_month == 9 && lunar_day == 9) return "重阳";
    if (lunar_month == 12 && lunar_day == 8) return "腊八";
    return nullptr;
}

static const char *solar_term(int year, int month, int day)
{
    if (year < kMinValidYear || year > kMaxValidYear || month < 1 || month > 12) {
        return nullptr;
    }
    constexpr double kYearMs = 31556925974.7;
    constexpr double kBaseMs = -2208491700000.0; // 1900-01-06 02:05 UTC
    constexpr double kDayMs = 86400000.0;
    int first = (month - 1) * 2;
    for (int i = 0; i < 2; ++i) {
        int term = first + i;
        double ms = kBaseMs + kYearMs * (year - kSolarTermBaseYear) + (double)kSolarTermMinutes[term] * kMsPerMinute;
        int days = (int)(ms / kDayMs);
        int ty = 0;
        unsigned tm = 0;
        unsigned td = 0;
        civil_from_days(days, &ty, &tm, &td);
        if (ty == year && (int)tm == month && (int)td == day) {
            return kSolarTermNames[term];
        }
    }
    return nullptr;
}

bool calendar_day_info(const struct tm &local, CalendarDayInfo *info)
{
    if (!info) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->year = local.tm_year + kTmYearOffset;
    info->month = local.tm_mon + kTmMonthOffset;
    info->day = local.tm_mday;
    if (info->year < kMinValidYear || info->year > kMaxValidYear) {
        strlcpy(info->subtext, "--", sizeof(info->subtext));
        return false;
    }

    bool lunar_ok = lunar_from_date(info->year, info->month, info->day, info);
    const char *text = gregorian_festival(info->month, info->day);
    if (!text) {
        text = solar_term(info->year, info->month, info->day);
    }
    if (!text && lunar_ok && !info->lunar_leap) {
        text = lunar_festival(info->lunar_month, info->lunar_day);
    }
    if (!text && lunar_ok) {
        if (info->lunar_day == 1 && info->lunar_month >= 1 && info->lunar_month <= 12) {
            snprintf(info->subtext, sizeof(info->subtext), "%s%s",
                     info->lunar_leap ? "闰" : "",
                     kLunarMonthNames[info->lunar_month]);
            return true;
        }
        if (info->lunar_day >= 1 && info->lunar_day <= 30) {
            text = kLunarDayNames[info->lunar_day];
        }
    }
    strlcpy(info->subtext, text ? text : "--", sizeof(info->subtext));
    return lunar_ok;
}
