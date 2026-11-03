// 定义天气看板固件与 SDL 预览共用的主体布局。
#pragma once

namespace ui_weather_board_layout {

inline constexpr int kForecastCardX[] = {138, 180, 222, 264, 306, 348};
inline constexpr int kForecastCardY = 66;
inline constexpr int kForecastCardW = 34;
inline constexpr int kForecastCardH = 126;
inline constexpr int kForecastCardDateH = 30;
inline constexpr int kForecastCardIconY = 35;
inline constexpr int kForecastCardIconH = 36;
inline constexpr int kForecastCardTextY = 72;
inline constexpr int kForecastCardTextH = 34;
inline constexpr int kForecastCardRangeY = 108;
inline constexpr int kForecastCardRangeH = 16;

inline constexpr int kWeatherBoardCurrentCityX = 20;
inline constexpr int kWeatherBoardCurrentCityY = 66;
inline constexpr int kWeatherBoardCurrentCityW = 135;
inline constexpr int kWeatherBoardCurrentCityH = 28;
inline constexpr int kWeatherBoardCurrentTempX = 20;
inline constexpr int kWeatherBoardCurrentTempY = 86;
inline constexpr int kWeatherBoardCurrentTempW = 88;
inline constexpr int kWeatherBoardCurrentTempH = 54;
inline constexpr int kWeatherBoardCurrentUnitX = 88;
inline constexpr int kWeatherBoardCurrentUnitY = 96;
inline constexpr int kWeatherBoardCurrentUnitW = 24;
inline constexpr int kWeatherBoardCurrentUnitH = 32;
inline constexpr int kWeatherBoardCurrentIconX = 20;
inline constexpr int kWeatherBoardCurrentIconY = 139;
inline constexpr int kWeatherBoardCurrentIconW = 42;
inline constexpr int kWeatherBoardCurrentIconH = 40;
inline constexpr int kWeatherBoardCurrentTextX = 62;
inline constexpr int kWeatherBoardCurrentTextY = 151;
inline constexpr int kWeatherBoardCurrentTextW = 92;
inline constexpr int kWeatherBoardCurrentTextH = 24;
inline constexpr int kWeatherBoardTodayRangeX = 20;
inline constexpr int kWeatherBoardTodayRangeY = 179;
inline constexpr int kWeatherBoardTodayRangeW = 134;
inline constexpr int kWeatherBoardTodayRangeH = 22;

inline constexpr int kWeatherBoardDetailLineX = 20;
inline constexpr int kWeatherBoardDetailLineY = 196;
inline constexpr int kWeatherBoardDetailLineW = 360;
inline constexpr int kWeatherBoardDetailLineH = 2;
inline constexpr int kWeatherBoardDetailTopY = 202;
inline constexpr int kWeatherBoardDetailBottomY = 224;
inline constexpr int kWeatherBoardDetailLabelH = 22;
inline constexpr int kWeatherBoardSunLabelH = 20;
inline constexpr int kWeatherBoardLeftColumnX = 20;
// 中列右移：右列文字起点在 279 附近，中列取 148 使下排
// “日出 / 日落 / 距日落”左右间隙接近均等（48 / 49 px）。
inline constexpr int kWeatherBoardMiddleColumnX = 148;
// 右列整体右移并改为列内左对齐，使“风力”首字“东”与“距日落”首字“距”起笔对齐。
// 盒内左内边距 2px，故盒子 X = 279 - 2 = 277；宽度放到屏幕右缘 400，
// 以容纳“无持续风向 3级”这类长风向文案而不被 LV_LABEL_LONG_CLIP 截断。
inline constexpr int kWeatherBoardRightColumnX = 277;
inline constexpr int kWeatherBoardAirLabelW = 110;
inline constexpr int kWeatherBoardHumidityLabelW = 86;
inline constexpr int kWeatherBoardWindLabelW = 123;
inline constexpr int kWeatherBoardSunriseLabelW = 110;
inline constexpr int kWeatherBoardSunsetLabelW = 98;
inline constexpr int kWeatherBoardSunCountdownLabelW = 123;

inline constexpr int kWeatherBoardAlertX = 20;
inline constexpr int kWeatherBoardAlertY = 246;
inline constexpr int kWeatherBoardAlertW = 360;
inline constexpr int kWeatherBoardAlertH = 22;
inline constexpr int kWeatherBoardAdviceX = 20;
inline constexpr int kWeatherBoardAdviceY = 272;
inline constexpr int kWeatherBoardAdviceW = 360;
inline constexpr int kWeatherBoardAdviceH = 20;

static_assert(kForecastCardW > 0 && kForecastCardH > 0,
              "weather forecast card size must be positive");
static_assert(kForecastCardRangeY + kForecastCardRangeH <= kForecastCardH,
              "weather forecast card content must fit card height");
static_assert(kWeatherBoardCurrentCityW > 0 &&
                  kWeatherBoardCurrentCityH > 0 &&
                  kWeatherBoardCurrentTempW > 0 &&
                  kWeatherBoardCurrentTempH > 0 &&
                  kWeatherBoardCurrentIconW > 0 &&
                  kWeatherBoardCurrentIconH > 0,
              "weather board current conditions must use positive sizes");
static_assert(kWeatherBoardDetailLineW > 0 &&
                  kWeatherBoardDetailLineH > 0 &&
                  kWeatherBoardAlertW > 0 &&
                  kWeatherBoardAlertH > 0 &&
                  kWeatherBoardAdviceW > 0 &&
                  kWeatherBoardAdviceH > 0,
              "weather board details must use positive sizes");

} // namespace ui_weather_board_layout
