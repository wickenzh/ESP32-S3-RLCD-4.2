# WeatherClock Detailed User Guide

[Quick guide](User.md) · [Simplified Chinese](User_Detailed_zh.md) · [Traditional Chinese](User_Detailed_zh_TW.md) · [Japanese](User_Detailed_ja.md)

This guide covers the current eight-page firmware. The device UI is primarily in Simplified Chinese; translated documentation does not add a firmware language selector. Read the release-specific upgrade notes before updating.

## 1. First-time setup

1. Power on and wait for startup. A new or factory-reset device enters setup mode.
2. Connect your phone or computer to the WeatherClock hotspot shown on the screen. Use the password displayed by the device.
3. Open the captive portal, or enter `http://192.168.4.1/` manually. Keep the connection even if your phone reports no internet; temporarily disable automatic switching to mobile data.
4. For online use, enter the primary Wi-Fi network and password, your QWeather API Key, and your account-specific API Host. A backup network and weather city are optional.
5. Submit and wait for validation. On success, the device closes setup and opens its home page. On failure, correct the indicated field rather than repeatedly resetting the device.

API Host is the domain assigned in your QWeather console. Enter the domain, not an API path, key, or retired shared hostname. If weather validation fails, check the Host, Key, service permissions, and connection together.

Saved Wi-Fi credentials alone are not a complete weather configuration. Use your own weather account and do not rely on publicly shared keys.

## 2. Buttons and status bar

| Button | Work page | Settings |
| --- | --- | --- |
| Short BOOT press | Next enabled page | Confirm or change the selected item |
| Short KEY press | Open settings | Move selection |
| Hold KEY | Follow the current screen prompt | Back one level, then back to the work page |

Settings close after about 30 seconds of inactivity. Sorting actions restart this timer. During an alarm or focus-completion sound, either button stops the sound and consumes that press.

The battery icon is an estimate. Blinking indicates inferred charging, not USB presence. The Wi-Fi icon means the radio is on, not necessarily that internet access works. The speaker and alarm icons indicate enabled reminders and a one-shot alarm.

A small header clock appears only where the main content does not already show time, including when the AI clock is occupied by a focus timer. The day-progress strip tracks the day. Small changes normally use partial redraws; page switches and large changes may require a full redraw.

## 3. Work pages

| Page | Main content | Refresh behavior |
| --- | --- | --- |
| Weather Clock | Large clock, weather, warnings, GIF, local readings | Seconds for time; weather synchronized as needed at hour boundaries |
| Picture Clock | Image, minute clock, daily text | Minute clock; configured image rotation |
| Weather Board | Current weather, forecast, air quality, wind, sunrise/sunset, advice | Shared weather cache; minute countdown to sunrise/sunset |
| Temperature & Humidity Clock | Hours/minutes/seconds, readings, trends, date | Second clock; scheduled sensor samples |
| Calendar | Month, lunar dates, holidays, today | On entry or date changes |
| Temperature & Humidity History | Last 24 hours of local readings | Hourly samples recorded in the background |
| Xiaozhi AI | Wake word, conversation, subtitles, tools | Session and timer events |
| Aggregate Clock | Second clock, today's weather, dates, local readings | Reuses the same time, weather, and sensor services |

Weather-service temperature and the onboard sensor describe different locations. Trend arrows compare rolling averages of valid samples from the last four hours; samples accumulate again after reboot.

Built-in images match the weekday and change at midnight. Uploaded galleries take priority and can rotate every 30 minutes, 1, 6, 12, or 24 hours, aligned to midnight. Daily text has its own cache.

Weather Board advice is based on today's forecast, so an umbrella recommendation can be valid even when current conditions are clear. Old cached data can remain after a failed request.

## 4. Networks, city, and offline use

Two Wi-Fi profiles are supported. After repeated failure of the preferred profile, the device tries the other; a successful backup becomes preferred. If both fail, retries are delayed. Enter setup to change credentials.

An empty city field uses automatic IP-based location, which may be inaccurate. Set a city through setup, WeatherClock Studio, or the AI weather-city tool. Use a real, unambiguous place name and check the resolved city, especially for duplicate names.

The weather-city item in Network settings can clear the manual city after confirmation and return to automatic location. Weather refresh is then requested; it can be deferred while voice or other resource-heavy work is active.

For offline setup, leave Wi-Fi blank and supply a valid local date and time. No weather Key or Host is needed. When using Wi-Fi, the offline date/time fields may remain empty.

Offline mode stops ordinary networking and prevents enabling Weather Clock, Picture Clock, Weather Board, Xiaozhi AI, and Aggregate Clock. The local clock, calendar, and history pages remain available. Leaving offline mode works directly with complete saved online configuration; otherwise the device asks for confirmation before returning to setup. Offline mode does not mean all caches are erased.

## 5. Settings

- Network: setup, time sync, weather sync, daily-text refresh, and manual-city status/reset. All weather pages share the result.
- Sound: select an available sound, volume, hourly reminders, and all-day reminders.
- Display: page toggles, page order, AI power saving, alarm status, and uploaded-image rotation.
- System: offline mode, network diagnostics, factory reset, device information, and update checks.

Only enabled pages appear in page order. Select with KEY and use BOOT to exchange positions as prompted. The first entry is home. At least one non-AI work page must remain enabled, and AI cannot be first. The history page has no special fixed five-minute return restriction.

AI power saving is enabled by default. After five idle minutes on the AI page it returns home; an active focus timer suspends that return. Built-in images stay on a fixed 24-hour rotation.

## 6. Voice, alarms, and focus timers

First use may require account activation; follow the device prompts. AI requires internet and uses substantially more power than clock pages, even while waiting for its wake word. Board heating can affect the local sensor.

Example intentions include setting a wake-up alarm, starting a 25-minute focus timer, asking for remaining focus time, cancelling focus, changing the weather city, and restoring automatic weather location. Supported spoken language depends on your Xiaozhi service configuration. Check the on-screen result rather than assuming a spoken acknowledgement proves that a tool succeeded.

Only one one-shot alarm is stored. Replacing a different existing alarm requires confirmation. It runs in the background and disables itself after firing. The sound repeats with roughly five-second gaps for up to a minute; either button stops it. Alarm and focus-completion reminders cannot target the same minute.

The focus timer defaults to 25 minutes and accepts 1 second through 99 minutes 59 seconds. Starting again replaces its duration; there is no pause. It continues across page changes but is cancelled by reboot. Below one minute it still shows minutes and seconds, not hundredths. Completion or cancellation returns the area to the normal clock after the completion indication.

A plain request to close or leave AI is not the same as cancelling an alarm or focus timer. Name the task explicitly when cancelling it. Normal AI exit may wait for the farewell to finish.

## 7. OTA and serial flashing

Open System settings, choose update checking, and press BOOT. When an update is found, confirm within 60 seconds. Download progress includes percentage, speed, and a bar. A verified update reboots automatically. Offline mode, low battery, setup, or an existing update blocks OTA.

GitHub OTA is the default primary source and Gitee OTA is the backup. Custom-server priority remains supported. A newly announced version may not be visible until building and mirror synchronization finish. Identical filenames do not imply identical hashes.

- App bin contains only the application. Never flash it at `0x0`; use the correct application partition and boot selection.
- Merged bin is a complete flash image written at `0x0`. It can overwrite settings and resources; back them up first.
- OTA cannot replace the partition table. Older v1.4.x layouts require the release-prescribed full serial upgrade to the newer layout.
- With the correct layout already installed, use a clearly documented data-preserving procedure and do not erase the whole flash.

Do not disconnect power during flashing or resource writes. Record errors before retrying instead of repeatedly erasing NVS.

## 8. WeatherClock Studio and custom assets

[Open WeatherClock Studio](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/). Desktop Chrome or Edge is recommended for serial operations. Grant serial access and close other programs using the same port.

The browser tool prepares GIFs and image galleries, previews pages, checks and flashes firmware, and reads logs. Asset conversion and configuration generation run locally; loading the site and downloading firmware still use the network. The preview is not a full hardware simulator.

Quick configuration generates a link to the device's save endpoint. Connect to the device hotspot first. Opening the link submits settings rather than merely prefilling a form. It contains plaintext credentials and must be treated like a password.

Missing or invalid resources normally fall back to built-in assets. Use tools and addresses matching the current partition layout, and keep original resource files before replacing them.

## 9. Battery and reset behavior

Normal battery and sensor sampling is about once per minute by day and every two minutes at night. Battery sampling uses the median of multiple ADC readings. A possible charging rise enters a short one-second confirmation cadence; only a stable rise changes the charging indicator.

The board does not provide an independent charging-status signal to this logic. Continuous USB connection does not prove continuous battery charging. Charge level, blinking, and full-charge time are estimates. Full-charge history requires a confirmed session that started below the full threshold, lasted at least 60 seconds, and reached that threshold. Merely stopping the animation on a plateau does not record a full charge.

Low battery activates a minimal page and pauses nonessential networking and audio. Recovery returns to the previous work page, or home if that page is unavailable.

Factory reset requires confirmation and clears Wi-Fi, weather Key/Host, manual city, offline mode, sound/page settings, and the one-shot alarm. It preserves AI binding, sensor history, and custom assets. Factory reset and whole-flash erase are different operations.

## 10. Troubleshooting and limitations

| Symptom | Check |
| --- | --- |
| Hotspot connected but no portal | Stay connected and open the local address manually |
| Weather waiting for data | Key, Host, permissions, city, network; allow delayed retries |
| Weather appears old | Cached data may remain after a failed update |
| AI unavailable or slow to prepare | Network, binding, and resource release; avoid rapid repeated entry |
| Incorrect image or temperature unit | Firmware/resource versions; capture the affected page |
| No OTA update found | Wait for source build and OTA mirrors to finish |
| Frozen display | Record time, page, and last action; USB connection may reset the device and lose evidence |
| A faint image persists after power-off | Panel image retention is possible, not proof of a missed software redraw |

Charging, AI use, and nearby heat sources may bias the onboard sensor. Keep the device ventilated and dry. Do not use its alarms or measurements as the sole basis for safety-critical decisions.

Remove credentials, tokens, location, and private addresses before sharing logs. Repository license, third-party notices, contribution guide, and security policy remain authoritative.
Time and day/night sampling use the firmware's fixed UTC+8 timezone; there is no timezone selector. Night sampling runs from 22:00 through 05:59. Lunar dates and holiday labels use the built-in Chinese calendar data, not a region-specific holiday service.
