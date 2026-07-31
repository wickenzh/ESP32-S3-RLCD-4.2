// 封装 RLCD 显示屏初始化、像素写入和整屏/局部刷新接口。
#include <string.h>
#include <limits>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "display_bsp.h"

#define PIXEL_OUT_OF_BOUNDS_LOG_FORMAT "Beyond the limit : (%d,%d)"
#define RLCD_TX_FAILED_LOG_FORMAT "RLCD tx failed err=%s len=%d offset=%d dma_free=%u dma_largest=%u"
#define RLCD_PARAM_TX_FAILED_LOG_FORMAT "RLCD %s tx failed value=0x%02x err=%s dma_free=%u dma_largest=%u"
#define RLCD_INIT_INVALID_SIZE_LOG_FORMAT "RLCD invalid display size width=%d height=%d"
#define RLCD_INIT_STAGE_FAILED_LOG_FORMAT "RLCD %s failed: %s"
#define RLCD_RELEASE_STAGE_FAILED_LOG_FORMAT "RLCD release %s failed: %s"

static constexpr int kRlcdSpiClockHz = 5 * 1000 * 1000;
static constexpr int kRlcdTxChunkBytes = 2048;
static constexpr int kRlcdOtaTxChunkBytes = 512;
static constexpr int kRlcdTxRetryCount = 4;
static constexpr int kRlcdOtaTxRetryCount = 8;
static constexpr int kRlcdTxRetryBaseDelayMs = 2;
static constexpr int kRlcdTxRetryStepDelayMs = 2;
static constexpr int kRlcdOtaTxRetryBaseDelayMs = 8;
static constexpr int kRlcdOtaTxRetryStepDelayMs = 4;
static constexpr int kRlcdLcdCommandBits = 8;
static constexpr int kRlcdLcdParamBits = 8;
static constexpr int kRlcdSpiMode = 0;
static constexpr int kRlcdSpiTransQueueDepth = 10;
static constexpr int kRlcdDmaConservativeMaxDepth = 8;
static constexpr const char *kRlcdKeepPinsActiveLog = "keep RLCD pins active in light sleep";
static constexpr uint32_t kRlcdSleepOutDelayMs = 200;
static constexpr uint32_t kRlcdResetHighDelayMs = 50;
static constexpr uint32_t kRlcdResetLowDelayMs = 20;
static constexpr TickType_t kRlcdSleepOutDelay = pdMS_TO_TICKS(kRlcdSleepOutDelayMs);
static constexpr TickType_t kRlcdResetHighDelay = pdMS_TO_TICKS(kRlcdResetHighDelayMs);
static constexpr TickType_t kRlcdResetLowDelay = pdMS_TO_TICKS(kRlcdResetLowDelayMs);
static_assert(kRlcdSleepOutDelayMs > 0, "RLCD sleep-out delay must be positive");
static_assert(kRlcdResetHighDelayMs > 0, "RLCD reset high delay must be positive");
static_assert(kRlcdResetLowDelayMs > 0, "RLCD reset low delay must be positive");
static_assert(kRlcdLcdCommandBits > 0, "RLCD command bit width must be positive");
static_assert(kRlcdLcdParamBits > 0, "RLCD parameter bit width must be positive");
static_assert(kRlcdTxChunkBytes > 0 && kRlcdOtaTxChunkBytes > 0,
              "RLCD transfer chunk sizes must be positive");
static_assert(kRlcdOtaTxChunkBytes <= kRlcdTxChunkBytes,
              "RLCD conservative transfer chunks must not exceed normal chunks");
static_assert(kRlcdTxRetryCount > 0 && kRlcdOtaTxRetryCount >= kRlcdTxRetryCount,
              "RLCD conservative retries must cover normal retries");
static_assert(kRlcdTxRetryBaseDelayMs > 0 && kRlcdTxRetryStepDelayMs >= 0,
              "RLCD normal retry delays must be valid");
static_assert(kRlcdOtaTxRetryBaseDelayMs >= kRlcdTxRetryBaseDelayMs &&
                  kRlcdOtaTxRetryStepDelayMs >= kRlcdTxRetryStepDelayMs,
              "RLCD conservative retry delays must not be shorter than normal delays");
static_assert(kRlcdSpiMode >= 0, "RLCD SPI mode must not be negative");
static_assert(kRlcdSpiTransQueueDepth > 0, "RLCD SPI transaction queue depth must be positive");
static_assert(kRlcdDmaConservativeMaxDepth > 0, "RLCD DMA conservative depth limit must be positive");
static_assert(kRlcdKeepPinsActiveLog[0] != '\0', "RLCD light-sleep pin log must not be empty");
static_assert(kRlcdSleepOutDelay > 0, "RLCD sleep-out tick delay must be positive");
static_assert(kRlcdResetHighDelay > 0, "RLCD reset high tick delay must be positive");
static_assert(kRlcdResetLowDelay > 0, "RLCD reset low tick delay must be positive");
static bool s_ota_quiet_mode = false;
static int s_dma_conservative_depth = 0;
static portMUX_TYPE s_dma_mode_mux = portMUX_INITIALIZER_UNLOCKED;

static int RlcdTxRetryDelayMs(bool conservative, int attempt)
{
    int base_delay = conservative ? kRlcdOtaTxRetryBaseDelayMs : kRlcdTxRetryBaseDelayMs;
    int step_delay = conservative ? kRlcdOtaTxRetryStepDelayMs : kRlcdTxRetryStepDelayMs;
    return base_delay + attempt * step_delay;
}

static bool RlcdTxCanRetry(esp_err_t err)
{
    return err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT;
}

static void LogDisplayAllocationFailure(const char *name, size_t bytes)
{
    ESP_LOGE("Display",
             "%s allocation failed bytes=%u psram_free=%u psram_largest=%u internal_free=%u dma_largest=%u",
             name,
             (unsigned)bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void LogDisplayStageFailure(const char *stage, esp_err_t err)
{
    ESP_LOGE("Display", RLCD_INIT_STAGE_FAILED_LOG_FORMAT, stage, esp_err_to_name(err));
}

static void LogDisplayReleaseFailure(const char *stage, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW("Display", RLCD_RELEASE_STAGE_FAILED_LOG_FORMAT, stage, esp_err_to_name(err));
    }
}

static bool Display_IsDmaConservativeMode()
{
    portENTER_CRITICAL(&s_dma_mode_mux);
    bool enabled = s_ota_quiet_mode || s_dma_conservative_depth > 0;
    portEXIT_CRITICAL(&s_dma_mode_mux);
    return enabled;
}

void Display_SetOtaQuietMode(bool enabled)
{
    portENTER_CRITICAL(&s_dma_mode_mux);
    s_ota_quiet_mode = enabled;
    portEXIT_CRITICAL(&s_dma_mode_mux);
}

void Display_AcquireDmaConservativeMode()
{
    portENTER_CRITICAL(&s_dma_mode_mux);
    if (s_dma_conservative_depth < kRlcdDmaConservativeMaxDepth) {
        ++s_dma_conservative_depth;
    }
    portEXIT_CRITICAL(&s_dma_mode_mux);
}

void Display_ReleaseDmaConservativeMode()
{
    portENTER_CRITICAL(&s_dma_mode_mux);
    if (s_dma_conservative_depth > 0) {
        --s_dma_conservative_depth;
    }
    portEXIT_CRITICAL(&s_dma_mode_mux);
}

DisplayPort::DisplayPort(int mosi, int scl, int dc, int cs, int rst, int width, int height, spi_host_device_t spihost) : 
mosi_(mosi), 
scl_(scl), 
dc_(dc), 
cs_(cs), 
rst_(rst), 
width_(width), 
height_(height),
spihost_(spihost)
{
    if (width_ <= 0 || height_ <= 0 ||
        width_ > std::numeric_limits<int>::max() / height_) {
        ESP_LOGE(TAG, RLCD_INIT_INVALID_SIZE_LOG_FORMAT, width_, height_);
        return;
    }

    esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi;
    buscfg.sclk_io_num                   = scl;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spihost_, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        LogDisplayStageFailure("SPI bus initialization", ret);
        return;
    }
    spi_bus_initialized_ = true;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = dc_;
    io_config.cs_gpio_num = cs_;
    io_config.pclk_hz = kRlcdSpiClockHz;
    io_config.lcd_cmd_bits = kRlcdLcdCommandBits;
    io_config.lcd_param_bits = kRlcdLcdParamBits;
    io_config.spi_mode = kRlcdSpiMode;
    io_config.trans_queue_depth = kRlcdSpiTransQueueDepth;

    ret = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)spihost_, &io_config, &io_handle);
    if (ret != ESP_OK) {
        LogDisplayStageFailure("panel IO creation", ret);
        ReleaseResources();
        return;
    }

    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ret = gpio_config(&gpio_conf);
    if (ret != ESP_OK) {
        LogDisplayStageFailure("reset GPIO configuration", ret);
        ReleaseResources();
        return;
    }
    reset_gpio_configured_ = true;

    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    if (DispBuffer == NULL) {
        LogDisplayAllocationFailure("RLCD display buffer", DisplayLen);
        ReleaseResources();
        return;
    }

#if (AlgorithmOptimization == 3)
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (PixelIndexLUT == NULL) {
        LogDisplayAllocationFailure("RLCD pixel index LUT", transfer * sizeof(uint16_t));
    }
    if (PixelBitLUT == NULL) {
        LogDisplayAllocationFailure("RLCD pixel bit LUT", transfer * sizeof(uint8_t));
    }
    if (!PixelIndexLUT || !PixelBitLUT) {
        ReleaseResources();
        return;
    }
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }
#endif
    ready_ = true;
}

DisplayPort::~DisplayPort() {
    ReleaseResources();
}

void DisplayPort::ReleaseResources() {
    ready_ = false;
    initializing_ = false;
#if (AlgorithmOptimization == 3)
    free(PixelBitLUT);
    PixelBitLUT = NULL;
    free(PixelIndexLUT);
    PixelIndexLUT = NULL;
#endif
    free(DispBuffer);
    DispBuffer = NULL;
    DisplayLen = 0;
    if (reset_gpio_configured_) {
        LogDisplayReleaseFailure("reset GPIO", gpio_reset_pin((gpio_num_t)rst_));
        reset_gpio_configured_ = false;
    }
    if (io_handle) {
        LogDisplayReleaseFailure("panel IO", esp_lcd_panel_io_del(io_handle));
        io_handle = NULL;
    }
    if (spi_bus_initialized_) {
        LogDisplayReleaseFailure("SPI bus", spi_bus_free(spihost_));
        spi_bus_initialized_ = false;
    }
}

bool DisplayPort::IsReady() const {
    return ready_;
}

void DisplayPort::RLCD_Init() {
    if (!ready_) {
        return;
    }
    RLCD_Reset();
    KeepPinsActiveInLightSleep();
    initializing_ = true;

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(kRlcdSleepOutDelay);
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    initializing_ = false;
    RLCD_ColorClear(ColorWhite);
}

void DisplayPort::KeepPinsActiveInLightSleep(void) {
    const gpio_num_t pins[] = {
        (gpio_num_t)mosi_,
        (gpio_num_t)scl_,
        (gpio_num_t)dc_,
        (gpio_num_t)cs_,
        (gpio_num_t)rst_,
    };

    for (gpio_num_t pin : pins) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_sleep_sel_dis(pin));
    }
    ESP_LOGI(TAG, "%s", kRlcdKeepPinsActiveLog);
}

void DisplayPort::RLCD_ColorClear(uint8_t color) {
    if (!ready_ || !DispBuffer || DisplayLen <= 0) {
        return;
    }
    memset(DispBuffer, color, DisplayLen);
}

void DisplayPort::RLCD_Display() {
    if (!ready_ || !DispBuffer) {
        return;
    }
    if (!RLCD_SendCommand(0x2A) ||     // Column Address Set
        !RLCD_SendData(0x12) ||
        !RLCD_SendData(0x2A) ||
        !RLCD_SendCommand(0x2B) ||     // Page Address Set
        !RLCD_SendData(0x00) ||
        !RLCD_SendData(0xC7) ||
        !RLCD_SendCommand(0x2c)) {     // Memory Write
        return;
    }

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

void DisplayPort::RLCD_DisplayXRange(uint16_t x1, uint16_t x2) {
    if (!ready_ || !DispBuffer || x1 >= (uint16_t)width_ ||
        x2 >= (uint16_t)width_ || x1 > x2) {
        return;
    }
    uint16_t start_pair = x1 >> 1;
    uint16_t end_pair = x2 >> 1;
    uint16_t rows_per_pair = height_ >> 2;
    uint32_t offset = (uint32_t)start_pair * rows_per_pair;
    uint32_t len = (uint32_t)(end_pair - start_pair + 1) * rows_per_pair;

    if (!RLCD_SendCommand(0x2A) ||
        !RLCD_SendData(0x12) ||
        !RLCD_SendData(0x2A) ||
        !RLCD_SendCommand(0x2B) ||
        !RLCD_SendData(start_pair & 0xFF) ||
        !RLCD_SendData(end_pair & 0xFF) ||
        !RLCD_SendCommand(0x2c)) {
        return;
    }

	RLCD_Sendbuffera(DispBuffer + offset, len);
}

void DisplayPort::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(kRlcdResetHighDelay);
    Set_ResetIOLevel(0);
    vTaskDelay(kRlcdResetLowDelay);
    Set_ResetIOLevel(1);
    vTaskDelay(kRlcdResetHighDelay);
}

bool DisplayPort::RLCD_SendParamChecked(int command,
                                       const void *data,
                                       size_t data_size,
                                       const char *kind,
                                       uint8_t value) {
    if (!ready_ || !io_handle) {
        return false;
    }
    const bool conservative = Display_IsDmaConservativeMode();
    const int retry_count = conservative ? kRlcdOtaTxRetryCount : kRlcdTxRetryCount;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < retry_count; ++attempt) {
        err = esp_lcd_panel_io_tx_param(io_handle, command, data, data_size);
        if (err == ESP_OK) {
            return true;
        }
        if (!RlcdTxCanRetry(err)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(RlcdTxRetryDelayMs(conservative, attempt)));
    }

    ESP_LOGW(TAG,
             RLCD_PARAM_TX_FAILED_LOG_FORMAT,
             kind,
             value,
             esp_err_to_name(err),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    if (initializing_) {
        ESP_ERROR_CHECK(err);
    }
    return false;
}

bool DisplayPort::RLCD_SendCommand(uint8_t Reg) {
    return RLCD_SendParamChecked(Reg, NULL, 0, "command", Reg);
}

bool DisplayPort::RLCD_SendData(uint8_t Data) {
    return RLCD_SendParamChecked(-1, &Data, 1, "data", Data);
}

void DisplayPort::RLCD_Sendbuffera(uint8_t *Data, int len) {
    if (!ready_ || !io_handle || !Data || len <= 0) {
        return;
    }
    int offset = 0;
    const bool quiet = Display_IsDmaConservativeMode();
    const int max_chunk = quiet ? kRlcdOtaTxChunkBytes : kRlcdTxChunkBytes;
    const int retry_count = quiet ? kRlcdOtaTxRetryCount : kRlcdTxRetryCount;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > max_chunk) {
            chunk = max_chunk;
        }

        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < retry_count; ++attempt) {
            err = esp_lcd_panel_io_tx_color(io_handle, -1, Data + offset, chunk);
            if (err == ESP_OK) {
                break;
            }
            if (!RlcdTxCanRetry(err)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(RlcdTxRetryDelayMs(quiet, attempt)));
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     RLCD_TX_FAILED_LOG_FORMAT,
                     esp_err_to_name(err),
                     len,
                     offset,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            return;
        }
        offset += chunk;
    }
}

void DisplayPort::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}
#if (AlgorithmOptimization != 3)

void DisplayPort::RLCD_SetPortraitPixel(uint16_t x, uint16_t y, uint8_t color) {
    if((x >= width_) || (y >= height_)) {
        ESP_LOGE("Pixel", PIXEL_OUT_OF_BOUNDS_LOG_FORMAT, x, y);
        return;
  	}
#if (AlgorithmOptimization == 2)
	const uint16_t W4 = width_ >> 2;  

    uint16_t byte_x = x >> 2;        
    uint16_t byte_y = y >> 1;        

    uint32_t index = byte_y * W4 + byte_x;

    uint8_t local_x = x & 0x03; 
    uint8_t local_y = y & 0x01; 

    uint8_t bit = 7 - ((local_x << 1) | local_y);

    uint8_t mask = 1 << bit;

    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t byte_x = x / 4;
    uint16_t byte_y = y / 2;

    uint32_t index = byte_y * (width_ / 4) + byte_x;

    uint8_t local_x = x % 4;  
    uint8_t local_y = y % 2;  
    uint8_t bit = 7 - (local_x * 2 + local_y);
    if (color)
        DispBuffer[index] |=  (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

void DisplayPort::RLCD_SetLandscapePixel(uint16_t x, uint16_t y, uint8_t color) {
    if (x >= width_ || y >= height_)
        return;
#if (AlgorithmOptimization == 2)

	uint16_t inv_y = (height_ - 1 - y);
    const uint16_t H4 = height_ >> 2;  
    uint16_t byte_x = x >> 1;          
    uint16_t block_y = inv_y >> 2;     
    uint32_t index = byte_x * H4 + block_y;
    uint8_t local_x = x & 0x01;        
    uint8_t local_y = inv_y & 0x03;    
    uint8_t bit = 7 - ((local_y << 1) | local_x);
    uint8_t mask = 1 << bit;
    if (color)
        DispBuffer[index] |= mask;
    else
        DispBuffer[index] &= ~mask;
#else
    uint16_t inv_y = height_ - 1 - y;

    uint16_t byte_x  = x / 2;           // 0..199
    uint16_t block_y = inv_y / 4;       // 0..74

    uint32_t index = byte_x * (height_ / 4) + block_y; 

    uint8_t local_x = x % 2;            // 0 or 1
    uint8_t local_y = inv_y % 4;        // 0..3

    uint8_t bit = 7 - (local_y * 2 + local_x);

    if (color)
        DispBuffer[index] |= (1 << bit);
    else
        DispBuffer[index] &= ~(1 << bit);
#endif
}

#endif


#if (AlgorithmOptimization == 3)

void DisplayPort::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void DisplayPort::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;

    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;

        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void DisplayPort::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    if (!ready_ || !DispBuffer || !PixelIndexLUT || !PixelBitLUT ||
        x >= static_cast<uint16_t>(width_) || y >= static_cast<uint16_t>(height_)) {
        return;
    }
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

#endif
