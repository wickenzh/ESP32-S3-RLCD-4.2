// 构建小智对话、准备状态和番茄钟 SDL 静态预览主体。
#include "sdl_preview_xiaozhi.h"

#include <cstring>
#include <vector>

#include "sdl_preview_flip_cards.h"
#include "sdl_preview_widgets.h"

LV_FONT_DECLARE(zh_font_16);
LV_FONT_DECLARE(zh_pomodoro_title_24);

namespace {

constexpr int kFaceSize = 76;
constexpr int kPreparingDotsWidth = 48;
constexpr int kPreparingDotsHeight = 16;
std::vector<lv_color_t> g_xiaozhi_face_pixels(kFaceSize * kFaceSize);
std::vector<lv_color_t> g_xiaozhi_preparing_pixels(kPreparingDotsWidth * kPreparingDotsHeight);

bool preview_mode_is(const char *mode, const char *expected)
{
    return mode && expected && std::strcmp(mode, expected) == 0;
}

const char *latest_xiaozhi_preview_subtitle(const char *text)
{
    static char visible[192] = {};
    const char *window_start = text ? text : "";
    lv_point_t text_size = {};
    lv_txt_get_size(&text_size,
                    window_start,
                    &zh_font_16,
                    0,
                    0,
                    248,
                    LV_TEXT_FLAG_NONE);
    while (text_size.y > 58 && window_start[0] != '\0') {
        uint32_t first_line_bytes = _lv_txt_get_next_line(window_start,
                                                         &zh_font_16,
                                                         0,
                                                         248,
                                                         nullptr,
                                                         LV_TEXT_FLAG_NONE);
        if (first_line_bytes == 0 || window_start[first_line_bytes] == '\0') {
            break;
        }
        window_start += first_line_bytes;
        lv_txt_get_size(&text_size,
                        window_start,
                        &zh_font_16,
                        0,
                        0,
                        248,
                        LV_TEXT_FLAG_NONE);
    }
    strlcpy(visible, window_start, sizeof(visible));
    return visible;
}

} // namespace

XiaozhiPreviewMode classify_xiaozhi_preview_mode(const char *preview_mode)
{
    XiaozhiPreviewMode mode = {};
    mode.pomodoro_running = preview_mode_is(preview_mode, "xiaozhi_pomodoro");
    mode.pomodoro_final = preview_mode_is(preview_mode, "xiaozhi_pomodoro_final");
    mode.pomodoro_completed = preview_mode_is(preview_mode, "xiaozhi_pomodoro_done");
    mode.preparing = preview_mode_is(preview_mode, "xiaozhi_preparing");
    return mode;
}

void build_xiaozhi_preview_body(lv_obj_t *screen,
                                const struct tm *local,
                                const XiaozhiPreviewMode &mode)
{
    if (!screen || !local) {
        return;
    }
    bool pomodoro_visible = mode.pomodoro_visible();
    static const int card_x[3] = {18, 144, 270};
    int values[3] = {
        local->tm_hour,
        mode.pomodoro_running ? 24 : (mode.pomodoro_final || mode.pomodoro_completed ? 0 : local->tm_min),
        mode.pomodoro_running ? 59 : (mode.pomodoro_final ? 37 : (mode.pomodoro_completed ? 0 : local->tm_sec)),
    };
    for (int index = 0; index < 3; ++index) {
        lv_obj_t *card = sdl_preview_flip_cards::create_preview_flip_card(
            screen, index, card_x[index], 66);
        if (index == 0 && pomodoro_visible) {
            lv_canvas_fill_bg(card, lv_color_black(), LV_OPA_COVER);
            sdl_preview_flip_cards::apply_preview_card_rounding(card);
            const char *state_text = mode.pomodoro_completed ? "已完成" : "专注中";
            const char *mode_text = mode.pomodoro_completed ? "" : "分 / 秒";
            lv_obj_t *title = sdl_preview_widgets::make_label_with_font(
                card, 0, 8, sdl_preview_flip_cards::kFlipCardW, 30, "番茄钟", &zh_pomodoro_title_24);
            lv_obj_t *title_bold = sdl_preview_widgets::make_label_with_font(
                card, 1, 8, sdl_preview_flip_cards::kFlipCardW, 30, "番茄钟", &zh_pomodoro_title_24);
            lv_obj_t *state = sdl_preview_widgets::make_label_with_font(
                card, 0, 44, sdl_preview_flip_cards::kFlipCardW, 24, state_text, &zh_font_16);
            lv_obj_t *mode_label = sdl_preview_widgets::make_label_with_font(
                card, 0, 76, sdl_preview_flip_cards::kFlipCardW, 24, mode_text, &zh_font_16);
            for (lv_obj_t *label : {title, title_bold, state, mode_label}) {
                if (label) {
                    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
                    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
                    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                }
            }
        } else {
            sdl_preview_flip_cards::draw_preview_flip_card(card, values[index]);
        }
    }

    lv_obj_t *panel = sdl_preview_widgets::make_black_bar(screen, 18, 188, 364, 102);
    lv_obj_set_style_radius(panel, 18, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(panel, true, LV_PART_MAIN);

    lv_obj_t *face = lv_canvas_create(screen);
    lv_obj_clear_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(face, 30, 201);
    lv_obj_set_size(face, kFaceSize, kFaceSize);
    lv_obj_set_style_border_width(face, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(face, 0, LV_PART_MAIN);
    lv_canvas_set_buffer(face,
                         g_xiaozhi_face_pixels.data(),
                         kFaceSize,
                         kFaceSize,
                         LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(face, lv_color_black(), LV_OPA_COVER);
    sdl_preview_widgets::canvas_draw_filled_circle(
        face, kFaceSize, kFaceSize, 38, 38, 32, lv_color_white());
    sdl_preview_widgets::canvas_draw_filled_circle(
        face, kFaceSize, kFaceSize, 38, 38, 29, lv_color_black());
    sdl_preview_widgets::canvas_draw_line(face, kFaceSize, kFaceSize, 22, 31, 27, 27, lv_color_white());
    sdl_preview_widgets::canvas_draw_line(face, kFaceSize, kFaceSize, 27, 27, 32, 31, lv_color_white());
    sdl_preview_widgets::canvas_draw_line(face, kFaceSize, kFaceSize, 44, 31, 49, 27, lv_color_white());
    sdl_preview_widgets::canvas_draw_line(face, kFaceSize, kFaceSize, 49, 27, 54, 31, lv_color_white());
    sdl_preview_widgets::canvas_draw_filled_circle(
        face, kFaceSize, kFaceSize, 38, 51, 8, lv_color_white());
    sdl_preview_widgets::canvas_draw_filled_circle(
        face, kFaceSize, kFaceSize, 38, 51, 5, lv_color_black());

    lv_obj_t *state = sdl_preview_widgets::make_label_with_font(screen,
                                                                118,
                                                                196,
                                                                248,
                                                                28,
                                                                mode.preparing
                                                                    ? "小智准备中"
                                                                    : "小智正在说话",
                                                                &zh_font_16);
    if (mode.preparing) {
        lv_obj_t *preparing_dots = lv_canvas_create(screen);
        lv_obj_clear_flag(preparing_dots, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(preparing_dots, 218, 201);
        lv_obj_set_size(preparing_dots, kPreparingDotsWidth, kPreparingDotsHeight);
        lv_obj_set_style_border_width(preparing_dots, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(preparing_dots, 0, LV_PART_MAIN);
        lv_canvas_set_buffer(preparing_dots,
                             g_xiaozhi_preparing_pixels.data(),
                             kPreparingDotsWidth,
                             kPreparingDotsHeight,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(preparing_dots, lv_color_black(), LV_OPA_COVER);
        for (int x : {6, 22, 38}) {
            sdl_preview_widgets::canvas_draw_filled_circle(
                preparing_dots, kPreparingDotsWidth, kPreparingDotsHeight, x, 8, 4, lv_color_white());
        }
    }
    lv_obj_t *detail = sdl_preview_widgets::make_label_with_font(
        screen,
        118,
        224,
        248,
        58,
        mode.preparing
            ? "正在初始化网络和语音服务"
            : latest_xiaozhi_preview_subtitle(
                  "当地今天白天多云，气温会逐渐升高，午后体感偏热。外出时建议带好饮用水并注意防晒，如果傍晚出门散步，最新预报显示风力会减弱，体感会更舒适。"),
        &zh_font_16);
    lv_obj_set_style_text_color(state, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_color(detail, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
}
