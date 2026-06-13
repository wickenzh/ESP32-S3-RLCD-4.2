# ESP32-S3 RLCD Weather Clock

ESP32-S3 RLCD Weather Clock is a low-power weather clock demo for the Waveshare ESP32-S3-RLCD-4.2 board. The current main firmware project is kept in `RLCD_CLOCK/` during the transition period. Repository root now holds project documentation, reference examples, and reusable media assets.

## Hardware And Stack

- MCU: ESP32-S3 with 16 MB Flash and PSRAM.
- Display: 4.2 inch reflective LCD driven by the board BSP.
- UI: LVGL 8 with a local SDL preview under `RLCD_CLOCK/simulator/`.
- RTC: PCF85063 over I2C.
- Local sensor: SHTC3 temperature and humidity over I2C.
- Battery: ADC1 channel 3 voltage sampling, 3x divider compensation, linear 3.00 V to 4.12 V percentage mapping.
- Weather: QWeather API Key authentication using `X-QW-Api-Key`.
- Time: NTP synchronization through `pool.ntp.org`, `ntp.aliyun.com`, and `time.windows.com`.

## Firmware Behavior

- On startup, the boot page shows the project name, version, progress, Wi-Fi/NTP status, and then enters the main clock page.
- If Wi-Fi credentials are configured, the device connects at startup, performs NTP synchronization, queues weather sync, and then turns Wi-Fi off when not needed.
- If Wi-Fi credentials are missing, the device starts setup AP mode. The AP SSID is generated from the device MAC as `WeatherClock-XXXX`.
- Setup portal: connect to the AP and open `192.168.4.1`; enter Wi-Fi SSID, Wi-Fi password, and QWeather API Key.
- NTP sync policy: once at boot, then once per day at midnight.
- Weather sync policy: once at boot or after provisioning, then once per hour.
- Sensor policy: local temperature and humidity are read once per minute.
- Battery policy: battery percentage and voltage are read once every five minutes.
- Power policy: Wi-Fi is turned off outside required network sync windows; CPU frequency and light sleep are enabled while keeping RLCD pins active.

## Boot Button Info Page

The BOOT key has a long-hold diagnostic flow:

- Hold for less than 5 seconds: no page change.
- Hold for 5 to 19 seconds: show an English-only system information page.
- Release before 20 seconds: return to the normal clock page.
- Hold for at least 20 seconds: enter setup AP mode.

The information page displays:

- Last NTP synchronization time.
- Connected Wi-Fi SSID.
- Last weather API query time.
- Battery percentage and voltage.
- Current software version.

## UI Notes

- The main clock uses LVGL.
- Hour and minute digits use a DSEG-style bitmap font inspired by the reference RLCD project.
- Seconds are displayed smaller than hours and minutes.
- Chinese UI text uses a generated LVGL font. The current font is `Hiragino Sans GB W6 Bold`, generated as 16 px, 1 bpp to keep it visible on the monochrome RLCD pipeline.
- Weather icons use the QWeather icon font converted into an LVGL font.

## Repository Layout

- `RLCD_CLOCK/`: current main ESP-IDF firmware project and SDL preview.
- `docs/`: hardware schematics, datasheets, and technical reference documents.
- `examples/esp-idf/ESP32-S3-RLCD-4.2-Demo/`: vendor ESP-IDF reference examples.
- `assets/weather-icons/QWeather-Icons-1.8.0/`: source QWeather icon assets and fonts.
- `assets/gif_video/`: GIF and audio assets reserved for later UI or media features.
- `README.md`: project overview, build instructions, and version notes.

## Build And Flash

Build firmware:

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && . /Users/zhwickner/esp/esp-idf/export.sh && idf.py build
```

Build SDL preview:

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && cmake --build simulator/build
```

Flash and monitor, replacing the port if needed:

```bash
cd "/Users/zhwickner/Documents/Codex/2026-05-30/ESP32-S3-RLCD-4.2/RLCD_CLOCK" && . /Users/zhwickner/esp/esp-idf/export.sh && export PATH="/Users/zhwickner/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin:$PATH" && idf.py -p /dev/cu.usbmodem21201 flash monitor
```

## Version Notes

- `v0.0.35`: Adds BOOT-key information page, records last NTP/weather sync times and battery voltage, organizes repository assets/docs/examples, and adds this README.
- `v0.0.34`: Restores visible Chinese rendering by switching the generated Chinese LVGL font back to 1 bpp while keeping a heavier font source.
- `v0.0.33`: Displays the IP geolocation city instead of the more specific QWeather district name.
- `v0.0.32`: Moves weather response buffers off the sync task stack and increases network sync task stack size.
- `v0.0.31`: Handles gzip-compressed weather API responses.
- `v0.0.30`: Fixes battery level reading using ADC voltage sampling.
