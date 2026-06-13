# ESP32-S3 RLCD 天气时钟

这是一个基于 Waveshare ESP32-S3-RLCD-4.2 开发板的低功耗天气时钟项目。当前主固件工程仍保留在 `RLCD_CLOCK/` 目录中，仓库根目录用于放置项目说明、硬件资料、示例代码和后续会用到的资源文件。

## 硬件与技术栈

- 主控：ESP32-S3，16 MB Flash，带 PSRAM。
- 屏幕：4.2 英寸 RLCD 反射式 LCD。
- UI：LVGL 8，支持 `RLCD_CLOCK/simulator/` 下的 SDL 本地预览。
- RTC：PCF85063，通过 I2C 通信。
- 本地传感器：SHTC3，通过 I2C 读取温度和湿度。
- 电池检测：ADC1 Channel 3 采样，按 3 倍分压换算电池电压。
- 天气服务：和风天气 QWeather，使用 API Key 认证，请求头为 `X-QW-Api-Key`。
- 时间同步：NTP，服务器包括 `pool.ntp.org`、`ntp.aliyun.com`、`time.windows.com`。

## 固件功能逻辑

- 开机后显示启动页，包含项目名称、版本号、启动状态和进度条。
- 如果已经保存 WiFi 信息，开机后会连接 WiFi，并进行一次 NTP 时间同步和天气同步。
- 如果没有 WiFi 信息，设备会进入配网模式，AP 名称格式为 `WeatherClock-XXXX`。
- 配网方式：连接设备 AP 后打开 `192.168.4.1`，输入 WiFi 名称、WiFi 密码和 QWeather API Key。
- NTP 同步规则：开机同步一次，之后每天 0 点同步一次。
- 天气同步规则：开机或首次配网成功后同步一次，之后每小时同步一次。
- 本地温湿度读取规则：每分钟读取一次。
- 电池读取规则：每 5 分钟读取一次电池百分比和电池电压。
- 低功耗策略：非联网同步时间窗口会主动关闭 WiFi；CPU 降频并启用 light sleep，同时保持 RLCD 屏幕显示。

## 主界面显示

- 时间使用断码屏风格的大号数字显示，小时和分钟最大，秒显示较小。
- 日期显示在右上角，格式为 `YYYY/MM/DD / 星期几`。
- 电池图标显示在左上角，内部 5 个小格，每格代表约 20% 电量。
- 城市、网络天气、本地温度和本地湿度显示在主界面下方区域。
- 正常工作界面不显示 `WiFi OFF`、`AP OFF`、`NTP OK` 等状态文字，避免干扰主要信息。
- 中文 UI 使用生成的 LVGL 字体，当前字体来源为 `Hiragino Sans GB W6 Bold`，尺寸为 16 px，1 bpp，适配单色 RLCD 显示链路。
- 天气图标使用 QWeather 图标字体转换后的 LVGL 字体。

## Boot 键信息页

BOOT 键用于查看系统信息和进入配网模式：

- 按住不足 5 秒：界面不变化。
- 按住 5 到 19 秒：显示英文信息页。
- 未满 20 秒松手：返回正常时钟主界面。
- 按住 20 秒以上：进入配网模式。

信息页只使用英文和数字，显示内容包括：

- 上一次 NTP 同步时间。
- 当前保存的 WiFi SSID。
- 上一次天气 API 查询时间。
- 电池百分比和电池电压。
- 当前软件版本号。

## 仓库目录

- `RLCD_CLOCK/`：当前主 ESP-IDF 固件工程和 SDL 预览工程。
- `docs/`：原理图、芯片手册、传感器资料和其他硬件文档。
- `examples/esp-idf/ESP32-S3-RLCD-4.2-Demo/`：厂商 ESP-IDF 参考例程。
- `assets/weather-icons/QWeather-Icons-1.8.0/`：和风天气图标 SVG、字体和源文件。
- `assets/gif_video/`：后续 UI 或媒体功能可能会使用的 GIF 和音频资源。
- `README.md`：项目说明、使用方式、功能逻辑和版本记录。

## 编译与烧录

编译固件：

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && . /Users/zhwickner/esp/esp-idf/export.sh && idf.py build
```

编译 SDL 预览：

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && cmake --build simulator/build
```

烧录并打开串口监视器，请根据实际串口替换 `-p` 后面的端口：

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && . /Users/zhwickner/esp/esp-idf/export.sh && export PATH="/Users/zhwickner/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin:$PATH" && idf.py -p /dev/cu.usbmodem2020_12_222 flash monitor
```

只打开串口监视器：

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && . /Users/zhwickner/esp/esp-idf/export.sh && export PATH="/Users/zhwickner/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin:$PATH" && idf.py -p /dev/cu.usbmodem2020_12_222 monitor
```

## 版本记录

- `v0.0.37`：将开机页原进度条替换为 GIF 抽帧动画，动画按启动百分比播放一个完整周期，并上移标题、状态和版本文字以避免遮挡。
- `v0.0.36`：调整主界面布局，电池图标移到左上角，日期和星期合并到右上角，移除底部 WiFi/AP/NTP 状态文字，并修复 Boot 信息页返回主界面后电池图标偶发空白的问题。
- `v0.0.35`：新增 BOOT 键信息页，记录上一次 NTP/天气同步时间和电池电压，整理仓库资料、示例和资源目录，并新增 README。
- `v0.0.34`：恢复中文字体可见性，将中文 LVGL 字体切回 1 bpp，同时保留较粗的字体来源。
- `v0.0.33`：天气城市显示改为 IP 定位得到的城市，避免显示更细的区县名称。
- `v0.0.32`：将天气响应缓冲区移出同步任务栈，并增大网络同步任务栈。
- `v0.0.31`：支持处理 gzip 压缩的天气 API 响应。
- `v0.0.30`：修复电池电量读取，改为使用 ADC 电压采样计算。
