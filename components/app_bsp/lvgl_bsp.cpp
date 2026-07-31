// 封装 LVGL 初始化、锁和显示驱动对接逻辑。
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

#define LVGL_BUFFER_ALLOCATION_FAILED_FORMAT "%s allocation failed bytes=%u psram_free=%u psram_largest=%u internal_free=%u dma_largest=%u"
#define LVGL_DRAW_BUFFER_LOG_FORMAT "LVGL draw buffer: %dx%d rows x2, %u bytes each"
#define LVGL_TICK_CREATE_FAILED_LOG_FORMAT "LVGL tick timer creation failed: %s"
#define LVGL_TICK_START_FAILED_LOG_FORMAT "LVGL tick timer start failed: %s"
#define LVGL_TICK_STOP_FAILED_LOG_FORMAT "LVGL tick timer rollback stop failed: %s"
#define LVGL_TICK_DELETE_FAILED_LOG_FORMAT "LVGL tick timer rollback delete failed: %s"

static lv_disp_draw_buf_t disp_buf; 		// contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      		// contains callback functions
static SemaphoreHandle_t lvgl_mux = NULL;

static const char *TAG = "LvglPort";
static constexpr int kDrawBufferRows = 40;
static constexpr const char *kLvglTickTimerName = "lvgl_tick";
static constexpr const char *kLvglTaskName = "LVGL";
static constexpr const char *kLvglLockBeforeInitLog = "LVGL lock requested before init";
static constexpr const char *kLvglUnlockBeforeInitLog = "LVGL unlock requested before init";
static constexpr const char *kLvglMutexCreateFailedLog = "LVGL mutex creation failed";
static constexpr const char *kLvglDisplayRegisterFailedLog = "LVGL display registration failed";
static constexpr const char *kLvglRegisterDisplayLog = "Register display driver to LVGL";
static constexpr const char *kLvglInstallTickTimerLog = "Install LVGL tick timer";
static constexpr const char *kLvglTaskCreateFailedLog = "LVGL task creation failed";
static constexpr uint32_t kLvglTaskStackWords = 8 * 1024;
static constexpr UBaseType_t kLvglTaskPriority = 5;
static constexpr BaseType_t kLvglTaskCore = 0;
static constexpr uint64_t kMicrosecondsPerMillisecond = 1000;
static constexpr uint64_t kLvglTickPeriodUs = LVGL_TICK_PERIOD_MS * kMicrosecondsPerMillisecond;
static_assert(kDrawBufferRows > 0, "LVGL draw buffer rows must be positive");
static_assert(kLvglTickTimerName[0] != '\0', "LVGL tick timer name must not be empty");
static_assert(kLvglTaskName[0] != '\0', "LVGL task name must not be empty");
static_assert(kLvglLockBeforeInitLog[0] != '\0', "LVGL lock-before-init log must not be empty");
static_assert(kLvglUnlockBeforeInitLog[0] != '\0', "LVGL unlock-before-init log must not be empty");
static_assert(kLvglMutexCreateFailedLog[0] != '\0', "LVGL mutex failure log must not be empty");
static_assert(kLvglDisplayRegisterFailedLog[0] != '\0', "LVGL display failure log must not be empty");
static_assert(kLvglRegisterDisplayLog[0] != '\0', "LVGL register-display log must not be empty");
static_assert(kLvglInstallTickTimerLog[0] != '\0', "LVGL tick timer install log must not be empty");
static_assert(kLvglTaskCreateFailedLog[0] != '\0', "LVGL task failure log must not be empty");
static_assert(kLvglTaskStackWords > 0, "LVGL task stack size must be positive");
static_assert(kLvglTaskPriority > tskIDLE_PRIORITY, "LVGL task priority must exceed idle");
static_assert(kLvglTaskCore >= 0, "LVGL task core must be non-negative");
static_assert(kMicrosecondsPerMillisecond == 1000, "millisecond to microsecond conversion must stay stable");
static_assert(kLvglTickPeriodUs > 0, "LVGL tick period must be positive");

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

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void ReleaseLvglInitResources(lv_disp_t *display,
                                     esp_timer_handle_t tick_timer,
                                     bool tick_timer_started,
                                     lv_color_t *buffer1,
                                     lv_color_t *buffer2)
{
    if (tick_timer_started) {
        esp_err_t err = esp_timer_stop(tick_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, LVGL_TICK_STOP_FAILED_LOG_FORMAT, esp_err_to_name(err));
        }
    }
    if (tick_timer != NULL) {
        esp_err_t err = esp_timer_delete(tick_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, LVGL_TICK_DELETE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        }
    }
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
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void Lvgl_unlock(void)
{
    if (lvgl_mux == NULL) {
        ESP_LOGW(TAG, "%s", kLvglUnlockBeforeInitLog);
        return;
    }
  	xSemaphoreGive(lvgl_mux);
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1)) 
  	  	{
  	  	  	task_delay_ms = lv_timer_handler();
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  	}
}


bool Lvgl_PortInit(int width, int height,DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    if (lvgl_mux == NULL) {
        ESP_LOGE(TAG, "%s", kLvglMutexCreateFailedLog);
        return false;
    }
    lv_init();
    int buffer_rows = height < kDrawBufferRows ? height : kDrawBufferRows;
    size_t buffer_pixels = (size_t)width * buffer_rows;
    size_t buffer_bytes = buffer_pixels * sizeof(lv_color_t);
    lv_color_t *buffer1 = (lv_color_t *)heap_caps_malloc(buffer_pixels * sizeof(lv_color_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer1 == NULL) {
        LogLvglBufferAllocationFailure("LVGL draw buffer 1", buffer_bytes);
        ReleaseLvglInitResources(NULL, NULL, false, NULL, NULL);
        return false;
    }
	lv_color_t *buffer2 = (lv_color_t *)heap_caps_malloc(buffer_pixels * sizeof(lv_color_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer2 == NULL) {
        LogLvglBufferAllocationFailure("LVGL draw buffer 2", buffer_bytes);
        ReleaseLvglInitResources(NULL, NULL, false, buffer1, NULL);
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
        ReleaseLvglInitResources(NULL, NULL, false, buffer1, buffer2);
        return false;
    }

    ESP_LOGI(TAG, "%s", kLvglInstallTickTimerLog);
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
    lvgl_tick_timer_args.name = kLvglTickTimerName;
    lvgl_tick_timer_args.skip_unhandled_events = true;
    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_err_t err = esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, LVGL_TICK_CREATE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        ReleaseLvglInitResources(display, NULL, false, buffer1, buffer2);
        return false;
    }
    err = esp_timer_start_periodic(lvgl_tick_timer, kLvglTickPeriodUs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, LVGL_TICK_START_FAILED_LOG_FORMAT, esp_err_to_name(err));
        ReleaseLvglInitResources(display, lvgl_tick_timer, false, buffer1, buffer2);
        return false;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(Lvgl_port_task,
                                                      kLvglTaskName,
                                                      kLvglTaskStackWords,
                                                      NULL,
                                                      kLvglTaskPriority,
                                                      NULL,
                                                      kLvglTaskCore);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "%s", kLvglTaskCreateFailedLog);
        ReleaseLvglInitResources(display, lvgl_tick_timer, true, buffer1, buffer2);
        return false;
    }
    return true;
}
