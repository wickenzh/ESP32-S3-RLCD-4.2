// 封装 LVGL 初始化、锁和显示驱动对接逻辑。
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "lvgl_memory_pool.h"
#include "lvgl_lock_health.h"
#include <esp_system.h>
#include <esp_attr.h>
#include <assert.h>

alignas(8) static EXT_RAM_BSS_ATTR uint8_t s_lvgl_pool[256 * 1024];
extern "C" void *weather_clock_lvgl_pool(size_t size) {
    assert(size == sizeof(s_lvgl_pool));
    return s_lvgl_pool;
}
#include <esp_timer.h>
#include <limits>
#include "lvgl_bsp.h"

#define LVGL_BUFFER_ALLOCATION_FAILED_FORMAT "%s allocation failed bytes=%u psram_free=%u psram_largest=%u internal_free=%u dma_largest=%u"
#define LVGL_DRAW_BUFFER_LOG_FORMAT "LVGL draw buffer: %dx%d rows x2, %u bytes each"
#define LVGL_INIT_INVALID_CONFIG_FORMAT \
    "LVGL invalid init config width=%d height=%d flush_cb=%d"
static lv_disp_draw_buf_t disp_buf; 		// contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      		// contains callback functions
static StaticSemaphore_t lvgl_mux_storage = {};
static SemaphoreHandle_t lvgl_mux = NULL;
static LvglLockHealth lvgl_lock_health;
static portMUX_TYPE lvgl_task_handle_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t lvgl_task_handle = NULL;
static int64_t lvgl_tick_last_us = 0;

static const char *TAG = "LvglPort";
static constexpr int kDrawBufferRows = 40;
static constexpr const char *kLvglTaskName = "LVGL";
static constexpr const char *kLvglLockBeforeInitLog = "LVGL lock requested before init";
static constexpr const char *kLvglUnlockBeforeInitLog = "LVGL unlock requested before init";
static constexpr const char *kLvglMutexCreateFailedLog = "LVGL mutex creation failed";
static constexpr const char *kLvglDisplayRegisterFailedLog = "LVGL display registration failed";
static constexpr const char *kLvglRefreshTimerUnavailableLog =
    "LVGL display refresh timer unavailable";
static constexpr const char *kLvglRegisterDisplayLog = "Register display driver to LVGL";
static constexpr const char *kLvglTaskCreateFailedLog = "LVGL task creation failed";
static constexpr uint32_t kLvglTaskStackBytes = 8 * 1024;
static constexpr UBaseType_t kLvglTaskPriority = 5;
static constexpr BaseType_t kLvglTaskCore = 0;
static constexpr int64_t kMicrosecondsPerMillisecond = 1000;
static_assert(kDrawBufferRows > 0, "LVGL draw buffer rows must be positive");
static_assert(kLvglTaskName[0] != '\0', "LVGL task name must not be empty");
static_assert(kLvglLockBeforeInitLog[0] != '\0', "LVGL lock-before-init log must not be empty");
static_assert(kLvglUnlockBeforeInitLog[0] != '\0', "LVGL unlock-before-init log must not be empty");
static_assert(kLvglMutexCreateFailedLog[0] != '\0', "LVGL mutex failure log must not be empty");
static_assert(kLvglDisplayRegisterFailedLog[0] != '\0', "LVGL display failure log must not be empty");
static_assert(kLvglRefreshTimerUnavailableLog[0] != '\0',
              "LVGL refresh-timer failure log must not be empty");
static_assert(kLvglRegisterDisplayLog[0] != '\0', "LVGL register-display log must not be empty");
static_assert(kLvglTaskCreateFailedLog[0] != '\0', "LVGL task failure log must not be empty");
static_assert(kLvglTaskStackBytes > 0, "LVGL task stack size must be positive");
static_assert(kLvglTaskStackBytes % sizeof(StackType_t) == 0,
              "LVGL task stack must align to StackType_t");
static_assert(kLvglTaskPriority > tskIDLE_PRIORITY, "LVGL task priority must exceed idle");
static_assert(kLvglTaskCore >= 0, "LVGL task core must be non-negative");
static_assert(kMicrosecondsPerMillisecond == 1000, "millisecond to microsecond conversion must stay stable");

static StackType_t lvgl_task_stack[kLvglTaskStackBytes / sizeof(StackType_t)] = {};
static StaticTask_t lvgl_task_storage = {};

static bool ComputeLvglDrawBufferGeometry(int width,
                                          int height,
                                          DispFlushCb flush_cb,
                                          int *buffer_rows_out,
                                          size_t *buffer_pixels_out,
                                          size_t *buffer_bytes_out)
{
    if (!buffer_rows_out || !buffer_pixels_out || !buffer_bytes_out) {
        return false;
    }
    *buffer_rows_out = 0;
    *buffer_pixels_out = 0;
    *buffer_bytes_out = 0;
    if (width <= 0 || height <= 0 || !flush_cb) {
        return false;
    }
    const int buffer_rows = height < kDrawBufferRows ? height : kDrawBufferRows;
    if (static_cast<size_t>(width) >
        std::numeric_limits<size_t>::max() / static_cast<size_t>(buffer_rows)) {
        return false;
    }
    const size_t pixels =
        static_cast<size_t>(width) * static_cast<size_t>(buffer_rows);
    if (pixels > std::numeric_limits<size_t>::max() / sizeof(lv_color_t)) {
        return false;
    }
    *buffer_rows_out = buffer_rows;
    *buffer_pixels_out = pixels;
    *buffer_bytes_out = pixels * sizeof(lv_color_t);
    return true;
}

static TaskHandle_t LoadLvglTaskHandle()
{
    portENTER_CRITICAL(&lvgl_task_handle_mux);
    const TaskHandle_t task_handle = lvgl_task_handle;
    portEXIT_CRITICAL(&lvgl_task_handle_mux);
    return task_handle;
}

static void StoreLvglTaskHandle(TaskHandle_t task_handle)
{
    portENTER_CRITICAL(&lvgl_task_handle_mux);
    lvgl_task_handle = task_handle;
    portEXIT_CRITICAL(&lvgl_task_handle_mux);
}

static void LogLvglBufferAllocationFailure(const char *name, size_t bytes)
{
    ESP_LOGE(TAG,
             LVGL_BUFFER_ALLOCATION_FAILED_FORMAT,
             name,
             (unsigned)bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void UpdateLvglTickLocked()
{
    const int64_t now_us = esp_timer_get_time();
    if (lvgl_tick_last_us == 0 || now_us < lvgl_tick_last_us) {
        lvgl_tick_last_us = now_us;
        return;
    }
    const uint64_t elapsed_ms =
        static_cast<uint64_t>((now_us - lvgl_tick_last_us) / kMicrosecondsPerMillisecond);
    if (elapsed_ms == 0) {
        return;
    }
    lv_tick_inc(static_cast<uint32_t>(elapsed_ms));
    lvgl_tick_last_us += static_cast<int64_t>(elapsed_ms) * kMicrosecondsPerMillisecond;
}

static void ReleaseLvglInitResources(lv_disp_t *display,
                                     lv_color_t *buffer1,
                                     lv_color_t *buffer2)
{
    if (display != NULL) {
        lv_disp_remove(display);
    }
    lv_deinit();
    if (buffer2 != NULL) {
        heap_caps_free(buffer2);
    }
    if (buffer1 != NULL) {
        heap_caps_free(buffer1);
    }
    disp_buf = {};
    disp_drv = {};
    lvgl_tick_last_us = 0;
    StoreLvglTaskHandle(NULL);
    if (lvgl_mux != NULL) {
        vSemaphoreDelete(lvgl_mux);
        lvgl_mux = NULL;
    }
}

bool Lvgl_lock(int timeout_ms)
{
    if (lvgl_mux == NULL) {
        ESP_LOGW(TAG, "%s", kLvglLockBeforeInitLog);
        return false;
    }
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms<0?1000:timeout_ms);
    while (xSemaphoreTake(lvgl_mux, timeout_ticks) != pdTRUE) {
        if(lvgl_lock_health.failed(xTaskGetTickCount(),pdMS_TO_TICKS(60000))) {
            const TaskHandle_t owner=xSemaphoreGetMutexHolder(lvgl_mux);
            ESP_LOGE(TAG,"LVGL stalled 60s: caller=%s owner=%s internal=%u dma_largest=%u; restarting",
                     pcTaskGetName(nullptr),owner?pcTaskGetName(owner):"none",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            esp_restart();
        }
        if(timeout_ms>=0)return false;
    }
    lvgl_lock_health.progress();
    UpdateLvglTickLocked();
    return true;
}

void Lvgl_unlock(void)
{
    if (lvgl_mux == NULL) {
        ESP_LOGW(TAG, "%s", kLvglUnlockBeforeInitLog);
        return;
    }
    const TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    const TaskHandle_t task_handle = LoadLvglTaskHandle();
    xSemaphoreGive(lvgl_mux);
    if (task_handle != NULL && caller != task_handle) {
        xTaskNotifyGive(task_handle);
    }
}

static void Lvgl_port_task(void *arg)
{
    StoreLvglTaskHandle(xTaskGetCurrentTaskHandle());
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (Lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            Lvgl_unlock();
        }
        if (task_delay_ms == LV_NO_TIMER_READY) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(task_delay_ms));
    }
}


bool Lvgl_PortInit(int width, int height,DispFlushCb flush_cb) {
    if (lvgl_mux != NULL) {
        return true;
    }
    int buffer_rows = 0;
    size_t buffer_pixels = 0;
    size_t buffer_bytes = 0;
    if (!ComputeLvglDrawBufferGeometry(width,
                                       height,
                                       flush_cb,
                                       &buffer_rows,
                                       &buffer_pixels,
                                       &buffer_bytes)) {
        ESP_LOGE(TAG,
                 LVGL_INIT_INVALID_CONFIG_FORMAT,
                 width,
                 height,
                 flush_cb ? 1 : 0);
        return false;
    }
    lvgl_mux = xSemaphoreCreateMutexStatic(&lvgl_mux_storage);
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "%s", kLvglMutexCreateFailedLog);
        return false;
    }
    lv_init();
    lv_color_t *buffer1 = (lv_color_t *)heap_caps_malloc(buffer_bytes,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer1 == NULL) {
        LogLvglBufferAllocationFailure("LVGL draw buffer 1", buffer_bytes);
        ReleaseLvglInitResources(NULL, NULL, NULL);
        return false;
    }
	lv_color_t *buffer2 = (lv_color_t *)heap_caps_malloc(buffer_bytes,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer2 == NULL) {
        LogLvglBufferAllocationFailure("LVGL draw buffer 2", buffer_bytes);
        ReleaseLvglInitResources(NULL, buffer1, NULL);
        return false;
    }

    lv_disp_draw_buf_init(&disp_buf, buffer1, buffer2, buffer_pixels);
    ESP_LOGI(TAG, LVGL_DRAW_BUFFER_LOG_FORMAT,
             width, buffer_rows, (unsigned)(buffer_pixels * sizeof(lv_color_t)));
    ESP_LOGI(TAG, "%s", kLvglRegisterDisplayLog);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = width;
    disp_drv.ver_res = height;
    disp_drv.flush_cb = flush_cb;
    disp_drv.full_refresh = 0;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_t *display = lv_disp_drv_register(&disp_drv);
    if (display == NULL) {
        ESP_LOGE(TAG, "%s", kLvglDisplayRegisterFailedLog);
        ReleaseLvglInitResources(NULL, buffer1, buffer2);
        return false;
    }
    lv_timer_t *display_refresh_timer = _lv_disp_get_refr_timer(display);
    if (display_refresh_timer == NULL) {
        ESP_LOGE(TAG, "%s", kLvglRefreshTimerUnavailableLog);
        ReleaseLvglInitResources(display, buffer1, buffer2);
        return false;
    }
    // All project UI owners explicitly refresh after changing LVGL objects.
    // Pause LVGL's periodic display timer so static pages do not wake once per
    // second merely to inspect an empty invalidation list.
    lv_timer_pause(display_refresh_timer);

    lvgl_tick_last_us = esp_timer_get_time();

    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(Lvgl_port_task,
                                                              kLvglTaskName,
                                                              kLvglTaskStackBytes,
                                                              NULL,
                                                              kLvglTaskPriority,
                                                              lvgl_task_stack,
                                                              &lvgl_task_storage,
                                                              kLvglTaskCore);
    if (task_handle == NULL) {
        ESP_LOGE(TAG, "%s", kLvglTaskCreateFailedLog);
        ReleaseLvglInitResources(display, buffer1, buffer2);
        return false;
    }
    StoreLvglTaskHandle(task_handle);
    return true;
}
