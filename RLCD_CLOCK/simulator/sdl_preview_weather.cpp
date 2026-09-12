// 实现 SDL 天气看板主体和 QWeather 图标码到字体字符的转换。
#include "sdl_preview_sample_data.h"
#include "sdl_preview_weather.h"

#include <stdlib.h>

#include "core/app_constexpr.h"
#include "sdl_preview_widgets.h"
#include "ui_weather_board_layout.h"

LV_FONT_DECLARE(qweather_icons_36);
LV_FONT_DECLARE(zh_font_16);

namespace {
using namespace ui_weather_board_layout;

using sdl_preview_widgets::make_black_bar;
using sdl_preview_widgets::make_label;
using sdl_preview_widgets::make_label_with_font;

uint32_t weather_icon_codepoint(const char *code)
{
    int icon = atoi(code);
    if (icon >= 100 && icon <= 104) return 0xF101 + static_cast<uint32_t>(icon - 100);
    if (icon >= 150 && icon <= 153) return 0xF106 + static_cast<uint32_t>(icon - 150);
    if (icon >= 300 && icon <= 318) return 0xF10A + static_cast<uint32_t>(icon - 300);
    if (icon >= 350 && icon <= 351) return 0xF11D + static_cast<uint32_t>(icon - 350);
    if (icon == 399) return 0xF11F;
    if (icon >= 400 && icon <= 410) return 0xF120 + static_cast<uint32_t>(icon - 400);
    if (icon >= 456 && icon <= 457) return 0xF12B + static_cast<uint32_t>(icon - 456);
    if (icon == 499) return 0xF12D;
    if (icon >= 500 && icon <= 504) return 0xF12E + static_cast<uint32_t>(icon - 500);
    if (icon >= 507 && icon <= 515) return 0xF133 + static_cast<uint32_t>(icon - 507);
    if (icon >= 800 && icon <= 807) return 0xF13C + static_cast<uint32_t>(icon - 800);
    if (icon == 900) return 0xF144;
    if (icon == 901) return 0xF145;
    if (icon == 9999) return 0xF1CB;
    return 0xF146;
}

void style_weather_preview_card(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
}
} // namespace

const char *preview_weather_icon_text(const char *code)
{
    static char text[5];
    uint32_t cp = weather_icon_codepoint(code);
    text[0] = static_cast<char>(0xE0 | (cp >> 12));
    text[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    text[2] = static_cast<char>(0x80 | (cp & 0x3F));
    text[3] = '\0';
    return text;
}

void build_weather_board_preview_body(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }

    make_label(screen,
               kWeatherBoardCurrentCityX,
               kWeatherBoardCurrentCityY,
               kWeatherBoardCurrentCityW,
               kWeatherBoardCurrentCityH,
               kPreviewCityLabel);
    make_label_with_font(screen,
                         kWeatherBoardCurrentTempX,
                         kWeatherBoardCurrentTempY,
                         kWeatherBoardCurrentTempW,
                         kWeatherBoardCurrentTempH,
                         "26",
                         &lv_font_montserrat_48);
    make_label_with_font(screen,
                         kWeatherBoardCurrentUnitX,
                         kWeatherBoardCurrentUnitY,
                         kWeatherBoardCurrentUnitW,
                         kWeatherBoardCurrentUnitH,
                         "C",
                         &lv_font_montserrat_24);
    lv_obj_t *icon = make_label(screen,
                                kWeatherBoardCurrentIconX,
                                kWeatherBoardCurrentIconY,
                                kWeatherBoardCurrentIconW,
                                kWeatherBoardCurrentIconH,
                                preview_weather_icon_text("100"));
    lv_obj_set_style_text_font(icon, &qweather_icons_36, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    make_label(screen,
               kWeatherBoardCurrentTextX,
               kWeatherBoardCurrentTextY,
               kWeatherBoardCurrentTextW,
               kWeatherBoardCurrentTextH,
               "晴");
    make_label(screen,
               kWeatherBoardTodayRangeX,
               kWeatherBoardTodayRangeY,
               kWeatherBoardTodayRangeW,
               kWeatherBoardTodayRangeH,
               "今日 22/29C");

    static constexpr const char *kDays[] = {
        "周二\n23日", "周三\n24日", "周四\n25日", "周五\n26日", "周六\n27日", "周日\n28日",
    };
    static constexpr const char *kTexts[] = {"晴", "多云转晴", "小到中雨", "阴", "雨夹雪", "大到暴雨"};
    static constexpr const char *kIcons[] = {"100", "101", "305", "104", "404", "306"};
    static constexpr const char *kRanges[] = {"22/29", "23/30", "-3/2", "22/28", "-8/-2", "18/24"};
    static_assert(array_count(kDays) == array_count(kTexts) &&
                      array_count(kDays) == array_count(kIcons) &&
                      array_count(kDays) == array_count(kRanges) &&
                      array_count(kDays) == array_count(kForecastCardX),
                  "weather preview forecast columns must stay aligned");
    for (size_t i = 0; i < array_count(kDays); ++i) {
        int x = kForecastCardX[i];
        lv_obj_t *card = lv_obj_create(screen);
        lv_obj_set_pos(card, x, kForecastCardY);
        lv_obj_set_size(card, kForecastCardW, kForecastCardH);
        style_weather_preview_card(card);
        lv_obj_t *date = make_label_with_font(screen,
                                              x,
                                              kForecastCardY,
                                              kForecastCardW,
                                              kForecastCardDateH,
                                              kDays[i],
                                              &zh_font_16);
        lv_label_set_long_mode(date, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *small_icon = make_label(screen,
                                          x,
                                          kForecastCardY + kForecastCardIconY,
                                          kForecastCardW,
                                          kForecastCardIconH,
                                          preview_weather_icon_text(kIcons[i]));
        lv_obj_set_style_text_font(small_icon, &qweather_icons_36, LV_PART_MAIN);
        lv_obj_set_style_text_align(small_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *text = make_label_with_font(screen,
                                              x,
                                              kForecastCardY + kForecastCardTextY,
                                              kForecastCardW,
                                              kForecastCardTextH,
                                              kTexts[i],
                                              &zh_font_16);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_t *range = make_label_with_font(screen,
                                               x,
                                               kForecastCardY + kForecastCardRangeY,
                                               kForecastCardW,
                                               kForecastCardRangeH,
                                               kRanges[i],
                                               &lv_font_montserrat_12);
        lv_obj_set_style_text_align(range, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    make_black_bar(screen,
                   kWeatherBoardDetailLineX,
                   kWeatherBoardDetailLineY,
                   kWeatherBoardDetailLineW,
                   kWeatherBoardDetailLineH);
    make_label(screen,
               kWeatherBoardLeftColumnX,
               kWeatherBoardDetailTopY,
               kWeatherBoardAirLabelW,
               kWeatherBoardDetailLabelH,
               "AQI 42 优");
    make_label(screen,
               kWeatherBoardMiddleColumnX,
               kWeatherBoardDetailTopY,
               kWeatherBoardHumidityLabelW,
               kWeatherBoardDetailLabelH,
               "湿度 58%");
    make_label(screen,
               kWeatherBoardRightColumnX,
               kWeatherBoardDetailTopY,
               kWeatherBoardWindLabelW,
               kWeatherBoardDetailLabelH,
               "东北风 3级");
    make_label(screen,
               kWeatherBoardLeftColumnX,
               kWeatherBoardDetailBottomY,
               kWeatherBoardSunriseLabelW,
               kWeatherBoardSunLabelH,
               "日出 05:01");
    make_label(screen,
               kWeatherBoardMiddleColumnX,
               kWeatherBoardDetailBottomY,
               kWeatherBoardSunsetLabelW,
               kWeatherBoardSunLabelH,
               "日落 19:06");
    make_label(screen,
               kWeatherBoardRightColumnX,
               kWeatherBoardDetailBottomY,
               kWeatherBoardSunCountdownLabelW,
               kWeatherBoardSunLabelH,
               "距日落 01:05");
    make_label(screen,
               kWeatherBoardAlertX,
               kWeatherBoardAlertY,
               kWeatherBoardAlertW,
               kWeatherBoardAlertH,
               "预警 大风蓝 / 暴雨黄 / 雷电橙");
    lv_obj_t *advice = make_label(screen,
                                  kWeatherBoardAdviceX,
                                  kWeatherBoardAdviceY,
                                  kWeatherBoardAdviceW,
                                  kWeatherBoardAdviceH,
                                  "天气平稳，适合轻装出行。");
    lv_label_set_long_mode(advice, LV_LABEL_LONG_WRAP);
}
