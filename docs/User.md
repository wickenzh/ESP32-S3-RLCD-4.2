# WeatherClock User Guide

This guide explains first-time setup, the seven work pages, hardware keys, online/offline operation, reminders, Xiaozhi AI, OTA updates, DIY assets, and troubleshooting.

> **Important upgrade notice:** `v1.5.x` uses a new flash partition table. A device still running `v1.4.59` or earlier must be fully flashed with the merged image or complete flash package before using `v1.5.x`. An App-only flash or ordinary OTA cannot update the partition table.

## 1. Quick Start

1. Power on the device and wait for the startup animation to finish.
2. On first use, connect a phone or computer to the AP whose name starts with `WeatherClock-`.
3. The captive portal normally opens automatically. If it does not, browse to `http://192.168.4.1/`.
4. Choose an operating mode:
   - Online: enter Wi-Fi SSID, password, and QWeather API Key.
   - Offline: leave Wi-Fi empty and enter the offline date and time.
5. After setup, the clock opens the first enabled page in the saved page order.

Hardware key behavior:

- **BOOT short press:** move to the next enabled work page; confirm in Settings.
- **KEY short press:** open Settings; move the selection inside Settings.
- **KEY long press:** return from a secondary menu; exit Settings from the primary menu.
- **Either key during an alarm or Pomodoro completion sound:** stop the remaining sound and consume that key press.

Button wakeups, alarms, Pomodoro events, and UI refreshes use thread-safe task notification internally. Interaction and response timing remain unchanged, while cross-core notification no longer relies on an unsynchronized task handle.

Settings returns to the current work page after about 30 seconds without activity. Valid operations in the page-order screen restart this timeout.

On low-refresh pages such as Gallery Clock, Weather Board, Calendar, and Temperature/Humidity History, the device waits for a key wakeup while idle to reduce power use. BOOT/KEY short-press and long-press behavior is unchanged.

## 2. Status Bar and Day Progress

Depending on the page, the status area shows date, weekday, battery, Wi-Fi, reminder, alarm, and local sensor information.

- **Wi-Fi icon:** shown whenever the Wi-Fi radio is on and hidden when it is off, including connection and synchronization periods.
- If the device cannot temporarily reserve the wake resource required for networking, that operation fails safely or retries later without forcing Wi-Fi on. Wait briefly before trying again.
- **Sound icon:** shown when an hourly or all-day reminder is enabled.
- **Alarm icon:** shown while the one-shot alarm is enabled.
- **Battery icon:** shows estimated charge. During detected charging it blinks on whole-second boundaries. The hardware has no dedicated CHG/VBUS input, so plug/unplug detection based on ADC voltage trends can be delayed briefly.
- **Day-progress strip:** shared by all seven pages. Its 60 segments each represent about 24 minutes and refresh only on page entry or when crossing a new segment.

The display favors partial refreshes. Second-level pages redraw only changing digits or small regions; low-frequency pages wait until the next minute, date, sensor, or network-data change before waking for an update.

## 3. Seven Work Pages

Press **BOOT** to follow the saved page order. Disabled pages are skipped. Every page can be disabled, but the device prevents disabling the final enabled page.

### 3.1 Weather Clock

Shows large time, date, battery, current weather, alerts, local temperature/humidity, trend arrows, status icons, and an animation area.

- Time and required animation regions use second-level partial refresh.
- Date, minute time, seconds, animation, and second progress use their own change-driven partial refresh rates; hourly chimes continue to follow the saved settings.
- Entering the page requests weather only when required data is missing or stale.
- Offline, low-battery, setup, and OTA states block ordinary weather requests. Low-battery and setup screens may reuse the Weather Clock layout, but that display fallback does not turn on networking.

### 3.2 Picture Clock

Shows a local image, large time, daily text, and local temperature/humidity summary.

- Built-in images are selected by weekday and change once per day.
- When a desktop-client gallery is installed, custom images take priority and rotate daily by date.
- Daily text is fetched only when the Picture Clock needs it and is not repeatedly downloaded after a successful update for the same day.

### 3.3 Weather Board

Shows city, current temperature and condition, air quality, humidity, wind, sunrise/sunset, time until the next sunrise or sunset, weather alerts, and a six-day forecast.

- Missing or stale data causes a full weather refresh when entering the page.
- Sunrise/sunset countdown updates once per minute.
- Local sensor and status text refresh only when their values change.

### 3.4 Temperature/Humidity Clock

Shows three high-contrast hour/minute/second cards plus local temperature, humidity, trend arrows, comfort faces, date, and lunar date.

- Seconds refresh locally once per second.
- Local temperature/humidity is sampled every minute during the day and every two minutes at night.
- Trend arrows use the rolling average of valid samples from the latest four-hour window and restart after reboot.
- Internal page construction and runtime refresh are isolated for stability; this does not change the display, refresh frequency, controls, or stored data.

### 3.5 Calendar

Shows the current month, weekday bar, lunar text, and holidays.

- The calendar body redraws only on page entry, date/month change, or a time correction that crosses a day boundary.
- For rare six-row months, once today reaches the sixth row the already-passed first row is hidden so today remains visible.

### 3.6 Temperature/Humidity History

Shows rolling 24-hour local temperature and humidity curves. Background sensing and hourly history recording continue regardless of the visible page.

- This page can be enabled, disabled, and reordered like other pages.
- If it is the only enabled page, the device remains on it and does not create an auto-return conflict.

### 3.7 Xiaozhi AI

Provides local wake-word detection, voice conversations, on-screen transcripts, response playback, one-shot alarms, Pomodoro timers, and weather-city configuration.

- High-power voice services start only while entering the Xiaozhi page.
- While waiting for cloud reply audio, an empty playback queue sleeps until new data arrives instead of polling every few milliseconds. Voice content, initial buffering, and playback order are unchanged.
- First use may require binding to the Xiaozhi service by following the on-screen prompt.
- A bound device restores its saved service configuration directly. An unbound device still displays and announces the binding ID first. It is marked announced only after the prompt and all digits finish; if audio is temporarily busy or the task cannot start, a later activation cycle retries without requiring page switching or a reboot.
- Speak the wake word while waiting. If the page says Xiaozhi is preparing, allow service initialization to finish.
- If the cloud service recognizes only an incomplete phrase and returns no text or audio reply, the page shows that it did not hear the complete request. Continue or repeat the request; about 12 seconds of silence returns to wake-word standby.
- User transcripts, assistant replies, emotions, and playback completion use one conversation-state path so multi-turn listening, farewell return, and minimum transcript visibility remain consistent.
- Service handshakes, ordinary replies, and MCP tool messages share a bounded session buffer. An invalid oversized text frame ends the current session instead of overwriting adjacent memory.
- Xiaozhi validates audio lengths before sending, decoding, sample-rate conversion, and playback queueing. An invalid frame ends only the current conversation instead of reading beyond its buffer.
- Page status, subtitles, and emotion refresh only when their content changes. Offline, unconfigured, or retry states do not repeatedly redraw the same message.
- While offline mode is enabled or Wi-Fi has not been saved, Xiaozhi waits for a configuration change instead of periodically starting network work. Saving setup, changing offline mode, or leaving the page wakes it immediately.
- Binding or service connection failures retry automatically at about 15-second intervals. Leaving the page, alarms, and Pomodoro events remain immediately responsive during this wait.
- Leaving the page or reaching a failed connection retry stops the ordinary voice session and releases its page-owned network/power resources. Alarm and an active Pomodoro keep running in the background.
- This page consumes substantially more power and warms the PCB, which may make the onboard temperature/humidity reading higher than the surrounding air.

## 4. Setup Portal

### 4.1 Entering Setup

With no saved online configuration and offline mode disabled, setup starts automatically after boot.

1. Join `WeatherClock-xxxx`.
2. Wait for the captive portal or open `http://192.168.4.1/`.
3. Fill the required fields:
   - **Wi-Fi SSID:** select from the scan list or enter manually.
   - **Wi-Fi password:** password for the selected network.
   - **QWeather API Key:** required for current weather, alerts, forecast, and air quality.
   - **Weather city (optional):** for example Hangzhou. Chinese names ending in `市` are normalized and validated through QWeather. Leave empty for public-IP location.
   - **Offline date and time:** use only when Wi-Fi is intentionally left empty.

In the rare case that the platform cannot configure the captive DNS receive timeout, the page may not open automatically. Open the address above manually; the failed DNS service exits cleanly instead of remaining active after setup.

### 4.2 Online Mode

Submitting Wi-Fi credentials stores the online configuration and starts connection. QWeather API Key is required only for weather services; NTP and daily text do not use it. If the short boot request obtains current conditions but not forecast or air quality, a staggered background refresh remains scheduled so extended weather-board data is normally ready before first entry.

The device publishes the Wi-Fi name, password, and weather API key to background tasks as one complete configuration. Saving cannot mix old and new credential fields, and serial logs do not print the password or full API key. Setup steps and field formats are unchanged.

After setup, NTP, weather, and daily-text requests are staggered to avoid concurrent HTTPS memory peaks. Enabled network pages receive an initial data prefetch even if they are not the first visible page.

The clock synchronizes time once at startup and then at local midnight each day. A failed midnight synchronization remains pending and retries after the normal delay, so crossing past 00:00 does not discard the daily update.

A manual time synchronization is a single user-requested attempt. If it fails, the settings page reports the result without scheduling an otherwise unused background wake-up; startup and midnight synchronization retain their automatic retry behavior.

### 4.3 Manual Weather City

A manual city takes priority over IP location and can be set through:

- The setup portal.
- Desktop-client resource configuration.
- Xiaozhi voice, for example “set the weather city to Hangzhou.”

**Settings > Network > Weather City** displays Auto or the saved city. In manual mode, press BOOT and confirm again to clear it and return to IP location. Clearing immediately queues a weather refresh.

An unrecognized QWeather city is rejected. If online validation times out, the normalized name is kept and retried during the next weather update.

The city is handed between setup, Xiaozhi, Settings, and weather synchronization as one complete text snapshot. Updating a Chinese city while a sync is running cannot expose a partial or mixed city name.

### 4.4 Offline Mode

Leave Wi-Fi empty and enter a valid date/time to write the RTC and enter offline mode.

While offline:

- Wi-Fi and normal network services are stopped.
- NTP, weather, alerts, daily text, diagnostics, and OTA are unavailable.
- Weather Clock, Picture Clock, Weather Board, and Xiaozhi AI are disabled and cannot be re-enabled until online mode returns.
- Calendar, Temperature/Humidity Clock, and History remain available.
- Cached data is not actively deleted.

To leave offline mode:

- With saved Wi-Fi and QWeather API Key, turn it off directly.
- If either is missing, confirm twice to enter setup and finish online configuration.

## 5. Settings

Press **KEY** to enter Settings. The left column is the primary menu; the right side contains secondary items. The selected item is shown with inverted colors.

Settings navigation and confirmation state are handed off safely between input, UI, and OTA tasks. Rapid key use, returning from About Device or Network Diagnostics, and keeping the update panel visible no longer reuse stale focus or confirmation state. Key behavior and the 30-second inactivity timeout are unchanged.

### 5.1 Network

- **Sync Time:** run NTP now.
- **Sync Weather:** update current weather, alerts, forecast, and air quality, refreshing the cache shared by Weather Clock and Weather Board.
- **Update Daily Text:** refresh the Picture Clock text.
- **Weather City:** inspect automatic/manual mode or clear a manual city with confirmation.

Each network window reports the requests captured when it started. A new request raised while that window is running is preserved for the next window instead of being cleared by the earlier result.

### 5.2 Sound

- **Volume:** cycle through 20%, 40%, 60%, 80%, and 100% with preview playback.
- **Sound Select:** cycle through available reminder sounds with preview playback.
- **Hourly Reminder 7:00–22:00:** chime only in the daytime window.
- **All-day Reminder 0:00–24:00:** higher priority; chime every hour all day.

When both reminder switches are off, no hourly sound plays. Critical Xiaozhi audio, OTA, and other protected operations avoid concurrent Codec use.
If the device cannot temporarily reserve the wake and clock resources required for playback, that sound is skipped safely and its playback state is released. Later reminders and previews can try again normally.
When audio is busy, rapid repeated sound-setting changes keep only one pending preview. Once playback becomes available, the preview uses the latest selection instead of replaying every intermediate choice.
No vendor audio demo task runs in the background. Audio resources are opened only for actual chimes, previews, alarms, Pomodoro completion, or Xiaozhi sessions and are released through the shared lifecycle afterward.
Codec and sensor I2C register writes no longer allocate internal heap memory on every call, reducing transient fragmentation during repeated previews or Xiaozhi startup. Audio, sensor, and page controls are unchanged.
Xiaozhi shutdown diagnostics now use a separately synchronized audio-resource state instead of reading a Codec pointer while another task may create or release it. This improves diagnostic stability without changing sound controls or use.
The audio layer validates speaker and microphone readiness separately. A partial Codec startup cannot publish an unusable audio resource or pass a missing microphone handle to the driver; the current attempt ends safely and a later reminder or Xiaozhi entry may retry.
Production audio now reuses the ES8311/ES7210 I2C controls already owned by the board Codec layer instead of registering duplicate, unused device handles for every session. Sound behavior and hardware addresses are unchanged.
The network transaction lock shared by weather, daily text, Xiaozhi, and OTA is now a static lifetime resource. This removes a cold-start heap allocation and a source of long-lived fragmentation without changing request order, timeouts, or user operation.
The depth counter mutex used by network and audio power locks is also a static lifetime resource. This reduces startup heap allocation and long-lived fragmentation without changing light sleep, network, or audio behavior.
The application event group shared by provisioning, synchronization, OTA, and startup is also a static lifetime resource. This removes another cold-start allocation without changing event delivery, wait timeouts, or user operation.
RTC and SHTC3 setup also rejects an unavailable shared I2C bus, and owned sensor handles are released when their object is destroyed. Sensor addresses, intervals, and displayed readings are unchanged.

### 5.3 Display

- **Page switches:** enable or disable pages. Network pages are blocked while offline, and the final enabled page cannot be disabled. Changes are handed safely to page switching and background network decisions without changing display or sync rules.
- **Page order:** lists only enabled pages. KEY selects; BOOT exchanges the selected page with the next one. The first item becomes the boot home page. The firmware reads and saves the complete order as one snapshot, so rapid sorting or page switching cannot observe a half-finished exchange.
- **Alarm:** displays the one-shot alarm and allows manual disable when active.
- **Xiaozhi AI auto return:** after five minutes without valid activity, return from Xiaozhi to the first page. Auto-return pauses while a Pomodoro runs.

Xiaozhi AI cannot be the first page because its high-power service is unsuitable as the boot home page. At least one non-Xiaozhi page must remain enabled.

### 5.4 System

- **Offline Mode**
- **Network Diagnostics**: checks local IP, public IP, IP location, DNS, QWeather, NTP, Daily Saying, internet access, and the OTA manifest. The local IP is updated as one complete address during connection, lease renewal, or disconnection, so a partial address is never shown.
- **Factory Reset** (requires confirmation)
- **About Device** (version, battery, voltage, last-charge time, device information, and source repository)
- **Check Update**

## 6. Alarm and Pomodoro

### 6.1 One-shot Alarm

The single alarm can be set, changed, or disabled through Xiaozhi voice.

- Example: “Wake me tomorrow at 6:30.”
- Replacing an existing different alarm requires explicit confirmation.
- Alarm and Pomodoro completion cannot target the same local minute; the later request is rejected.
- The enabled alarm keeps running in the background while the device sleeps between minute boundaries. NTP or manually corrected time automatically reschedules it.
- The alarm repeats after about five seconds for up to one minute.
- Either hardware key stops it.
- It disables itself after ringing.

### 6.2 Pomodoro

- Say “start a 25-minute Pomodoro” or “focus for 45 minutes.”
- Default is 25 minutes; valid range is 1 second to 99 minutes 59 seconds. Starting again changes the active duration.
- Ask for remaining time, or say “cancel the Pomodoro” / “end focus.”
- If the Pomodoro finishes while Xiaozhi is visible, voice listening pauses only for the completion sound. The service is not restarted while paused, and wake-word standby resumes directly afterward.
- Ordinary “remind me in 10 minutes” requests remain alarm requests.
- It continues after changing pages but is cleared by reboot.
- In the final minute, the minute card shows `00` and the right card shows whole remaining seconds; hundredths are intentionally not displayed.
- Completion shows a completed state and plays two prompts. Either key stops playback.
- Saying only “close” or “stop” exits the Xiaozhi conversation and does not cancel a background Pomodoro.

## 7. OTA and Flashing

For each public source release, GitHub Actions automatically attaches two build outputs to the matching GitHub Release: `weather_clock_vX.X.X.bin` for OTA or app-partition flashing, and `weather_clock_vX.X.X_merged.bin` for a complete flash from address `0x0`. The automated build only adds firmware assets and does not rewrite the existing release notes. Release notes use a numbered list to summarize all user-visible fixes and maintenance changes since the previous formal release.

After the GitHub build completes, the Cloudflare OTA service imports and verifies both files automatically. If the automatic notification is delayed, the maintenance release flow requests a protected retry. The previous online manifest remains active until both new firmware images pass validation.

Internally, provisioning, offline mode, chime, volume, and Xiaozhi auto-return settings are safely published to background tasks. This maintenance does not change where settings are edited, how they are saved, or how they are restored after restart.

The online manifest may include release notes for publishing tools and the desktop client. The device retains only the version, download URL, file size, and SHA256 metadata required for installation instead of keeping unused release-note text in memory.

Open **Settings > System > Check Update**:

1. Press BOOT to check.
2. When a new version is available, press BOOT again within 60 seconds.
3. Download percentage, speed, and progress bar remain visible.
4. After validation, the device shows a reboot notice before restarting.

The firmware follows OTA download redirects and closes the current HTTP connection on failure or early exit. A failed download does not switch the boot partition and can be retried.

OTA uses a primary remote source and a scheduled backup source. The backup may lag behind shortly after a release.

OTA is blocked during offline mode, low-battery mode, setup, or another active OTA flow.

While an OTA check or download is active, ordinary background network synchronization sleeps until the OTA state changes. Download progress continues to update normally without repeatedly waking unrelated weather, time, or daily-text work.

### 7.1 App bin vs. merged bin

- **App bin:** updates an existing valid OTA App partition and can preserve NVS, assets, and sensor history. Never write an App bin at `0x0`; use the correct current App partition address.
- **Merged bin:** written from `0x0` and includes bootloader, partition table, OTA data, and App. Use for first install or partition-table migration.

`v1.5.x` has a different partition layout from `v1.4.x`. Fully flash once before using normal OTA updates on that device.

## 8. Desktop-client DIY Assets

The dedicated `assets` partition supports desktop-client uploads for:

- The Weather Clock GIF region as fixed-size 1-bit animation frames.
- Picture Clock gallery images as fixed-size 1-bit bitmaps.
- Optional resource-package weather city and custom OTA endpoint settings.

The firmware validates package header, dimensions, offsets, lengths, and CRC. Missing or invalid custom data falls back to built-in assets.

Because `v1.5.0` moved partition addresses, an old desktop client must be updated to use the current partition table before writing resources.

## 9. Battery and Low-power Behavior

- Battery is sampled immediately after boot.
- When not charging, battery ADC follows the same schedule as local temperature/humidity: every minute during the day and every two minutes at night.
- Temperature and humidity samples notify the active page for an on-demand update. Stable text is not redrawn repeatedly; a roughly one-minute fallback check remains for resilience.
- During confirmed active charging, battery sampling increases to about once per second.
- Low battery enters a minimal page and stops non-essential networking, animation, audio, and high-frequency refresh.
- Charging state and percentage are estimates derived from voltage trends and are not precision battery instrumentation.

## 10. Factory Reset and Retained Data

Factory reset clears:

- Wi-Fi SSID/password.
- QWeather API Key and manual city.
- Offline mode.
- Sound, volume, and reminder settings.
- Page switches/order and Xiaozhi auto-return.
- One-shot alarm.

Factory reset retains:

- Existing Xiaozhi activation/binding data.
- Stored sensor history.
- Desktop-client DIY assets.

The device then enters setup as an unconfigured clock.

## 11. Troubleshooting

### Setup portal does not open

Stay connected to the `WeatherClock-` AP, disable automatic mobile-data switching, and browse to `http://192.168.4.1/`.

If portal startup fails, the device removes the incomplete AP and restores its previous Wi-Fi mode instead of leaving an unusable high-power hotspot active. Retry setup after a short wait.

### Weather remains “Waiting for data”

Verify Wi-Fi, QWeather API Key, and city. Run **Network > Sync Weather** or use **System > Network Diagnostics** to inspect QWeather, DNS, and Internet access.

### Manual city does not work

Use a common city name. Chinese `杭州市` is normalized to `杭州`. Ambiguous names may return QWeather's first matching city; restore automatic mode or use a more specific accepted name.

### Custom image or GIF is ignored

Verify the current partition table, WCA1 package format, required dimensions, and CRC. Invalid custom resources are safely ignored.

### Xiaozhi remains unavailable or preparing

Allow Wi-Fi, model, and Codec initialization to retry. If the main task cannot start because resources are temporarily unavailable, it retries about every five seconds without requiring page switching. If it does not recover, leave the page and enter it again, then inspect serial logs for network, model, or audio errors.
After Wi-Fi startup or connection timeout, the device retries in about 15 seconds. During that wait it releases the failed session's network and voice resources instead of remaining at conversation power.
An occasional microphone read error is retried briefly. If reads remain unavailable for about one second, the device exits that failing capture loop and automatically rebuilds voice listening instead of staying in a high-frequency error state.
When an abnormal listener is stopped, its capture buffer is also released centrally, so repeated automatic recovery does not keep consuming additional PSRAM.
If temporary memory pressure prevents a Xiaozhi tool response from being built, the device discards that response and releases its temporary data safely. Retry the request after the service recovers; a reboot is not required.
If a speaker write fails, the current voice session ends immediately, queued audio is released, and listening is rebuilt automatically instead of remaining in the speaking state until timeout.
After Xiaozhi changes the weather city, the device releases real-time voice resources before refreshing all weather data. A network fault cannot leave Xiaozhi waiting forever; the queued refresh remains owned by the background network task.

### A just-published OTA version is not found

Check access to the primary manifest. The GitHub backup is synchronized on a schedule and may temporarily remain on the previous version.

If the serial log shows both `OTA manifest source skipped: R2` and `OTA manifest source skipped: GitHub`, the firmware was built without production OTA endpoints and did not make an HTTP request. This is not caused by Wi-Fi or manifest length. Recover by flashing a corrected App image over serial while preserving NVS, or by provisioning a valid custom OTA endpoint with the desktop client.

### Time was lost and data is initially blank

With an implausible RTC time, the device first shows placeholders and attempts NTP. Local sensors sample immediately when no cached value exists. Date and network data recover after synchronization.

### Startup screen does not continue

In the rare event of a display startup failure, the firmware releases any acquired SPI bus, panel interface, reset GPIO, display buffers, lookup tables, LVGL buffers, timer, and lock instead of rebooting repeatedly or continuing periodic wake-ups in an unusable state. Power-cycle the device; if it still cannot enter a work page, inspect the serial log for `RLCD display resources unavailable` or `LVGL initialization failed` and the preceding specific error.

If the shared I2C master bus itself cannot be created, startup stops before RTC, sensor, audio, networking, and application tasks are initialized instead of entering a reset loop. Power-cycle the device and inspect the serial log for `I2C master bus unavailable` and the preceding driver error. A missing individual RTC or temperature/humidity sensor remains a separate recoverable device error and does not by itself stop the clock.

During normal operation, a temporary SPI display allocation or timeout error is retried within a fixed limit. If it still fails, only that frame is skipped and `RLCD command/data tx failed` is logged instead of rebooting the device. Repeated messages warrant checking power stability and the logged DMA headroom.

## 12. Safety and Use Restrictions

- Never expose Wi-Fi passwords, QWeather keys, Xiaozhi credentials, or private OTA endpoints in public logs or repositories.
- Keep stable power during OTA or full flashing.
- Xiaozhi AI draws much more current and warms the PCB; leave the page or enable auto-return when battery runtime matters.
- This project is for personal learning, research, and non-commercial use only. Commercial use is prohibited. Third-party components remain governed by their own licenses.
