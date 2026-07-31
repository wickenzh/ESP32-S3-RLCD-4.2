// 封装 SHTC3、PCF85063 等 I2C 外设的设备级操作。
#include <stdio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include "i2c_equipment.h"
#include "i2c_bsp.h"
#include "SensorPCF85063.hpp"

namespace {
constexpr uint32_t kShtc3PostResetDelayMs = 20;
constexpr uint32_t kShtc3WakeupDelayMs = 50;
constexpr uint32_t kShtc3MeasureDelayMs = 20;
constexpr uint32_t kShtc3I2cSpeedHz = 400000;
constexpr uint32_t kRtcI2cSpeedHz = 300000;
constexpr const char *kShtc3LogTag = "shtc3";
constexpr const char *kRtcLogTag = "rtc";
#define SHTC3_DEVICE_ADD_FAILED_FORMAT "device add failed: %s"
#define RTC_DEVICE_ADD_FAILED_FORMAT "device add failed: %s"
constexpr float kShtc3RawFullScale = 65536.0f;
constexpr float kShtc3TemperatureScaleC = 175.0f;
constexpr float kShtc3TemperatureOffsetC = -45.0f;
constexpr float kShtc3HumidityScalePercent = 100.0f;
constexpr TickType_t kShtc3PostResetDelay = pdMS_TO_TICKS(kShtc3PostResetDelayMs);
constexpr TickType_t kShtc3WakeupDelay = pdMS_TO_TICKS(kShtc3WakeupDelayMs);
constexpr TickType_t kShtc3MeasureDelay = pdMS_TO_TICKS(kShtc3MeasureDelayMs);
static_assert(kShtc3PostResetDelayMs > 0, "SHTC3 post-reset delay must be positive");
static_assert(kShtc3WakeupDelayMs > 0, "SHTC3 wakeup delay must be positive");
static_assert(kShtc3MeasureDelayMs > 0, "SHTC3 measure delay must be positive");
static_assert(kShtc3I2cSpeedHz > 0, "SHTC3 I2C speed must be positive");
static_assert(kRtcI2cSpeedHz > 0, "RTC I2C speed must be positive");
static_assert(kShtc3LogTag[0] != '\0', "SHTC3 log tag must not be empty");
static_assert(kRtcLogTag[0] != '\0', "RTC log tag must not be empty");
static_assert(kShtc3RawFullScale > 0.0f, "SHTC3 raw full scale must be positive");
static_assert(kShtc3TemperatureScaleC > 0.0f, "SHTC3 temperature scale must be positive");
static_assert(kShtc3HumidityScalePercent > 0.0f, "SHTC3 humidity scale must be positive");
static_assert(kShtc3PostResetDelay > 0, "SHTC3 post-reset tick delay must be positive");
static_assert(kShtc3WakeupDelay > 0, "SHTC3 wakeup tick delay must be positive");
static_assert(kShtc3MeasureDelay > 0, "SHTC3 measure tick delay must be positive");
} // namespace

Shtc3Port::Shtc3Port(I2cMasterBus& i2cbus) :
i2cbus_(i2cbus) {
    if (!i2cbus_.IsReady()) {
        ESP_LOGW(kShtc3LogTag,
                 SHTC3_DEVICE_ADD_FAILED_FORMAT,
                 esp_err_to_name(ESP_ERR_INVALID_STATE));
        return;
    }
    i2c_master_bus_handle_t I2cMasterBus = i2cbus_.Get_I2cBusHandle();
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = Shtc3Address;
    dev_cfg.scl_speed_hz    = kShtc3I2cSpeedHz;
    esp_err_t err = i2c_master_bus_add_device(I2cMasterBus, &dev_cfg, &I2c_DevShtc3);
    if (err != ESP_OK) {
        ESP_LOGW(kShtc3LogTag, SHTC3_DEVICE_ADD_FAILED_FORMAT, esp_err_to_name(err));
        return;
    }

    Shtc3_Wakeup();
    Shtc3_SoftReset();
    vTaskDelay(kShtc3PostResetDelay);
    Shtc3_GetId();
    Shtc3_Sleep();
    ESP_LOGI(TAG, "ID:%04x", shtc3_id);
}

Shtc3Port::~Shtc3Port() {
    if (!I2c_DevShtc3) {
        return;
    }
    esp_err_t err = i2c_master_bus_rm_device(I2c_DevShtc3);
    if (err != ESP_OK) {
        ESP_LOGW(kShtc3LogTag, "device remove failed: %s", esp_err_to_name(err));
    }
    I2c_DevShtc3 = nullptr;
}

etError Shtc3Port::Shtc3_GetId() {
    uint8_t senBuf[2]  = {(READ_ID >> 8), (READ_ID & 0xff)};
    uint8_t readBuf[3] = {0, 0, 0};
    int     err        = i2cbus_.i2c_master_write_read_dev(I2c_DevShtc3, senBuf, 2, readBuf, 3);
    etError error      = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetId WRITE Failure");
        return error;
    }
    error = Shtc3_CheckCrc(readBuf, 2, readBuf[2]);
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetId CRC Failure");
        return error;
    }
    shtc3_id = ((readBuf[0] << 8) | readBuf[1]);
    return error;
}

uint16_t Shtc3Port::Shtc3_GetShtc3Id() {
    return shtc3_id;
}

// wake up the sensor from sleep mode
etError Shtc3Port::Shtc3_Wakeup() {
    uint8_t senBuf[2] = {(WAKEUP >> 8), (WAKEUP & 0xff)};
    int     err       = i2cbus_.i2c_write_buff(I2c_DevShtc3, -1, senBuf, 2);
    etError error     = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    //esp_rom_delay_us(100); //100us
    vTaskDelay(kShtc3WakeupDelay);
    if (error != NO_ERROR)
        ESP_LOGE(kShtc3LogTag, "Wakeup Failure");
    return error;
}

etError Shtc3Port::Shtc3_SoftReset() {
    uint8_t senBuf[2] = {(SOFT_RESET >> 8), (SOFT_RESET & 0xff)};
    int     err       = i2cbus_.i2c_write_buff(I2c_DevShtc3, -1, senBuf, 2);
    etError error     = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    if (error != NO_ERROR)
        ESP_LOGE(kShtc3LogTag, "SoftReset Failure");
    return error;
}

etError Shtc3Port::Shtc3_CheckCrc(uint8_t data[], uint8_t nbrOfBytes, uint8_t checksum) {
    uint8_t bit;        // bit mask
    uint8_t crc = 0xFF; // calculated checksum
    uint8_t byteCtr;    // byte counter

    // calculates 8-Bit checksum with given polynomial
    for (byteCtr = 0; byteCtr < nbrOfBytes; byteCtr++) {
        crc ^= (data[byteCtr]);
        for (bit = 8; bit > 0; --bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ CRC_POLYNOMIAL;
            } else {
                crc = (crc << 1);
            }
        }
    }

    // verify checksum
    if (crc != checksum) {
        return CHECKSUM_ERROR;
    } else {
        return NO_ERROR;
    }
}

float Shtc3Port::Shtc3_CalcTemperature(uint16_t rawValue) {
    // calculate temperature [°C]
    // T = -45 + 175 * rawValue / 2^16
    return kShtc3TemperatureScaleC * (float) rawValue / kShtc3RawFullScale + kShtc3TemperatureOffsetC;
}

float Shtc3Port::Shtc3_CalcHumidity(uint16_t rawValue) {
    // calculate relative humidity [%RH]
    // RH = rawValue / 2^16 * 100
    return kShtc3HumidityScalePercent * (float) rawValue / kShtc3RawFullScale;
}

etError Shtc3Port::Shtc3_GetTempAndHumiPolling(float *temp, float *humi) {
    int      err = 0;
    etError  error;        // error code
    uint16_t rawValueTemp; // temperature raw value from sensor
    uint16_t rawValueHumi; // humidity raw value from sensor
    uint8_t  bytes[6] = {0};
    ;
    uint8_t senBuf[2] = {(MEAS_T_RH_POLLING >> 8), (MEAS_T_RH_POLLING & 0xff)};
    err               = i2cbus_.i2c_write_buff(I2c_DevShtc3, -1, senBuf, 2);
    error             = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetTempAndHumi WRITE Failure");
        return error;
    }

    vTaskDelay(kShtc3MeasureDelay);

    // if no error, read temperature and humidity raw values
    err   = i2cbus_.i2c_read_buff(I2c_DevShtc3, -1, bytes, 6);
    error = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetTempAndHumi READ Failure");
        return error;
    }
    error = Shtc3_CheckCrc(bytes, 2, bytes[2]);
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetTempAndHumi TempCRC Failure");
        return error;
    }
    error = Shtc3_CheckCrc(&bytes[3], 2, bytes[5]);
    if (error != NO_ERROR) {
        ESP_LOGE(kShtc3LogTag, "GetTempAndHumi humidityCRC Failure");
        return error;
    }
    // if no error, calculate temperature in °C and humidity in %RH
    rawValueTemp = (bytes[0] << 8) | bytes[1];
    rawValueHumi = (bytes[3] << 8) | bytes[4];
    *temp        = Shtc3_CalcTemperature(rawValueTemp);
    *humi        = Shtc3_CalcHumidity(rawValueHumi);
    return error;
}

etError Shtc3Port::Shtc3_Sleep() {
    uint8_t senBuf[2] = {(SLEEP >> 8), (SLEEP & 0xff)};
    int     err       = i2cbus_.i2c_write_buff(I2c_DevShtc3, -1, senBuf, 2);
    etError error     = (err == ESP_OK) ? NO_ERROR : ACK_ERROR;
    if (error != NO_ERROR)
        ESP_LOGE(kShtc3LogTag, "Sleep Failure");
    return error;
}

uint8_t Shtc3Port::Shtc3_ReadTempHumi(float *t,float *h) {
    if (!I2c_DevShtc3 || !t || !h) {
        return 1;
    }
    etError      error;
    Shtc3_Wakeup();
    error = Shtc3_GetTempAndHumiPolling(t, h);
    if (error != NO_ERROR) {
        ESP_LOGW(kShtc3LogTag, "error:%d", error);
    }
    Shtc3_Sleep();
    return error == NO_ERROR ? 0 : 1;
}

static i2c_master_dev_handle_t I2cRTCdev = NULL;
static I2cMasterBus           *I2cbus_   = NULL;
static bool                    s_rtc_ready = false;
SensorPCF85063 rtc;

static bool I2cDevCallback(uint8_t address, uint8_t reg, uint8_t *buf, size_t len, bool writeReg, bool isWrite) {
    if (!I2cbus_ || !I2cRTCdev || !buf || len == 0) {
        return false;
    }
    int                     ret;
    i2c_master_dev_handle_t dev_handle = NULL;
    dev_handle = I2cRTCdev;
    if (isWrite) {
        if (writeReg) {
            ret = I2cbus_->i2c_write_buff(dev_handle, reg, buf, len);
        } else {
            ret = I2cbus_->i2c_write_buff(dev_handle, -1, buf, len);
        }
    } else {
        if (writeReg) {
            ret = I2cbus_->i2c_read_buff(dev_handle, reg, buf, len);
        } else {
            ret = I2cbus_->i2c_read_buff(dev_handle, -1, buf, len);
        }
    }
    return (ret == ESP_OK) ? true : false;
}

void Rtc_Setup(I2cMasterBus *i2cbus,uint8_t dev_addr) {
    if (!i2cbus || !i2cbus->IsReady()) {
        esp_err_t err = i2cbus ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
        ESP_LOGW(kRtcLogTag, RTC_DEVICE_ADD_FAILED_FORMAT, esp_err_to_name(err));
        return;
    }
    if (I2cbus_ == NULL) {
        I2cbus_ = i2cbus;
    }
    if (I2cRTCdev == NULL) {
        i2c_master_bus_handle_t BusHandle = i2cbus->Get_I2cBusHandle();
        i2c_device_config_t     dev_cfg   = {};
        dev_cfg.dev_addr_length           = I2C_ADDR_BIT_LEN_7;
        dev_cfg.scl_speed_hz              = kRtcI2cSpeedHz;
        dev_cfg.device_address            = dev_addr;
        esp_err_t err = i2c_master_bus_add_device(BusHandle, &dev_cfg, &I2cRTCdev);
        if (err != ESP_OK) {
            ESP_LOGW(kRtcLogTag, RTC_DEVICE_ADD_FAILED_FORMAT, esp_err_to_name(err));
            return;
        }
    }
    s_rtc_ready = rtc.begin(I2cDevCallback);
    if (s_rtc_ready) {
        ESP_LOGI(kRtcLogTag, "InitWill");
    } else {
        ESP_LOGE(kRtcLogTag, "InitFailure");
    }
}

void Rtc_SetTime(uint16_t year,uint8_t month,uint8_t day,uint8_t hour,uint8_t minute,uint8_t second) {
    if (!s_rtc_ready) {
        return;
    }
    rtc.setDateTime(year, month, day, hour, minute, second);
}

void Rtc_GetTime(rtcTimeStruct_t *time) {
    if (!time) {
        return;
    }
    *time = {};
    if (!s_rtc_ready) {
        return;
    }
    RTC_DateTime  datetime = rtc.getDateTime();
    time->year              = datetime.getYear();
    time->month             = datetime.getMonth();
    time->day               = datetime.getDay();
    time->hour              = datetime.getHour();
    time->minute            = datetime.getMinute();
    time->second            = datetime.getSecond();
    time->week              = datetime.getWeek();
}
