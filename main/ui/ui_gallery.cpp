// 构建和刷新图片时钟页面的图库、大号时间、每日文字和本地音乐播放界面。
// Gallery页面双模式：默认显示星期图片，KEY键切入音乐播放模式。
#include "ui_views.h"

#include "app_constexpr.h"
#include "app_time_constants.h"
#include "clock_gallery_images.h"
#include "custom_assets.h"
#include "music_player.h"
#include "network_services.h"
#include "ui_battery.h"
#include "ui_gallery_selection.h"

#define GALLERY_IMAGE_CANVAS_CREATE_FAILED_LOG "gallery image canvas create failed"
#define GALLERY_TIME_CANVAS_CREATE_FAILED_LOG "gallery time canvas create failed"
#define GALLERY_SAYING_LABEL_CREATE_FAILED_LOG "gallery saying label create failed"

static int s_last_gallery_image_index = -1;
static int s_last_gallery_time_key = -1;
static lv_color_t *s_gallery_time_canvas_buffer;
static lv_color_t *s_gallery_image_canvas_buffer;
static uint8_t s_custom_gallery_image[CLOCK_GALLERY_IMAGE_BYTES_PER_ROW * CLOCK_GALLERY_IMAGE_HEIGHT];

// 音乐面板容器（用于整体显示/隐藏）
static lv_obj_t *s_music_panel = nullptr;

// 音乐播放界面布局常量
static constexpr int kGalleryTopLineX = 18;
static constexpr int kGalleryTopLineY = 54;
static constexpr int kGalleryTopLineW = 364;
static constexpr int kGalleryTopLineH = 4;

// 星期图片区域
static constexpr int kGalleryImageCanvasX = 20;
static constexpr int kGalleryImageCanvasY = 62;

// 音乐面板（与星期图片同区域，220×208）
static constexpr int kMusicPanelX = 20;
static constexpr int kMusicPanelY = 66;
static constexpr int kMusicPanelW = 220;
static constexpr int kMusicPanelH = 208;
static constexpr int kMusicPanelRadius = 12;

// 当前歌曲文件名（面板内相对坐标）
static constexpr int kMusicFileNameRelX = 10;
static constexpr int kMusicFileNameRelY = 16;
static constexpr int kMusicFileNameW = 200;
static constexpr int kMusicFileNameH = 24;

// 右侧时间区域保持不变
static constexpr int kGalleryDividerX = 252;
static constexpr int kGalleryDividerY = 70;
static constexpr int kGalleryDividerW = 3;
static constexpr int kGalleryDividerH = kMusicPanelY + kMusicPanelH - kGalleryDividerY;
static constexpr int kGalleryTimeCanvasX = 268;
static constexpr int kGalleryTimeCanvasY = 66;
static constexpr int kGalleryTimeCanvasW = 112;
static constexpr int kGalleryTimeCanvasH = 198;
static constexpr int kGallerySayingLabelX = 18;
static constexpr int kGallerySayingLabelY = 275;
static constexpr int kGallerySayingLabelW = 364;
static constexpr int kGallerySayingLabelH = 24;
static constexpr int kGalleryMinutesPerHour = 60;
static constexpr int kGalleryBlockDigitRows = 7;
static constexpr int kGalleryBlockDigitCols = 5;
static constexpr int kGalleryBlockDigitScale = 10;
static constexpr int kGalleryBlockDigitGap = 8;
static constexpr int kGalleryBlockDigitW = kGalleryBlockDigitCols * kGalleryBlockDigitScale;
static constexpr int kGalleryBlockDigitH = kGalleryBlockDigitRows * kGalleryBlockDigitScale;
static constexpr int kGalleryBlockNumberW = kGalleryBlockDigitW * 2 + kGalleryBlockDigitGap;
static constexpr int kGalleryTimeHourY = 15;
static constexpr int kGalleryTimeMinuteY = 116;

// 上次显示状态（避免闪烁）
static char s_last_music_file[64] = {};

static const char *const kBlockDigits[][kGalleryBlockDigitRows] = {
    {"11111", "10001", "10011", "10101", "11001", "10001", "11111"},
    {"00100", "01100", "00100", "00100", "00100", "00100", "01110"},
    {"11110", "00001", "00001", "11110", "10000", "10000", "11111"},
    {"11110", "00001", "00001", "01110", "00001", "00001", "11110"},
    {"10010", "10010", "10010", "11111", "00010", "00010", "00010"},
    {"11111", "10000", "10000", "11110", "00001", "00001", "11110"},
    {"01111", "10000", "10000", "11110", "10001", "10001", "01110"},
    {"11111", "00001", "00010", "00100", "01000", "01000", "01000"},
    {"01110", "10001", "10001", "01110", "10001", "10001", "01110"},
    {"01110", "10001", "10001", "01111", "00001", "00001", "11110"},
};
static constexpr int kGalleryBlockDigitCount = static_cast<int>(array_count(kBlockDigits));

static_assert(kGalleryBlockDigitCount > 0, "Gallery block digit table must not be empty");
static_assert(kGalleryTopLineW > 0 && kGalleryTopLineH > 0, "Gallery top separator size must be positive");
static_assert(kGalleryDividerW > 0 && kGalleryDividerH > 0, "Gallery divider size must be positive");
static_assert(kGalleryDividerY + kGalleryDividerH == kMusicPanelY + kMusicPanelH,
              "Gallery divider bottom must align with the music panel bottom");
static_assert(kGalleryTimeCanvasW > 0 && kGalleryTimeCanvasH > 0, "Gallery time canvas dimensions must be positive");
static_assert(kGallerySayingLabelW > 0 && kGallerySayingLabelH > 0, "Gallery saying label size must be positive");
static_assert(kGalleryMinutesPerHour > 0, "Gallery minutes per hour must be positive");
static_assert(kGalleryBlockDigitCount == 10, "Gallery block digit table must contain decimal digits");
static_assert(kGalleryBlockDigitRows > 0 && kGalleryBlockDigitCols > 0, "Gallery block digit grid must be positive");
static_assert(kGalleryBlockDigitScale > 1, "Gallery block digit scale must leave a visible gap");
static_assert(kGalleryBlockDigitGap >= 0, "Gallery block digit gap must not be negative");
static_assert(kGalleryBlockNumberW <= kGalleryTimeCanvasW, "Gallery block number must fit the time canvas width");
static_assert(kGalleryTimeHourY >= 0, "Gallery hour Y must not be negative");
static_assert(kGalleryTimeMinuteY >= 0, "Gallery minute Y must not be negative");
static_assert(kGalleryTimeHourY + kGalleryBlockDigitH <= kGalleryTimeCanvasH,
              "Gallery hour digits must fit the time canvas height");
static_assert(kGalleryTimeMinuteY + kGalleryBlockDigitH <= kGalleryTimeCanvasH,
              "Gallery minute digits must fit the time canvas height");

static void canvas_fill_rect(lv_obj_t *canvas, int x, int y, int w, int h, lv_color_t color)
{
    if (!canvas || w <= 0 || h <= 0) {
        return;
    }
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            canvas_set_px_safe(canvas, xx, yy, kGalleryTimeCanvasW, kGalleryTimeCanvasH, color);
        }
    }
}

static void draw_block_digit(lv_obj_t *canvas, int digit, int x, int y, int scale)
{
    if (digit < 0 || digit >= kGalleryBlockDigitCount) {
        return;
    }
    for (int row = 0; row < kGalleryBlockDigitRows; ++row) {
        for (int col = 0; col < kGalleryBlockDigitCols; ++col) {
            if (kBlockDigits[digit][row][col] == '1') {
                canvas_fill_rect(canvas, x + col * scale, y + row * scale, scale - 1, scale - 1, lv_color_black());
            }
        }
    }
}

static void draw_block_number(lv_obj_t *canvas, int value, int y)
{
    int x = (kGalleryTimeCanvasW - kGalleryBlockNumberW) / 2;
    draw_block_digit(canvas, value / 10, x, y, kGalleryBlockDigitScale);
    draw_block_digit(canvas, value % 10, x + kGalleryBlockDigitW + kGalleryBlockDigitGap, y, kGalleryBlockDigitScale);
}

static bool update_gallery_time_labels(const struct tm &local)
{
    if (!g_gallery_time_canvas || !s_gallery_time_canvas_buffer) {
        return false;
    }
    lv_canvas_fill_bg(g_gallery_time_canvas, lv_color_white(), LV_OPA_COVER);
    draw_block_number(g_gallery_time_canvas, local.tm_hour, kGalleryTimeHourY);
    draw_block_number(g_gallery_time_canvas, local.tm_min, kGalleryTimeMinuteY);
    lv_obj_invalidate(g_gallery_time_canvas);
    return true;
}

static bool update_gallery_image_for_date(const struct tm &local)
{
    if (!g_gallery_image_canvas || !s_gallery_image_canvas_buffer) {
        return false;
    }
    int custom_count = custom_assets_gallery_count();
    int builtin_image_count = CLOCK_GALLERY_IMAGE_COUNT;
    if (builtin_image_count <= 0) {
        return false;
    }
    // 按星期几选择图片（与原始逻辑一致）
    int image_index = local.tm_wday % builtin_image_count;
    if (image_index == s_last_gallery_image_index) {
        return false;
    }
    s_last_gallery_image_index = image_index;
    const uint8_t *image_bits = clock_gallery_images[image_index];
    if (custom_count > 0 &&
        custom_assets_read_gallery_image(image_index, s_custom_gallery_image, sizeof(s_custom_gallery_image))) {
        image_bits = s_custom_gallery_image;
    }
    draw_1bit_icon(g_gallery_image_canvas,
                   CLOCK_GALLERY_IMAGE_WIDTH,
                   CLOCK_GALLERY_IMAGE_HEIGHT,
                   CLOCK_GALLERY_IMAGE_BYTES_PER_ROW,
                   image_bits,
                   lv_color_black(),
                   lv_color_white());
    return true;
}

static bool update_gallery_saying_label()
{
    if (!g_gallery_saying_label) {
        return false;
    }
    char saying[kDailySayingLen] = {};
    get_daily_saying_snapshot(saying, sizeof(saying));
    return set_label_text_if_changed(g_gallery_saying_label, saying);
}

static void style_gallery_saying_label(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_obj_set_style_text_font(label, &zh_font_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static void build_gallery_canvas(lv_obj_t *screen,
                                 lv_obj_t **canvas,
                                 lv_color_t **buffer,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 const char *failure_log)
{
    if (!screen || !canvas || !buffer || width <= 0 || height <= 0) {
        return;
    }
    if (!ensure_canvas_buffer(buffer, width, height)) {
        return;
    }
    *canvas = lv_canvas_create(screen);
    if (!*canvas) {
        ESP_LOGW(TAG, "%s", failure_log);
        return;
    }
    configure_canvas_base(*canvas, *buffer, x, y, width, height);
    lv_canvas_fill_bg(*canvas, lv_color_white(), LV_OPA_COVER);
}

// 上次同步到music_set_page_active的音乐模式状态
static bool s_music_mode_synced = false;

// 切换Gallery子模式：星期图片模式 <-> 音乐播放模式
void gallery_set_music_mode(bool music_mode)
{
    g_gallery_music_mode.store(music_mode);
}

bool update_gallery_page(const struct tm &local)
{
    build_gallery_page();
    bool changed = false;

    // 同步音乐模式状态
    bool music_mode = g_gallery_music_mode.load();
    if (music_mode != s_music_mode_synced) {
        s_music_mode_synced = music_mode;

        // 星期图片canvas
        if (g_gallery_image_canvas) {
            if (music_mode) {
                lv_obj_add_flag(g_gallery_image_canvas, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(g_gallery_image_canvas, LV_OBJ_FLAG_HIDDEN);
                s_last_gallery_image_index = -1;
            }
        }

        // 音乐面板
        if (s_music_panel) {
            if (music_mode) {
                lv_obj_clear_flag(s_music_panel, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_music_panel, LV_OBJ_FLAG_HIDDEN);
            }
        }

        // 激活/停用音乐播放
        music_set_page_active(music_mode);
        changed = true;
    }

    int time_key = local.tm_hour * kGalleryMinutesPerHour + local.tm_min;
    if (time_key != s_last_gallery_time_key) {
        s_last_gallery_time_key = time_key;
        changed |= update_gallery_time_labels(local);
    }

    // 非音乐模式时更新星期图片
    if (!g_gallery_music_mode.load()) {
        changed |= update_gallery_image_for_date(local);
    }

    changed |= update_work_page_sensor_summary(g_gallery_summary_label);
    changed |= update_gallery_saying_label();

    // 音乐模式下更新当前文件名
    if (g_gallery_music_mode.load()) {
        MusicSnapshot snapshot = {};
        music_get_snapshot(&snapshot);

        if (strcmp(s_last_music_file, snapshot.current_file) != 0) {
            strlcpy(s_last_music_file, snapshot.current_file, sizeof(s_last_music_file));
            if (g_gallery_music_file_label) {
                changed |= set_label_text_if_changed(g_gallery_music_file_label, snapshot.current_file);
            }
        }

        // 播放中持续刷新
        if (snapshot.state == kMusicPlaying || snapshot.state == kMusicScanning) {
            changed = true;
        }
    }

    changed |= update_work_page_status_icons(kWorkPageGallery);
    return changed;
}

void build_gallery_page()
{
    if (g_gallery_root) {
        return;
    }
    lv_obj_t *screen = create_page_root();
    if (!screen) {
        return;
    }
    g_gallery_root = screen;

    build_battery_icon(screen, g_gallery_battery_segments);
    build_work_page_status_bar(screen,
                               kWorkPageGallery,
                               &g_gallery_date_label,
                               &g_gallery_summary_label,
                               nullptr,
                               false);

    lv_obj_t *top_line = make_bar(screen,
                                  kGalleryTopLineX,
                                  kGalleryTopLineY,
                                  kGalleryTopLineW,
                                  kGalleryTopLineH);
    set_obj_black(top_line, true);
    build_work_page_day_progress(screen, kWorkPageGallery);

    // === 星期图片区域（默认显示） ===
    if (!s_gallery_image_canvas_buffer) {
        s_gallery_image_canvas_buffer = alloc_canvas_buffer(CLOCK_GALLERY_IMAGE_WIDTH, CLOCK_GALLERY_IMAGE_HEIGHT);
    }
    if (s_gallery_image_canvas_buffer) {
        g_gallery_image_canvas = lv_canvas_create(screen);
    }
    if (g_gallery_image_canvas) {
        lv_obj_clear_flag(g_gallery_image_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(g_gallery_image_canvas, kGalleryImageCanvasX, kGalleryImageCanvasY);
        lv_obj_set_size(g_gallery_image_canvas, CLOCK_GALLERY_IMAGE_WIDTH, CLOCK_GALLERY_IMAGE_HEIGHT);
        lv_obj_set_style_border_width(g_gallery_image_canvas, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(g_gallery_image_canvas, 0, LV_PART_MAIN);
        lv_canvas_set_buffer(g_gallery_image_canvas,
                             s_gallery_image_canvas_buffer,
                             CLOCK_GALLERY_IMAGE_WIDTH,
                             CLOCK_GALLERY_IMAGE_HEIGHT,
                             LV_IMG_CF_TRUE_COLOR);
        lv_canvas_fill_bg(g_gallery_image_canvas, lv_color_white(), LV_OPA_COVER);
    } else if (s_gallery_image_canvas_buffer) {
        ESP_LOGW(TAG, "%s", GALLERY_IMAGE_CANVAS_CREATE_FAILED_LOG);
    }

    // === 音乐面板容器（默认隐藏） ===
    s_music_panel = lv_obj_create(screen);
    if (s_music_panel) {
        lv_obj_set_pos(s_music_panel, kMusicPanelX, kMusicPanelY);
        lv_obj_set_size(s_music_panel, kMusicPanelW, kMusicPanelH);
        lv_obj_set_style_radius(s_music_panel, kMusicPanelRadius, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_music_panel, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_music_panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_music_panel, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_music_panel, 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_music_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_music_panel, LV_OBJ_FLAG_HIDDEN);
    }

    // 歌曲文件名（白色，挂在面板内）
    lv_obj_t *music_parent = s_music_panel ? s_music_panel : screen;
    g_gallery_music_file_label = make_label_with_font(music_parent,
                                                       kMusicFileNameRelX,
                                                       kMusicFileNameRelY,
                                                       kMusicFileNameW,
                                                       kMusicFileNameH,
                                                       "",
                                                       &zh_font_16);
    if (g_gallery_music_file_label) {
        lv_obj_set_style_text_color(g_gallery_music_file_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(g_gallery_music_file_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_long_mode(g_gallery_music_file_label, LV_LABEL_LONG_DOT);
    }

    // 右侧分隔线
    lv_obj_t *divider = make_bar(screen,
                                 kGalleryDividerX,
                                 kGalleryDividerY,
                                 kGalleryDividerW,
                                 kGalleryDividerH);
    set_obj_black(divider, true);

    // 右侧时间Canvas
    build_gallery_canvas(screen,
                         &g_gallery_time_canvas,
                         &s_gallery_time_canvas_buffer,
                         kGalleryTimeCanvasX,
                         kGalleryTimeCanvasY,
                         kGalleryTimeCanvasW,
                         kGalleryTimeCanvasH,
                         GALLERY_TIME_CANVAS_CREATE_FAILED_LOG);

    // 底部每日语录
    g_gallery_saying_label = make_label(screen,
                                        kGallerySayingLabelX,
                                        kGallerySayingLabelY,
                                        kGallerySayingLabelW,
                                        kGallerySayingLabelH,
                                        "");
    if (!g_gallery_saying_label) {
        ESP_LOGW(TAG, "%s", GALLERY_SAYING_LABEL_CREATE_FAILED_LOG);
    } else {
        style_gallery_saying_label(g_gallery_saying_label);
    }

    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);
    update_battery_segments(g_gallery_battery_segments, battery_percent_load());
    s_last_gallery_image_index = -1;
    s_last_gallery_time_key = -1;
}
