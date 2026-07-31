// 封装 I2C 主机初始化和设备读写基础接口。
#include <array>
#include <limits>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include "i2c_bsp.h"

namespace {
constexpr int kI2cGlitchIgnoreCount = 7;
constexpr size_t kI2cRegisterPrefixLen = 1;
constexpr size_t kI2cRegisterIndex = 0;
constexpr size_t kI2cMaxRegisterWriteFrameLen =
    static_cast<size_t>(std::numeric_limits<uint8_t>::max()) + kI2cRegisterPrefixLen;
constexpr uint32_t kI2cTransferTimeoutMs = 5000;
constexpr uint32_t kI2cBusDoneTimeoutMs = 1000;
constexpr TickType_t kI2cTransferTimeoutTicks = pdMS_TO_TICKS(kI2cTransferTimeoutMs);
constexpr TickType_t kI2cBusDoneTimeoutTicks = pdMS_TO_TICKS(kI2cBusDoneTimeoutMs);
constexpr const char *kI2cLogTag = "I2cMasterBus";
#define I2C_BUS_INIT_FAILED_LOG_FORMAT "I2C master bus initialization failed: %s"
#define I2C_BUS_RELEASE_FAILED_LOG_FORMAT "I2C master bus release failed: %s"
static_assert(kI2cGlitchIgnoreCount >= 0, "I2C glitch ignore count must not be negative");
static_assert(kI2cRegisterPrefixLen == 1, "I2C register writes prepend one register byte");
static_assert(kI2cRegisterIndex == 0, "I2C register prefix must start at byte 0");
static_assert(kI2cMaxRegisterWriteFrameLen == 256,
              "I2C register write frame must cover every uint8_t payload length");
static_assert(kI2cTransferTimeoutMs > 0, "I2C transfer timeout must be positive");
static_assert(kI2cBusDoneTimeoutMs > 0, "I2C bus done timeout must be positive");
static_assert(kI2cTransferTimeoutTicks > 0, "I2C transfer tick timeout must be positive");
static_assert(kI2cBusDoneTimeoutTicks > 0, "I2C bus done tick timeout must be positive");
static_assert(kI2cLogTag[0] != '\0', "I2C log tag must not be empty");
} // namespace

I2cMasterBus::I2cMasterBus(int scl_pin,int sda_pin,int i2c_port) {
    i2c_master_bus_config_t i2c_bus_config      = {};
    i2c_bus_config.clk_source                   = I2C_CLK_SRC_DEFAULT;
    i2c_bus_config.i2c_port                     = (i2c_port_t)i2c_port;
    i2c_bus_config.scl_io_num                   = (gpio_num_t)scl_pin;
    i2c_bus_config.sda_io_num                   = (gpio_num_t)sda_pin;
    i2c_bus_config.glitch_ignore_cnt            = kI2cGlitchIgnoreCount;
    i2c_bus_config.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&i2c_bus_config, &user_i2c_handle);
    if (err != ESP_OK) {
        user_i2c_handle = NULL;
        ESP_LOGE(kI2cLogTag, I2C_BUS_INIT_FAILED_LOG_FORMAT, esp_err_to_name(err));
    }
}

I2cMasterBus::~I2cMasterBus() {
    if (!user_i2c_handle) {
        return;
    }
    esp_err_t err = i2c_del_master_bus(user_i2c_handle);
    if (err != ESP_OK) {
        ESP_LOGW(kI2cLogTag, I2C_BUS_RELEASE_FAILED_LOG_FORMAT, esp_err_to_name(err));
        return;
    }
    user_i2c_handle = NULL;
}

bool I2cMasterBus::IsReady() const {
    return user_i2c_handle != NULL;
}

int I2cMasterBus::i2c_write_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len) {
    if (!IsReady()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dev_handle || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int  ret;
    ret           = i2c_master_bus_wait_all_done(user_i2c_handle, kI2cBusDoneTimeoutTicks);
    if (ret != ESP_OK)
        return ret;
    if (reg == -1) {
        ret = i2c_master_transmit(dev_handle, buf, len, kI2cTransferTimeoutTicks);
    } else {
        std::array<uint8_t, kI2cMaxRegisterWriteFrameLen> write_frame;
        const size_t write_len = static_cast<size_t>(len) + kI2cRegisterPrefixLen;
        write_frame[kI2cRegisterIndex] = static_cast<uint8_t>(reg);
        memcpy(write_frame.data() + kI2cRegisterPrefixLen, buf, len);
        ret = i2c_master_transmit(
            dev_handle, write_frame.data(), write_len, kI2cTransferTimeoutTicks);
    }
    return ret;
}

int I2cMasterBus::i2c_master_write_read_dev(i2c_master_dev_handle_t dev_handle, uint8_t *writeBuf, uint8_t writeLen, uint8_t *readBuf, uint8_t readLen) {
    if (!IsReady()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dev_handle || !writeBuf || writeLen == 0 || !readBuf || readLen == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int ret;
    ret = i2c_master_bus_wait_all_done(user_i2c_handle, kI2cBusDoneTimeoutTicks);
    if (ret != ESP_OK)
        return ret;
    ret = i2c_master_transmit_receive(dev_handle, writeBuf, writeLen, readBuf, readLen, kI2cTransferTimeoutTicks);
    return ret;
}

int I2cMasterBus::i2c_read_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len) {
    if (!IsReady()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!dev_handle || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int ret;
    uint8_t addr = 0;
    ret          = i2c_master_bus_wait_all_done(user_i2c_handle, kI2cBusDoneTimeoutTicks);
    if (ret != ESP_OK)
        return ret;
    if (reg == -1) {
        ret = i2c_master_receive(dev_handle, buf, len, kI2cTransferTimeoutTicks);
    } else {
        addr = (uint8_t) reg;
        ret  = i2c_master_transmit_receive(dev_handle, &addr, 1, buf, len, kI2cTransferTimeoutTicks);
    }
    return ret;
}

i2c_master_bus_handle_t I2cMasterBus::Get_I2cBusHandle() const {
    return user_i2c_handle;
}
