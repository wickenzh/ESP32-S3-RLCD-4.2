# WeatherClock User Guide

The Aggregate Clock date panel shows the Gregorian day number and the lunar month/day, including leap months. Single-digit dates have no leading zero. Upgrading preserves the existing page settings and appends the new page; no factory reset is required.

Version v1.6.0 adds an Aggregate Clock page: hours/minutes/seconds, today's weather, date/lunar day, and local temperature/humidity. It is appended to the page order and can be toggled or reordered in Display settings. Weather shares the existing hourly cache; sensor sampling remains every minute by day and every two minutes at night. Offline mode disables this network-dependent page. Static black-and-white patterns simulate gray backgrounds.

Web client: [open the browser tool](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/). Firmware and web client sources now share one repository, under `RLCD_CLOCK/` and `host_web/`. The previous web site remains available. This repository migration does not require reflashing or resetting device settings.

This guide explains first-time setup, the eight work pages, hardware keys, online/offline operation, reminders, Xiaozhi AI, OTA updates, DIY assets, and troubleshooting.

> **Important upgrade notice:** `v1.5.x` uses a new flash partition table. A device still running `v1.4.59` or earlier must be fully flashed with the merged image or complete flash package before using `v1.5.x`. An App-only flash or ordinary OTA cannot update the partition table.

> **v1.5.29 weather configuration notice:** QWeather is progressively retiring its legacy public API domains. After upgrading to `v1.5.29`, reopen the setup portal and enter the account-specific domain shown under **Settings → API Host** in the QWeather Console. Weather, alerts, forecasts, and air quality cannot update until this is done. Offline mode does not require it.

## 1. Quick Start

1. Power on the device and wait for the startup animation to finish.
2. On first use, connect a phone or computer to the AP whose name starts with `WeatherClock-`.
3. The captive portal normally opens automatically. If it does not, browse to `http://192.168.4.1/`.
4. Choose an operating mode:
   - Online: enter Wi-Fi SSID, password, QWeather API Key, and the account-specific API Host.
   - Offline: leave Wi-Fi empty and enter the offline date and time.
5. After setup, the clock opens the first enabled page in the saved page order.

Hardware key behavior:

- **BOOT short press:** move to the next enabled work page; confirm in Settings.
- **KEY short press:** open Settings; move the selection inside Settings.
- **KEY long press:** return from a secondary menu; exit Settings from the primary menu.
- **Either key during an alarm or Pomodoro completion sound:** stop the remaining sound and consume that key press.

Button wakeups, alarms, Pomodoro events, and UI refreshes use thread-safe task notification internally. Interaction and response timing remain unchanged, while cross-core notification no longer relies on an unsynchronized task handle.

Settings returns to the current work page after about 30 seconds without activity. Valid operations in the page-order screen restart this timeout.

While Settings is idle, the UI sleeps until a button, synchronization result, or OTA state notification arrives instead of continuously polling the display. Feedback expiry, manual-sync timeout, and the 30-second automatic return still occur at their original deadlines without changing controls or response timing.

If OTA updates the Device Info or update-check status near an automatic-return deadline, the latest hold period now remains active instead of being cleared by an older timeout decision. The 30-second return timing and long-press controls are unchanged.

On every page, the button task waits for a GPIO edge whenever both keys are released and no press is being tracked. Settings, Device Info, Network Diagnostics, and provisioning therefore no longer poll idle keys periodically. A key edge wakes the task immediately, after which the original debounce, short-press, and long-press polling resumes. Background charging, networking, audio, or updating does not affect this behavior.

Button pins and polling fallback intervals are now maintained by the input module. This internal cleanup does not change pin assignments, debounce, long-press timing, Light Sleep wakeup, response timing, or page controls.

## 2. Status Bar and Day Progress

Depending on the page, the status area shows date, weekday, battery, Wi-Fi, reminder, alarm, and local sensor information.

- **Wi-Fi icon:** shown whenever the Wi-Fi radio is on and hidden when it is off, including connection and synchronization periods.
- If the device cannot temporarily reserve the wake resource required for networking, that operation fails safely or retries later without forcing Wi-Fi on. Wait briefly before trying again.
- Wi-Fi name, password, weather API key, and account-specific API Host take effect as one complete configuration. If the system is temporarily busy, it keeps using the previous complete configuration instead of mixing old and new fields.
- Submitting the same online configuration again does not rewrite persistent storage. Connection, weather synchronization, and page behavior after setup remain unchanged.
- Networking and audio keep independent nested wake-resource ownership. If one initialization step fails, resources acquired by that attempt are rolled back instead of permanently blocking the normal low-power state.
- If startup synchronization or OTA encounters a rare Wi-Fi reconfiguration failure, the device also queues radio shutdown after protected network owners finish. Normal connection and update controls are unchanged.
- The final startup network message remains visible briefly, but Wi-Fi and its wake lock are released before that display hold. This shortens unnecessary startup power use without changing the message or startup sequence.
- During automatic startup weather or daily-saying refresh, insufficient contiguous memory keeps the task pending and gradually extends checks through approximately 10, 20, 40, and 60 seconds, capped at once per minute. Manual synchronization, setup validation, and on-demand refresh after entering the related page are not blocked by this background delay.
- Current weather, alerts, forecasts, and air quality are published as one consistent snapshot. If the alerts, forecast, or air-quality endpoint temporarily fails, the last valid data is retained only when the location is unchanged. After a city or IP-location change, alerts, forecast, and air-quality data from the previous location are not mixed with the new current conditions.
- **Sound icon:** shown when an hourly or all-day reminder is enabled.
- **Alarm icon:** shown while the one-shot alarm is enabled.
- Every work page uses the same date, local sensor, minute-time, sound, Wi-Fi, and alarm state sources through one per-page status-bar registry. The Wi-Fi icon directly reflects the actual radio state. Internally, each page now declares the network state it needs instead of receiving it through the shared UI header; switching pages or rebuilding the UI does not change their content, position, visibility rules, or icon-buffer reuse.
- Work pages, System Info, Network Diagnostics, and Settings share one page-lifecycle manager for switching and UI rebuilding. This structural maintenance does not change the eight work pages or the entry, return, and timeout behavior of auxiliary pages.
- Shared bitmap, digit-clock, day-progress, OTA-panel, visible-data-sync, and base-widget interfaces now declare only their actual dependencies. This internal maintenance reduces unrelated coupling without changing page pixels, refresh timing, network rules, or controls.
- Display dimensions and partial-refresh parameters now have one lightweight internal contract, while display, weather, settings, and Xiaozhi activation implementations include only the interfaces they use. Page dimensions, refresh decisions, weather synchronization, and controls are unchanged.
- Provisioning, weather, NTP, HTTP, and background synchronization keep the same public behavior while their internal helpers now declare dependencies directly. Request order, weather data, failure handling, and controls are unchanged.
- Offline, unconfigured, OTA-blocked, and setup-retry states continue to sleep on their existing event or timeout instead of polling continuously. Internal test coverage now executes these waits directly; response timing and user behavior are unchanged.
- Enabling offline mode or restoring factory settings still cancels pending manual synchronization, diagnostics, and OTA requests before networking is stopped or configuration is reset. This maintenance does not change any user-visible result or confirmation flow.
- **Battery icon:** shows the shared battery state on all eight work pages while refreshing only the visible page. During detected charging it blinks on whole-second boundaries. The hardware has no dedicated CHG/VBUS input, so plug/unplug detection based on ADC voltage trends can be delayed briefly.
- Page rendering, charging animation, and low-power scheduling reuse one battery snapshot per UI loop, reducing idle overhead without changing sampling intervals, icons, or controls.
- **Day-progress strip:** shared by all eight pages. Its 60 segments each represent about 24 minutes and refresh only on page entry or when crossing a new segment.
- Every page uses the same internal drawing and boundary rules for this strip. A changed segment is written to the shared canvas buffer as one batch and only that segment is refreshed. Segment count, placement, refresh timing, and visible behavior remain unchanged.

The display favors partial refreshes. Second-level pages redraw only changing digits or small regions; low-frequency pages wait until the next minute, date, sensor, or network-data change before waking for an update.

The temperature/humidity clock updates its sensor text, comfort icons, and trend arrows only when the local sensor snapshot changes. Its seconds display and the existing one-minute daytime/two-minute nighttime sensor schedule are unchanged.

Sound, Wi-Fi, and alarm icon changes no longer reprocess an unchanged sensor snapshot. Entering another page still refreshes its sensor display immediately.

The firmware still tracks display refresh behavior internally, but a normal minute window containing only partial refreshes no longer emits a periodic diagnostic log. A diagnostic is written only when a full-screen refresh occurred; page content and refresh timing are unchanged.

## 3. Eight Work Pages

Press **BOOT** to follow the saved page order. Disabled pages are skipped. Every page can be disabled, but the device prevents disabling the final enabled page.

The eight page identities now share one internal definition across display, storage, and tests. This maintenance does not change the saved order, page switches, controls, or upgrade data.

### 3.1 Weather Clock

Shows large time, date, battery, current weather, alerts, local temperature/humidity, trend arrows, status icons, and an animation area.

- Time and required animation regions use second-level partial refresh.
- Date, minute time, seconds, animation, and second progress use their own change-driven partial refresh rates; hourly chimes continue to follow the saved settings.
- Entering the page requests weather only when required data is missing or stale.
- Offline, low-battery, setup, and OTA states block ordinary weather requests. Low-battery and setup screens may reuse the Weather Clock layout, but that display fallback does not turn on networking.

### 3.2 Picture Clock

Time retains the original square dot-matrix digits. Double rules beside the daily saying adjust to its width and disappear for long or empty text. Image rotation and minute-level clock updates are unchanged.

Shows a local image, large time, daily text, and local temperature/humidity summary.

- Built-in images are selected by weekday and change once per day.
- The built-in Monday and Sunday images use their own tuned monochrome thresholds and edge fades, with dithering and inversion disabled; the other weekday images retain their existing conversion settings.
- When a desktop-client gallery is installed, custom images take priority. `Settings / Display / Picture interval` cycles through `30m / 1h / 6h / 12h / 24h` on local wall-clock boundaries. Built-in weekday images remain fixed at `24h` and cannot be adjusted.
- The temporary custom-gallery read buffer is allocated in PSRAM only when custom images are actually used. Built-in-only operation reserves no memory for it. If PSRAM is temporarily unavailable, the matching built-in image remains visible and the device retries later without consuming critical internal RAM.
- A transient custom-image read failure keeps the matching built-in image visible and triggers only a limited set of minute-spaced retries. A recovered image appears automatically, while persistent failures do not cause continuous reads or redraws.
- Daily text is fetched only when the Picture Clock needs it and is not repeatedly downloaded after a successful update for the same day.
- Rebuilding the page redraws its image, time, and daily text without changing selection rules or controls.

### 3.3 Weather Board

The bottom advice uses today's daytime forecast, not current conditions. A rain/snow advisory may therefore appear while it is sunny now. Without a valid forecast for today, the panel shows a waiting message rather than yesterday's advice.

Shows city, current temperature and condition, air quality, humidity, wind, sunrise/sunset, time until the next sunrise or sunset, weather alerts, and a six-day forecast.

- Missing or stale data causes a full weather refresh when entering the page.
- Sunrise/sunset countdown updates once per minute.
- Local sensor and status text refresh only when their values change.
- Current conditions, alerts, forecast, air quality, and sunrise/sunset formatting now share the same internal production weather types and capacities, preventing parser/display size drift without changing content, synchronization, or controls.

### 3.4 Temperature/Humidity Clock

Shows three high-contrast hour/minute/second cards plus local temperature, humidity, trend arrows, comfort faces, date, and lunar date.

- Seconds refresh locally once per second. Ordinary ticks write the pixel buffer directly and translate the changed digit region to its correct screen coordinates, so the visible card cannot remain frozen while its backing pixels change. This also avoids implicitly invalidating the whole card per pixel. The page sleeps until the next wall-clock second instead of polling repeatedly between ticks.
- Local temperature/humidity is sampled every minute during the day and every two minutes at night.
- Trend arrows use the rolling average of valid samples from the latest four-hour window and restart after reboot.
- Internal page construction and runtime refresh are isolated. A page rebuild restores all clock cards, sensor, trend/comfort, date, and lunar objects while reusing pixel buffers; display, refresh frequency, controls, and stored data are unchanged.

### 3.5 Calendar

Shows the current month, weekday bar, lunar text, and holidays.

- Manual date/time entry, RTC validity, and the lunar calendar currently share the supported range `2024–2035`; dates outside this range are not accepted as valid device time.
- Lunar dates, leap months, and all 24 solar terms within this range are calibrated against the Hong Kong Observatory conversion tables.
- The calendar body redraws only on page entry, date/month change, or a time correction that crosses a day boundary.
- The first frame after a page rebuild always redraws the calendar instead of reusing the previous page's date cache.
- The device calendar and desktop preview share the same weekday, date-cell, and today-highlight layout so future adjustments remain aligned.
- For rare six-row months, once today reaches the sixth row the already-passed first row is hidden so today remains visible.
- When today is on the lowest visible row, its black highlight keeps the complete rounded bottom edge instead of being clipped by the canvas.

### 3.6 Temperature/Humidity History

Shows rolling 24-hour local temperature and humidity curves. Background sensing and hourly history recording continue regardless of the visible page.

- The device page and desktop preview share the same plot, time-tick, value-axis, and extrema-badge layout so the preview remains aligned with the device.
- This page can be enabled, disabled, and reordered like other pages.
- If it is the only enabled page, the device remains on it and does not create an auto-return conflict.
- Hourly samples are published to the chart only after a complete save. History readers cannot observe a mixture of old and new slots, while existing NVS records and upgrade compatibility remain unchanged.
- Low-level hourly-history writes are now owned only by the sensor-history service. Other pages consume complete read-only snapshots, preventing unrelated modules from modifying samples while preserving the existing 48 slots, hourly saves, migration, chart, and controls.
- After page objects are rebuilt, the chart, axes, and extrema redraw immediately instead of waiting for the next hourly sample.

### 3.7 Xiaozhi AI

Provides local wake-word detection, voice conversations, on-screen transcripts, response playback, one-shot alarms, Pomodoro timers, and weather-city configuration.

- High-power voice services start only while entering the Xiaozhi page.
- While waiting for cloud reply audio, an empty playback queue sleeps until new data arrives instead of polling every few milliseconds. Voice content, initial buffering, and playback order are unchanged.
- First use may require binding to the Xiaozhi service by following the on-screen prompt.
- A bound device restores its saved service configuration directly. An unbound device still displays and announces the binding ID first. It is marked announced only after the prompt and all digits finish; if audio is temporarily busy or the task cannot start, a later activation cycle retries without requiring page switching or a reboot.
- An unbound device or failed service activation still retries at the existing interval. Request and response scratch memory is cleared and reused between attempts to reduce long-running heap fragmentation without changing the binding procedure or prompts.
- When the service repeats an identical activation configuration, the device reuses the stored values instead of writing them to flash again. Binding recovery, rebinding, and factory-reset retention behavior are unchanged.
- Speak the wake word while waiting. If the page says Xiaozhi is preparing, allow service initialization to finish.
- If the cloud service recognizes only an incomplete phrase and returns no text or audio reply, the page shows that it did not hear the complete request. Continue or repeat the request; about 12 seconds of silence returns to wake-word standby.
- User transcripts, assistant replies, emotions, and playback completion use one conversation-state path so multi-turn listening, farewell return, and minimum transcript visibility remain consistent.
- Xiaozhi page state is published as a complete snapshot before the UI is notified. If the resource is temporarily busy, the previous complete state remains visible instead of mixing partial new and old text.
- Xiaozhi conversation, retry, and page-lifecycle paths now use the same snapshot entry points directly instead of passing through duplicate forwarding wrappers. Status text, subtitles, expressions, wake-up, and controls are unchanged.
- Service handshakes, ordinary replies, and MCP tool messages share a bounded session buffer. An invalid oversized text frame ends the current session instead of overwriting adjacent memory.
- Wake-word listening starts only after Xiaozhi has acquired the microphone and audio hardware. If an alarm, Pomodoro alert, or prompt is using audio, the existing recovery path retries after the hardware is released instead of starting a partial listener.
- Device-status queries format sensor, battery, and volume data in a fixed-capacity buffer. Under memory pressure, the current query fails cleanly without retaining temporary memory or disrupting later conversations.
- Xiaozhi validates audio lengths before sending, decoding, sample-rate conversion, and playback queueing. An invalid frame ends only the current conversation instead of reading beyond its buffer.
- WebSocket receive, Opus encode, and audio-decode scratch now share one PSRAM lease across conversations and are cleared before and after each one. The encoder no longer allocates a separate buffer per conversation; microphone, speaker, Wi-Fi, and voice tasks still stop after leaving the Xiaozhi page.
- Each voice connection now owns its WebSocket, TLS, and network-serialization resources through one scoped lifecycle. Handshake, buffer, or audio startup failures therefore clean up automatically without retaining network resources. Connection behavior, retry timing, and controls are unchanged.
- The short first-binding code now crosses into its playback task through a fixed slot protected by an application-lifetime static task mutex, avoiding both per-announcement heap allocation and interrupt masking during string copies. Its state is also rolled back if later Xiaozhi initialization fails, allowing a clean retry; prompt and digit order are unchanged, and a failed playback remains eligible for a later activation retry.
- Page status, subtitles, and emotion refresh only when their content changes. Offline, unconfigured, or retry states do not repeatedly redraw the same message.
- After setup transitions, page-resource recovery, or a full UI rebuild, Xiaozhi status, emotion, and an active Pomodoro are immediately restored on the new page objects instead of retaining stale display references.
- While offline mode is enabled or Wi-Fi has not been saved, Xiaozhi waits for a configuration change instead of periodically starting network work. Saving setup, changing offline mode, or leaving the page wakes it immediately.
- Binding or service connection failures retry automatically at about 15-second intervals. Leaving the page, alarms, and Pomodoro events remain immediately responsive during this wait.
- Leaving the page or reaching a failed connection retry stops the ordinary voice session and releases its page-owned network/power resources. Alarm and an active Pomodoro keep running in the background.
- If weather, diagnostics, OTA, or another network task is still using Wi-Fi when Xiaozhi exits, the request is handed to the resident network task and serialized until the final owner finishes, so voice cleanup cannot interrupt a newly acquired network window. A completed shutdown clears that request immediately, so it cannot carry into and unexpectedly close a later connection. Protected ownership still waits only for a real state change. If the Wi-Fi driver itself fails to stop, redundant automatic reconnect remains suppressed between serialized retries of approximately 1, 2, 4, 8, 16, 32, and then at most 60 seconds, until the radio stops or a new network operation successfully takes ownership. This prevents a rare repeated failure from leaving the radio powered indefinitely without introducing rapid polling.
- Audio hardware still shuts down after prompts, alarms, chimes, and Xiaozhi sessions. Repeated audio use reuses fixed object storage to reduce long-running heap fragmentation without keeping the codec powered while idle.
- When an alarm or Pomodoro completion overlaps Xiaozhi audio, both use the same shared wait rule before taking the hardware. Existing timeout, button stop, alert count, and Xiaozhi resume behavior are unchanged.
- Xiaozhi reply queues and playback tasks still run only during a conversation. Their small control metadata now reuses fixed storage to reduce internal-heap fragmentation across repeated conversations without changing page-exit cleanup.
- The intermediate wake-word and speech-processing queue, plus the two recognition tasks' small control blocks, reuse fixed storage. Their larger task stacks still exist only while the Xiaozhi page is active and are released on exit. Leaving the page still stops the microphone, recognition tasks, and audio hardware; fixed control metadata does not mean the device keeps listening.
- This page consumes substantially more power and warms the PCB, which may make the onboard temperature/humidity reading higher than the surrounding air.

### 3.8 Aggregate Clock

Sunny weather uses an arc and dotted rays; rain uses a scalloped cloud band and diagonal streaks. These are static accents selected by weather results, not continuously animated effects.

The test build switches sunny/rainy accents with hourly weather results. Unchanged styles do not redraw, and failed requests retain cached content. There is no sunrise/sunset transition or additional animation timer. Hidden pages do not draw; clock digits still update by second. Existing manual synchronization and missing-data requests are unchanged.

Weather uses a reverse-video header and a white reading area. A thin border frames the date; static edge and footer dot patterns simulate gray without extra refresh timers.

Combines second-level time, current weather and daily high/low, Gregorian day number, lunar month/day, and local temperature/humidity. It is appended to the default page order and supports toggling and reordering in Display settings.

- Only changed time blocks redraw. The date updates on date changes, without a leading zero.
- Weather shares hourly caching and missing-data requests, without retrying solely because air-quality data is absent.
- Local sampling runs every minute from 06:00 to 21:59 and every two minutes from 22:00 to 05:59. Hidden pages stop drawing while necessary background sampling continues.
- Offline mode disables this page. Upgrading appends it while preserving existing settings; no NVS erase is needed.

## 4. Setup Portal

### 4.1 Entering Setup

With no saved online configuration and offline mode disabled, setup starts automatically after boot.

1. Join `WeatherClock-xxxx`.
2. Wait for the captive portal or open `http://192.168.4.1/`.
3. Fill the required fields:
   - **Primary Wi-Fi SSID:** select from the scan list or enter manually. The list shows up to 32 access points; refreshing scans again, and a temporary memory warning does not prevent manual entry.
   - **Primary Wi-Fi password:** password for the preferred network. Leave it empty to keep the saved password when the SSID has not changed.
   - **Backup Wi-Fi SSID and password (optional):** tried when the primary network is unavailable. Leave both empty when no backup is needed. An unchanged backup SSID can also keep its saved password.
   - **QWeather API Key:** required for current weather, alerts, forecast, and air quality.
   - **QWeather API Host:** find it under **Settings → API Host** in the QWeather Console, for example `abc123.re.qweatherapi.com`. Enter only the domain, without `https://`, a port, or a path.
   - **Weather city (optional):** for example Hangzhou. Chinese names ending in `市` are normalized and validated through QWeather. Leave empty for public-IP location.
   - **Offline date and time (optional):** use only when Wi-Fi is intentionally left empty. Leave it blank for normal Wi-Fi setup.

The setup page groups fields into Network, Weather Service, and Offline sections. Nearby Wi-Fi uses a compact list, while validating, success, and failure states use distinct feedback treatments. This layout change does not alter field meaning, save order, or validation rules.

Wi-Fi names up to the router-supported 32-byte limit are passed to the radio driver without losing the final byte. Because `v1.5.29` introduces the account-specific API Host, devices upgraded from an earlier version must reopen setup and fill this field; other saved values remain available for reuse.

An older single-network configuration automatically becomes the primary slot, so an OTA upgrade does not require a factory reset. Each radio session tries the current preferred network first. After bounded repeated failures, or when its connection window expires, that session may switch to the backup only once. A successfully connected backup becomes the new preferred network for the next session; the same promotion rule works in the opposite direction later.

If both networks are unavailable, the device does not switch forever. Ordinary work ends the current connection attempt and returns to the existing synchronization backoff, avoiding continuous Wi-Fi power use. Setup mode keeps the device hotspot and result page available so the user can correct either network. Serial logs expose slot state, SSIDs, and password lengths only, never full passwords.

After Save is pressed, the page immediately shows a validating state; validation does not intentionally restart the device. A background network task connects to the selected router in AP+STA mode, validates the QWeather API key through the configured API Host, and then validates an optional manual city. The page polls the lightweight validation status. The setup hotspot closes only after all checks pass. A Wi-Fi password, API key, API Host, or city failure keeps the hotspot active and shows a specific error. If the phone loses the current HTTP connection while the STA changes channel, reopening `http://192.168.4.1/` shows the latest validation state above the form. The Save button remains on its own row below the date/time field.

After validation, the device serializes the AP+STA-to-setup-hotspot result handoff with other Wi-Fi lifecycle operations before publishing success or failure to the phone. This reliability guard does not change form fields, result messages, retry behavior, or automatic hotspot shutdown after success.

If setup is submitted again while an earlier validation or result page is still active, the device keeps the two save attempts separate. A late page request or network result from the old attempt cannot overwrite, cancel, or keep blocking the new attempt. The new successful result must reach the phone before setup exits normally. An invalid or offline replacement submission also does not revive the earlier online validation.

Normal field values submit directly. If a custom OTA URL or another field makes the form body abnormally long, the portal asks the user to shorten it and keeps the hotspot open. That request does not save a truncated Wi-Fi name, API key, city, or URL.

When setup starts, the device first presents the setup overlay and then plays the provisioning prompt. Normal prompts initialize speaker output only and do not allocate microphone input DMA. If the first display frame temporarily consumes the remaining DMA memory, prompt playback waits briefly and retries a bounded number of times. The setup view keeps RTC-restored hours/minutes and date visible while second animation, GIF, weather, and lower work-page refreshes stay paused.

In the rare case that the platform cannot configure the captive DNS receive timeout, the page may not open automatically. Open the address above manually; the failed DNS service exits cleanly instead of remaining active after setup.

If captive DNS encounters a temporary socket, bind, or receive error, the device now performs a small number of bounded recovery attempts. This reduces the chance that one platform error disables automatic setup-page redirection for the rest of the session. Persistent failures still stop cleanly without an infinite retry loop; reconnecting to the setup hotspot, entering setup again, or opening `http://192.168.4.1/` manually remains available.

When the last phone leaves the setup hotspot, the device resets the captive DHCP lease state. The current lease is preserved while Wi-Fi/API credentials are being validated or a successful result is being delivered, so a temporary channel transition is not mistaken for a real departure. A phone that reconnects can therefore obtain a fresh `192.168.4.x` address. If the captive page does not reopen automatically, visit `http://192.168.4.1/` directly.

The on-device setup status rows follow setup-mode visibility as one panel. If the UI is rebuilt after a mode switch or resource recovery, those rows are recreated and continue refreshing without retaining stale object references.

Setup-field decoding, configuration-event cleanup, configuration storage, setup-start requests, hotspot startup/result handoff, and factory-reset cleanup use lightweight internal helpers. Portal activity, disconnect reason, AP name, local IP, and validation feedback can now be written only by the provisioning subsystem; other pages receive complete read-only snapshots. A failed hotspot start still keeps the request for bounded retry, validation failure keeps the portal available, and success still waits for the phone to read the result before closing the hotspot. This maintenance does not change field names, Chinese text handling, truncation feedback, save results, timeouts, hotspot timing, existing configuration recovery, or button controls.

While the setup hotspot is open and waiting for input, its network task sleeps until Save or portal shutdown instead of waking every 30 seconds. Setup fields, response time, validation, and success/failure feedback are unchanged.

### 4.2 Online Mode

Submitting Wi-Fi credentials stores the online configuration and starts connection. Both the QWeather API Key and account-specific API Host are required for weather services; NTP and daily text do not use them. If the short boot request obtains current conditions but a later alert, forecast, or air-quality step is deferred, the available current conditions are shown first and a staggered background refresh remains scheduled to complete Weather Board data.

The device publishes the Wi-Fi name, password, weather API key, and API Host to background tasks as one complete configuration. Saving cannot mix old and new credential fields, and serial logs do not print the password, full API key, or full Host. The Host is trimmed and normalized to lowercase; legacy public domains, schemes, ports, paths, and domains outside `qweatherapi.com` are rejected.

After setup, NTP, weather, and daily-text requests are staggered to avoid concurrent HTTPS memory peaks. Enabled network pages receive an initial data prefetch even if they are not the first visible page.
If a weather page or Gallery Clock is disabled before its queued startup prefetch runs, that automatic request is cancelled instead of powering Wi-Fi after its delay or memory retry. Manual synchronization and system NTP continue through their existing paths.
If Weather Board is disabled while Weather Clock remains enabled, automatic weather refreshes fetch only current conditions and alerts instead of downloading unused six-day forecasts and air quality. Re-enabling and entering Weather Board keeps any usable older details visible while immediately requesting a complete refresh. Manual Weather Sync and Network Diagnostics still fetch the complete weather set.

Regular HTTPS synchronization and Xiaozhi WebSocket connection setup share one serialized network boundary. If they overlap, the later operation waits for the active transaction to finish instead of competing for TLS memory. User-facing synchronization, OTA, and Xiaozhi controls are unchanged.

While Xiaozhi keeps Wi-Fi online, ordinary weather, time, or daily-text synchronization reuses the connected station instead of disconnecting to reapply identical credentials. It also preserves the real-time Wi-Fi power policy required by the voice session. Setup mode and a genuinely disconnected station still reconnect through the normal path.

The clock synchronizes time once at startup and then at local midnight each day. A failed midnight synchronization remains pending and retries after the normal delay, so crossing past 00:00 does not discard the daily update.

A manual time synchronization is an explicit user request and bypasses any retry deadline left by an earlier startup or midnight failure. If it fails, the settings page reports the result without scheduling an otherwise unused background wake-up. Startup and midnight synchronization retain automatic retries: about 15 seconds while RTC time is implausible and about 5 minutes after time is already valid.

### 4.3 Manual Weather City

A manual city takes priority over IP location and can be set through:

- The setup portal.
- Desktop-client resource configuration.
- Xiaozhi voice, for example “set the weather city to Hangzhou.”

**Settings > Network > Weather City** displays Auto or the saved city. In Auto mode it directs the user to the setup portal, Xiaozhi, or desktop web client to set a city. In manual mode, press BOOT and confirm again to clear it and return to IP location. Clearing immediately queues a weather refresh.

An unrecognized QWeather city is rejected while the setup hotspot remains active. The user can correct the city or clear it to restore automatic IP location; setup does not silently replace an invalid manual choice.

After a city is saved, it takes effect as one complete text snapshot without a reboot across setup, Xiaozhi, Settings, and weather synchronization. Updating a Chinese city while a sync is running cannot expose a partial or mixed city name.

The setup portal, desktop-client resource, Xiaozhi, saved configuration, and Settings display use the same city-length and validation limits. A city is therefore not accepted by one entry path only to be truncated differently by another.

When cities are set repeatedly through Xiaozhi, each confirmed request has its own generation and the newest request is retained even when two consecutive names are identical. After the spoken reply finishes, current conditions, alerts, forecast, and air quality are refreshed through the existing background flow.

### 4.4 Offline Mode

Leave Wi-Fi empty and enter a valid date/time to write the RTC and enter offline mode.

While offline:

- Wi-Fi and normal network services are stopped.
- NTP, weather, alerts, daily text, diagnostics, and OTA are unavailable.
- Weather Clock, Picture Clock, Weather Board, and Xiaozhi AI are disabled and cannot be re-enabled until online mode returns.
- Calendar, Temperature/Humidity Clock, and History remain available.
- Cached data is not actively deleted.
- Offline state is saved across restarts. Settings, page availability, weather, OTA, and Xiaozhi immediately use the same state without requiring it to be enabled again.
- If the router disconnects during the short transition into offline mode, the device will not start another connection. Unexpected disconnects still recover automatically while online.

To leave offline mode:

- With saved Wi-Fi, QWeather API Key, and account-specific API Host, turn it off directly.
- If any of them is missing, confirm twice to enter setup and finish online configuration.

## 5. Settings

Press **KEY** to enter Settings. The left column is the primary menu; the right side contains secondary items. The selected item is shown with inverted colors.

Settings navigation and confirmation state are handed off safely between input, UI, and OTA tasks. Primary/secondary focus, page-manager mode, and all selection indexes are updated as one complete state, so rapid key use cannot briefly combine the wrong focus and item. Returning from About Device or Network Diagnostics and keeping the update panel visible also cannot reuse navigation state from an earlier path. Key behavior and the 30-second inactivity timeout are unchanged.

The complete Settings-navigation write entry point is now limited to the internal navigation and Settings-action owners. Rendering, the UI loop, and other ordinary modules can only read the current snapshot, reducing the risk of a future maintenance change publishing an invalid focus or mode. Menu content, controls, page ordering, confirmations, and timeout behavior are unchanged.

The existing brief protection against accidentally leaving Settings still applies after returning from a secondary menu. Leaving or re-entering Settings now clears that temporary state, so a previous session cannot suppress a new long press. The 800 ms duration, menu hierarchy, and controls are unchanged.

The confirmations for factory reset, leaving offline mode, and clearing a manual weather city are now also maintained only by internal Settings actions and navigation. Rendering can only read whether a confirmation is pending, reducing the risk of another module clearing or triggering one accidentally. Confirmation steps, wording, duration, controls, and exit behavior are unchanged.

Settings, System Info, and Network Diagnostics are always mutually exclusive on screen. Rapid entry or return cannot leave another auxiliary page visible. Their entry points, content, diagnostic progress, key navigation, and 30-second inactivity timeout are unchanged.

Save, synchronization, timeout, and confirmation feedback is published together with its display deadline. The active synchronization type and its 60-second timeout are also handed off as one complete state. Rapid input, a late background result, or synchronization completion cannot expose partial text, mix old and new deadlines, or finish another request. The same item can be retried immediately after a timeout; a late result from the earlier attempt cannot cancel the new request or overwrite its feedback. Wording, duration, and controls are unchanged.

When network settings are saved or restored, Wi-Fi data, the weather API key, and the API Host become active as one complete configuration. Background tasks never consume a half-updated configuration, and passwords, full API keys, or the full API Host are never written to the serial log.

### 5.1 Network

- **Sync Time:** run NTP now.
- **Sync Weather:** update current weather, alerts, forecast, and air quality, refreshing the cache shared by Weather Clock and Weather Board.
- **Update Daily Text:** refresh the Picture Clock text.
- **Weather City:** inspect automatic/manual mode or clear a manual city with confirmation.

Network Diagnostics still updates immediately as each check completes. After completion, the page keeps the same approximately 30-second inactivity return but sleeps until a button or the exact return deadline instead of continuously polling the display.

Each network window reports the requests captured when it started. A new request raised while that window is running is preserved for the next window instead of being cleared by the earlier result.

### 5.2 Sound

- **Volume:** cycle through 20%, 40%, 60%, 80%, and 100% with preview playback.
- **Sound Select:** cycle through available reminder sounds with preview playback.
- **Hourly Reminder 7:00–22:00:** chime only in the daytime window.
- **All-day Reminder 0:00–24:00:** higher priority; chime every hour all day.

When both reminder switches are off, no hourly sound plays. Critical Xiaozhi audio, OTA, and other protected operations avoid concurrent Codec use.
If the device cannot temporarily reserve the wake and clock resources required for playback, that sound is skipped safely and its playback state is released. Later reminders and previews can try again normally.
When audio is busy, rapid repeated sound-setting changes keep only one pending preview. Once playback becomes available, the preview uses the latest selection instead of replaying every intermediate choice.
If Low Battery mode or an OTA download begins while that preview is pending, the device cancels the asynchronous playback instead of opening the Codec later during the protected operation. The next normal preview or hourly chime remains available after the protected state ends.
No vendor audio demo task runs in the background. Audio resources are opened only for actual chimes, previews, alarms, Pomodoro completion, or Xiaozhi sessions and are released through the shared lifecycle afterward.
The button-task and OTA entry contracts no longer pull unrelated page, network, or global-state implementation details into their callers. Button controls and the update check, download, validation, and reboot flow are unchanged.
Codec and sensor I2C register writes no longer allocate internal heap memory on every call, reducing transient fragmentation during repeated previews or Xiaozhi startup. Audio, sensor, and page controls are unchanged.
Xiaozhi shutdown diagnostics now use a separately synchronized audio-resource state instead of reading a Codec pointer while another task may create or release it. This improves diagnostic stability without changing sound controls or use.
Hourly chimes, previews, alarms, Pomodoro completion, and Xiaozhi share one atomic playback claim. Concurrent sounds still allow only one Codec owner, while frequent busy-state checks no longer enter a cross-core critical section.
The audio layer validates speaker and microphone readiness separately. A partial Codec startup cannot publish an unusable audio resource or pass a missing microphone handle to the driver; the current attempt ends safely and a later reminder or Xiaozhi entry may retry.
If no valid playback handle can be created, the device parks the audio pins and releases the high-performance power resources before returning from that attempt, so one initialization failure cannot leave sustained extra power draw.
Production audio now reuses the ES8311/ES7210 I2C controls already owned by the board Codec layer instead of registering duplicate, unused device handles for every session. Sound behavior and hardware addresses are unchanged.
The mono conversion buffer used by hourly chimes, Settings previews, and provisioning prompts now resides in PSRAM, leaving more internal memory available when Wi-Fi, HTTPS, display, and audio work overlap. Prompt content, volume, fades, stopping behavior, and Xiaozhi voice operation are unchanged.
The network transaction lock shared by weather, daily text, Xiaozhi, and OTA is now an independently owned static lifetime resource with direct production tests for initialization retry and contention. This removes a cold-start heap allocation and a source of long-lived fragmentation without changing request order, timeouts, or user operation.
The OTA status snapshot and the provisioning page's AP-name/IP text snapshots now use the same application-lifetime lock owner as other permanent runtime state. This removes duplicated initialization bookkeeping without changing OTA sources, progress UI, reboot behavior, provisioning feedback, Wi-Fi operation, or displayed addresses.
The fixed capacities for OTA version, download URL, and SHA256 metadata are now owned by the OTA module, so the pure parser interface no longer carries unrelated display or application-state dependencies. Manifest format, source priority, download validation, and user operation are unchanged.
Daily text and its successful synchronization time are now published as one consistent snapshot, preventing a page refresh from pairing new text with an older timestamp. The internal cache remains 160 bytes; the 22-character limit, fetch timing, and display behavior are unchanged.
The depth counter mutex used by network and audio power locks now uses the same static lifetime owner as other permanent task state. If that owner cannot initialize, power-lock setup stops before creating partially usable handles and can be retried later. Normal light sleep, network, and audio behavior is unchanged.
Network, audio, and audio-performance lock types, names, handles, and nesting counters are now maintained together per lock. This reduces the risk of mismatching a lock handle and its reference counter during future maintenance without changing CPU frequency, light sleep, networking, playback, or Xiaozhi behavior.
Current local temperature, humidity, trends, and refresh version are also published as one task-level snapshot, so pages and Xiaozhi cannot observe fields from different sampling batches. Sampling intervals, arrows, and display behavior are unchanged.
Battery runtime, the current local-sensor snapshot, and the 48-slot hourly history now share the same application-lifetime lock ownership pattern. This maintenance cleanup removes duplicated initialization bookkeeping without changing sampling, charging detection, NVS history, page readings, or controls.
Internal sensor, battery, hourly-history, wall-clock, and power-lock interfaces now declare their own actual dependencies. This reduces unrelated maintenance coupling without changing sampling cadence, RTC behavior, charging detection, light sleep, or displayed readings.
The power-management implementation also imports only its lightweight metadata, ESP PM, logging, and lock contracts instead of the full application-state aggregate. This is a maintenance boundary only; CPU frequency, light sleep, networking, and audio behavior remain unchanged.
Xiaozhi internals now read the application log tag and version through a lightweight metadata contract instead of also importing unrelated display, weather, and system-state declarations. Binding, wake-up, conversation, MCP, audio, networking, and page behavior are unchanged.
Shared audio and chime orchestration also declare only the logging, task, battery, and hardware contracts they actually use instead of importing unrelated display, weather, network, and OTA internals. Hourly chimes, setting previews, provisioning prompts, Xiaozhi audio, power behavior, and controls are unchanged.
Weather location text and QWeather response handling now load only the weather types, JSON, buffer, and logging contracts they actually use instead of unrelated display, OTA, audio, and system state. City resolution, weather data, failure messages, and page output are unchanged.
The generic HTTP client, IP geolocation, provisioning weather validation, QWeather client, and weather runtime state now follow the same lightweight dependency boundary. Network requests, parsing, caches, event notifications, and controls are unchanged.
Provisioning AP credentials, portal addresses, and built-in primary/backup OTA manifest endpoints are now owned by one lightweight compile-time configuration contract. Provisioning, diagnostics, and update behavior are unchanged, and private deployment endpoints remain outside the tracked source tree.
NTP synchronization and last-successful-sync queries now use a dedicated lightweight service interface. Startup time recovery, scheduled synchronization, RTC updates, diagnostics, and displayed information remain unchanged.
RTC restoration and system-time writeback now use a dedicated internal interface as well. The device still restores plausible RTC time at startup and writes corrected NTP or offline manual time back to the RTC through the same sequence; alarm rescheduling and page refresh behavior are unchanged.

Generic HTTP text requests and the startup connectivity budget now use separate lightweight interfaces as well. Ordinary requests still use a fixed 10-second timeout and are shortened only when the remaining startup budget is smaller. Weather, IP geolocation, daily text, diagnostics, and OTA checks retain their existing URLs, certificates, startup staggering, retry behavior, display, and user controls.
The startup-screen completion state can now be changed only by the application startup flow. Networking and weather modules can only read it and continue applying the existing first-minute staggering and memory protection, reducing the risk that a future maintenance change starts concentrated HTTPS work too early. The boot screen, work-page transition, weather refresh, Wi-Fi use, and controls are unchanged.
Startup time validation and background network sleep duration now consume their existing shared policies directly instead of passing through duplicate network-service wrappers. Startup time recovery, hourly synchronization, low-power waiting, and button wake-up behavior remain unchanged.
OTA, startup connectivity, and Xiaozhi now share one lightweight Wi-Fi lifecycle interface. Connection waiting, provisioning AP behavior, on-demand radio shutdown, and Xiaozhi session keepalive behavior remain unchanged.
Weather city, current conditions, alerts, forecast, and air-quality requests now share a dedicated client interface. Provisioning validation, weather-page caching, and Xiaozhi city configuration behave as before.
Automatic IP geolocation now also exposes one lightweight interface shared by weather updates and network diagnostics. Its endpoint, city result, failure handling, and user controls are unchanged.
Full weather refresh success, failure, and resource-deferral results now use a dedicated internal interface. Background synchronization, diagnostics, cache publication, and page behavior are unchanged.
Provisioning startup/result handoff and Daily Saying retrieval now use their dedicated internal interfaces. Provisioning feedback, Daily Saying retries, caching, and gallery-clock output are unchanged.
The public Network Diagnostics interface now retains only the reset operation needed by settings and UI code. Session start, completion, unavailable-result publication, and all nine checks use a dedicated network-internal interface, while per-line result writes remain private to the executor. Offline and unconfigured feedback, check order, timeouts, displayed results, and user operation are unchanged.
Provisioning HTTP and captive-DNS lifecycles now use their dedicated internal interfaces, while route handlers remain private to the portal implementation. Captive DNS startup, shutdown, and repeated setup entry use one task-state handoff, preventing overlapping DNS services during rapid setup transitions. Browser compatibility, form submission, validation feedback, and hotspot behavior are unchanged.
The four-hour trend samples and 48-slot hourly history now share one stable production data definition, and host validation uses the real firmware layout instead of a parallel test copy. NVS data, restart recovery, history charts, and page operation are unchanged.
The application event identifiers and event-group resource shared by provisioning, synchronization, OTA, and startup are managed by one internal owner and remain a static lifetime resource. Calls made before initialization or after startup-failure cleanup fail safely instead of touching an invalid handle. Event delivery, wait timeouts, and user operation are unchanged.
The Xiaozhi page snapshot lock also uses a static control block, reducing startup internal-memory allocation and long-term fragmentation without changing wake-up, subtitles, expressions, Pomodoro, or page controls.
The internal Xiaozhi event group used for page activity, wake-up, and suspension also uses a static control block, further reducing startup allocation without changing page transitions, alarms, or Pomodoro behavior.
If button GPIO setup fails during a rare hardware initialization fault, the firmware now shuts down the button task cleanly while leaving other background services intact instead of returning directly from a FreeRTOS task entry. Normal button, debounce, and page-switch behavior is unchanged.

After the startup animation and initial network tasks exit, FreeRTOS may need a brief Idle-task window to reclaim their temporary stacks and task control blocks. If transient memory pressure prevents a resident network, OTA, sensor, UI, button, alarm, or pomodoro task from being created, the firmware retries only the missing service for a bounded period. It never duplicates a task that already started and never retries indefinitely. Normal startup timing is unchanged; a persistent failure remains visible through the named task log and failure bitmask.
RTC and SHTC3 setup also rejects an unavailable shared I2C bus. The display and shared I2C bus each have one firmware owner, and RTC, the temperature/humidity sensor, and audio always reuse the same I2C bus instead of constructing parallel application buses. The sensor module privately owns the application-lifetime SHTC3 object in static storage to reduce startup heap-allocation failure and long-term fragmentation, while complete destruction still releases its owned device handle. Each sample starts only after a confirmed wake command and verifies that the sensor returns to sleep, with one short retry for a transient sleep-command failure to avoid excess idle power. GPIO assignments, sensor addresses, first-sample ordering, intervals, and displayed readings are unchanged.

### 5.3 Display

- **Page switches:** enable or disable pages. Network pages are blocked while offline, and the final enabled page cannot be disabled. Changes are handed safely to page switching and background network decisions without changing display or sync rules.
- **Page order:** lists only enabled pages. KEY selects; BOOT exchanges the selected page with the next one. The first item becomes the boot home page. An application-lifetime static task mutex publishes the complete order as one snapshot, and each sorting action uses one consistent page-enable snapshot for normalization and home-page validation. Rapid sorting, page-switch changes, or page switching therefore cannot mix two configuration batches or observe a half-finished exchange. If saving fails, the previous order and selection are restored instead of leaving a change that disappears after reboot.
- **Xiaozhi power saving:** enabled by default after a fresh flash or factory reset. After five minutes without valid activity, return from Xiaozhi to the first page. Auto-return pauses while a Pomodoro runs, and a later manual choice remains saved.
- **Alarm:** displays the one-shot alarm and allows manual disable when active.
- **Picture interval:** defaults to `24h` after a desktop-client custom gallery is first imported, then cycles through `30m / 1h / 6h / 12h / 24h`. Every interval is anchored to local midnight: `24h` changes at `00:00`, `12h` at `00:00/12:00`, `6h` at `00:00/06:00/12:00/18:00`, `1h` on each hour, and `30m` on `:00/:30`. With built-in images, the value is fixed at `24h` and BOOT does not change it.

These five items use the same compact two-column sizing as the System menu items. The device UI and desktop preview share the same layout definition, so their positions remain consistent.
The alarm button reserves a separate area for its status dot, so an active alarm time does not overlap it. The shorter Xiaozhi power-saving label stays aligned with the Page switches label.

Xiaozhi AI cannot be the first page because its high-power service is unsuitable as the boot home page. At least one non-Xiaozhi page must remain enabled.

### 5.4 System

- **Offline Mode**
- **Network Diagnostics**: checks local IP, public IP, IP location, DNS, QWeather, NTP, Daily Saying, internet access, and the OTA manifest. The DNS item resolves the currently saved QWeather API Host and GitHub host instead of a retired public QWeather domain, and reports success only when the resolver actually returns an address. The local IP and all nine results are published as one consistent snapshot and update without a reboot, so connection, lease, disconnection, or background updates never expose a partial address or status line. Public-IP responses are accepted only when they contain a valid four-part IPv4 address; service error text, host names, and out-of-range addresses are reported as failures instead of false successes.
- **Factory Reset** (requires confirmation)
- **About Device** (version, battery, voltage, last full-charge time, device information, and source repository). A record is created only after charging has been confirmed, the session started below `96%`, lasted at least 60 seconds, and crossed the full-charge threshold. Unplugging, voltage fallback, or a high-battery reboot load recovery does not create a new record. A high-battery plateau may still stop icon animation but cannot overwrite the history. The timestamp is stored in NVS and remains visible after reboot. Because the board does not expose the charger status pin to the ESP32-S3, this remains a conservative ADC-based software estimate rather than a hardware charger signal.
- **Check Update**

About Device and Network Diagnostics each maintain their own dynamic content. After a UI rebuild they recreate the current device information and latest diagnostics snapshot without changing entry, long-press return, or post-check timeout behavior. Both pages now include only the display interfaces they actually use, reducing unrelated maintenance coupling while preserving device information, diagnostic checks, layout, refresh timing, and controls.

The idle, running, and completed diagnostics states now use the same internal typed snapshot. Only the diagnostics executor can publish state and the nine result lines; other pages can only read results or request and close the diagnostics page, preventing partial results from unrelated maintenance. Ordinary work pages no longer read this state while Network Diagnostics is closed. The nine checks, their display order, and all controls remain unchanged.

The settings menu, feedback line, manual-sync status, page order, and update progress panel are likewise maintained by their owning UI modules. The feedback line and manual-sync status now belong to one settings-feedback implementation while retaining two independent application-lifetime locks, removing production files that had only one business owner. The boot screen, setup status, and Settings implementation now include only the display interfaces they actually use, reducing unrelated maintenance coupling. Startup configuration loading and routine configuration saving use separate internal entries, while sound, page enable/order, and Xiaozhi auto-return settings share their dedicated persistence entry. NVS content, reboot recovery, save timing, and feedback are unchanged. Returning from setup, About Device, or Network Diagnostics rebuilds them from current state without changing the boot screen, setup status, item order, page sorting, the 30-second timeout, OTA percentage, speed, or progress bar.

Settings menu indexes, item counts, and manual synchronization operations now share one internal definition. This prevents display, key handling, and network actions from drifting apart without changing any visible menu order, labels, or controls.

## 6. Alarm and Pomodoro

### 6.1 One-shot Alarm

The single alarm can be set, changed, or disabled through Xiaozhi voice.

- Example: “Wake me tomorrow at 6:30.”
- Replacing an existing different alarm requires explicit confirmation.
- Alarm and Pomodoro completion cannot target the same local minute; the later request is rejected.
- The enabled alarm runs in the background and sleeps directly until its next target time instead of waking every minute. Changing the alarm, NTP synchronization, or manually correcting time wakes it immediately to recalculate the target.
- The alarm repeats after about five seconds for up to one minute.
- Either hardware key stops it.
- It disables itself after ringing.
- Saving an identical alarm state does not perform another Flash commit; set, disable, and reboot-restore behavior is unchanged.
- Alarm enablement, ringing state, time, and replacement confirmation are updated as one thread-safe state. Status icons use a lightweight enablement read, while rapid voice replacement, background triggering, and page refresh cannot observe mixed alarm data.

### 6.2 Pomodoro

- Say “start a 25-minute Pomodoro” or “focus for 45 minutes.”
- Default is 25 minutes; valid range is 1 second to 99 minutes 59 seconds. Starting again changes the active duration.
- Ask for remaining time, or say “cancel the Pomodoro” / “end focus.”
- If the Pomodoro finishes while Xiaozhi is visible, voice listening pauses only for the completion sound. The service is not restarted while paused, and wake-word standby resumes directly afterward.
- Ordinary “remind me in 10 minutes” requests remain alarm requests.
- It continues after changing pages but is cleared by reboot.
- While Xiaozhi remains visible, the ordinary clock and Pomodoro cards share the same second-level partial-refresh path, so an active countdown continues to advance while voice services are running.
- Pomodoro state, monotonic deadline, and completion time are published as one thread-safe snapshot. Page changes, concurrent background work, and NTP corrections cannot expose mixed timer state; countdown behavior and normal-clock restoration after completion or cancellation are unchanged.
- In the final minute, the minute card shows `00` and the right card shows whole remaining seconds; hundredths are intentionally not displayed.
- Completion shows a completed state and plays two prompts. Either key stops playback.
- Saying only “close” or “stop” exits the Xiaozhi conversation and does not cancel a background Pomodoro.

## 7. OTA and Flashing

For each public source release, GitHub Actions automatically attaches two build outputs to the matching GitHub Release: `weather_clock_vX.X.X.bin` for OTA or app-partition flashing, and `weather_clock_vX.X.X_merged.bin` for a complete flash from address `0x0`. The automated build only adds firmware assets and does not rewrite the complete source Release notes. The source tag also contains one bounded, numbered OTA summary shared by the GitHub and Gitee OTA repositories.

The GitHub primary OTA repository and Gitee fallback OTA repository are updated from the same source build. The source workflow sends only a synchronization event; each target repository downloads both Release assets, verifies size and SHA256, reads the uploaded files back, and only then updates its own Release and manifests. A failed target keeps its previous manifest active, so devices do not see a partial release.

Both automation paths now share one firmware-artifact naming and validation contract, preventing the app and complete flash image from drifting apart. Existing filenames, verification, device OTA steps, and serial flashing instructions are unchanged.

The maintenance verifier also uses one shared argument contract for app/merged names, sizes, and SHA256 values, preventing one mirror target from omitting either firmware image. User-visible filenames, manifests, and OTA steps are unchanged.

The maintenance release gate also confirms that online assets came from the latest build for the current version tag and that both GitHub OTA and Gitee OTA manifests match the app and merged size/SHA256 metadata. During a same-version replacement, still-reachable older assets cannot satisfy the new build; users do not need to change the update-check or download workflow.

If GitHub or Gitee temporarily returns a missing, null, or incorrectly typed asset/version list, the maintenance flow treats it as not synchronized and continues its bounded retry instead of publishing half of a release. Device update, download, and fallback-source behavior are unchanged.

The maintenance flow applies one SHA256 and file-size validation rule to source Release assets, GitHub OTA assets, and Gitee OTA assets. Hash letter case is normalized, while malformed hashes or invalid sizes retain the previous online manifest instead of being accepted as installable firmware. Device JSON and update steps are unchanged.

The source-build and fallback-mirror tools also share the same manifest field names, 1,800-byte `latest.json` limit, and ten-version history limit. The Gitee deployment installs that shared contract together with its mirror worker, so both OTA repositories keep matching app and complete-image metadata. This maintenance change does not alter the JSON consumed by devices or the desktop client.

Internally, provisioning, offline mode, chime, volume, and Xiaozhi auto-return settings are safely published to background tasks. Offline mode can now be changed only by the saved-configuration path, so ordinary display, weather, OTA, Wi-Fi, and Xiaozhi consumers cannot accidentally re-enable network work. Xiaozhi auto-return keeps its dedicated runtime state. These maintenance boundaries do not change where settings are edited, how they are saved, or how they are restored after restart.

Network configuration, setup-form submission, the background synchronization task, and the Wi-Fi radio lifecycle are now maintained by separate internal modules instead of one aggregate interface. This is a maintenance-boundary change only: Wi-Fi, QWeather API Key, manual city, offline mode, setup feedback, and synchronization behavior are unchanged, so an update does not require reconfiguration or a factory reset.

If a stored Wi-Fi configuration cannot be applied to the radio driver, an ordinary synchronization attempt now ends immediately instead of waiting through the full connection timeout. In setup mode, the setup hotspot and web page remain available so the configuration can be corrected. Normal Wi-Fi connection and provisioning steps are unchanged.

If the setup web service fails during startup, the device now uses the same complete radio cleanup path as other network sessions. If the radio driver cannot stop, its real active state remains visible for later recovery and status indication instead of becoming hidden background power use. Normal users do not need any additional steps.

OTA check state, download progress, speed, and reboot notices are likewise published as one consistent snapshot between background tasks and the UI. The check, confirmation, download, and restart workflow is unchanged.

OTA Wi-Fi cleanup now uses explicit internal ownership modes: ordinary checks and failed downloads release the network wake lock, while a verified successful update keeps the CPU awake only until restart. Update controls, displayed progress, radio shutdown, and reboot timing are unchanged.

If an update check or installation times out while waiting for the router, the device now immediately uses the OTA-specific radio shutdown path and preserves the existing deferred retry for a low-level driver failure. This prevents Wi-Fi from remaining powered after a weak-network failure without changing messages, retry behavior, update sources, or user controls.

If Offline Mode is enabled while a queued update request is being handed to the OTA task, the task rechecks the live mode before acquiring its network wake lock. It stays offline and reports the Offline Mode reason instead of briefly reserving network power resources or reporting a Wi-Fi failure.

OTA state, download progress, and the pre-restart quiet window are now published only by the update service. Other pages, audio, sensors, and ordinary network tasks can only read them. This reduces the risk of maintenance accidentally changing update power gates or restart state without changing update checks, confirmation, percentage, speed, validation, or restart controls.

When a normal weather, Daily Saying, or network-diagnostic session finishes while OTA, setup, or Xiaozhi still owns Wi-Fi, the device now records a deferred shutdown and turns off the radio after the final owner exits. A path that never acquired the current network session cannot create a new shutdown request. Synchronization content, timing, retries, and controls are unchanged.

The online manifest may include release notes for publishing tools and the desktop client. The device retains only the version, download URL, file size, and SHA256 metadata required for installation instead of keeping unused release-note text in memory.
OTA manifests and both OTA mirror Releases use the same bounded summary from the source tag. Complete numbered notes remain in the matching Gitea/GitHub source Release so long descriptions cannot interfere with device update checks.

Open **Settings > System > Check Update**:

1. Press BOOT to check.
2. When a new version is available, press BOOT again within 60 seconds.
3. Download percentage, speed, and progress bar remain visible.
4. After validation, the device shows a reboot notice before restarting.

The firmware follows OTA download redirects and closes the current HTTP connection on failure or early exit. Initial and redirected download addresses must fit completely in the protected URL workspace; an empty, missing, or oversized redirect is rejected instead of being silently truncated. A failed download does not switch the boot partition and can be retried.
OTA diagnostics keep status codes, address lengths, image size, battery, signal strength, stages, and error codes, but no longer print complete manifest or redirect addresses. Custom server addresses and signed temporary query parameters therefore remain out of the serial log.

After a firmware image has downloaded successfully and its HTTP data is confirmed complete, the device now releases the finished 4 KiB transfer buffer before final image validation and boot-partition selection. Update sources, progress, integrity checks, failure fallback, restart behavior, and controls are unchanged.

Temporary Wi-Fi and battery diagnostics, SHA text formatting, and new-image description data are also kept only for the short stage that uses them, reducing OTA task stack pressure without changing the download or validation sequence.

OTA uses a primary remote source and an event-driven GitHub fallback. The fallback may briefly remain on the previous version while source build and mirror validation are still running.

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

Internal resource-loader dependencies and duplicate diagnostic declarations have been simplified. Metadata for both the built-in weekday gallery and weather-status GIF is kept separate from the full pixel payloads, reducing repeated processing of large generated resources during firmware builds. Weather-status GIF frame differences are also written to the canvas buffer in one batch before only the changed rectangle is refreshed, avoiding repeated full-canvas redraw scheduling for individual pixels. The package format, validation sequence, messages, image/GIF output, frame rate, weekday mapping, desktop-client interface, and built-in fallback behavior are unchanged.

Because `v1.5.0` moved partition addresses, an old desktop client must be updated to use the current partition table before writing resources.

## 9. Battery and Low-power Behavior

- Battery is sampled immediately after boot.
- When not charging, battery ADC follows the same schedule as local temperature/humidity: every minute during the day and every two minutes at night.
- Temperature and humidity samples notify the active page for an on-demand update. Stable text is not redrawn repeatedly; a roughly one-minute fallback check remains for resilience.
- During confirmed charging, battery sampling remains at about once per second even after the visible charging animation stops. Normal day/night sampling resumes only after unplugging is detected.
- Once charging is confirmed, boot scheduling, OTA recovery, and network time recovery no longer create a temporary minute-level sampling gap. Charging and unplug detection retain their fast response.
- Low-battery thresholds, charging detection, animation stopping, and fast charging sampling share one internal policy source. The animation state controls only the visible blink and no longer delays unplug detection; displayed percentage, low-battery behavior, and OTA protection are unchanged.
- Each battery reading releases the ADC and calibration resources after publishing the result, so the measurement peripheral is not kept active between samples.
- If ADC channel setup and resource release both fail in the same rare recovery path, a later sample reconfigures the retained channel before reading; no user restart is required, and normal sampling cadence and percentage calculation are unchanged.
- Production firmware does not emit raw ADC, voltage, and percentage details for every sample. Sampling failures and charging start, animation completion, and unplug transitions remain logged. Sampling cadence, battery calculation, and displayed values are unchanged.
- Percentage, charging state, and low-battery mode are published consistently from the same sample; pages, OTA, and Xiaozhi do not trigger an extra ADC conversion when reading battery status.
- Low battery enters a minimal page and stops non-essential networking, animation, audio, and high-frequency refresh.
- After charging raises the battery above the low-battery exit threshold, the device returns to the work page that was active before low-battery mode. If that page is no longer enabled, it returns to the current first enabled page. The temporary page is kept only in RAM for the current boot and is restored after provisioning finishes when the setup portal is still active.
- If weather, Daily Saying, or Network Diagnostics was queued just before low-battery mode begins, the request is cancelled before Wi-Fi starts. Network Diagnostics reports "Skipped: low battery"; manual time sync and provisioning keep their existing behavior.
- If low-battery mode begins while Network Diagnostics is running, the current check finishes safely and later checks stop. Completed results remain visible and pending rows report that they were skipped.
- Charging state and percentage are estimates derived from voltage trends and are not precision battery instrumentation.

## 10. Factory Reset and Retained Data

Factory reset clears:

- Wi-Fi SSID/password.
- QWeather API Key and manual city.
- Offline mode.
- Sound, volume, and reminder settings.
- Page switches/order and Xiaozhi power-saving auto-return.
- One-shot alarm.

Factory reset retains:

- Existing Xiaozhi activation/binding data.
- Stored sensor history.
- Desktop-client DIY assets.

The device enters setup as an unconfigured clock only after both the regular settings and the one-shot alarm have been cleared. If storage cleanup fails, the device reports the failure and preferentially retains Wi-Fi/API Key/API Host data so the reset can be retried.

## 11. Troubleshooting

### Setup portal does not open

Stay connected to the `WeatherClock-` AP, disable automatic mobile-data switching, and browse to `http://192.168.4.1/`.

The startup screen, on-device setup status, and phone portal all show the same complete name of the active setup AP. If the name is blank or inconsistent, re-enter setup mode and retain the serial log.

If portal startup fails, the device removes the incomplete AP and restores its previous Wi-Fi mode instead of leaving an unusable high-power hotspot active. Retry setup after a short wait.

If another network operation is finishing, or temporary resource pressure prevents the AP from starting immediately, the setup request remains queued and retries at a bounded interval instead of repeatedly restarting Wi-Fi. The portal, validation feedback, and correction flow remain unchanged.

Writing an already-stopped portal state no longer counts as a network-state change, so a failed start cannot wake its own retry early. A real portal stop still wakes pending result delivery and network work immediately.

The portal accepts the longer request headers commonly sent by current phone browsers. If the phone still shows `Header fields are too long` or HTTP `431`, close the automatically opened captive window, reconnect to the device AP, and browse to `http://192.168.4.1/` manually. This means that particular web request was rejected before save processing; it does not indicate damaged NVS, Wi-Fi credentials, QWeather API Key, or API Host.

### Weather remains “Waiting for data”

Verify Wi-Fi, QWeather API Key, account-specific API Host, and city. Run **Network > Sync Weather** or use **System > Network Diagnostics** to inspect QWeather, DNS, and Internet access.

If an automatic sync encounters Wi-Fi startup failure, connection timeout, or setup weather-configuration validation failure, the device keeps its existing weather cache and resumes through the existing bounded retry or next scheduled attempt. A setup validation failure keeps the portal available with the specific reason, while a normal background failure releases that network session. A transient setup-hotspot start or stop failure keeps its existing bounded retry, while network state changes can wake the retry early. User interaction and retry timing are unchanged, and Wi-Fi, API Key, API Host, or city settings are not erased. If data remains unavailable, use manual sync or Network Diagnostics.

Network-failure cleanup now updates one complete background-sync runtime state instead of passing weather, Daily Saying, and NTP fields separately. This internal maintenance does not change synchronization timing, error feedback, backoff, cached data, Wi-Fi control, or user interaction.

If startup memory is temporarily constrained, later weather requests may be deferred. A completed IP-location result is reused briefly instead of calling the location service again on every retry. No user action is required, and this temporary context is cleared after success, failure, timeout, or a switch to a manual city.

If the display is rebuilt after setup-mode switching or display-resource recovery, the weather city, status, icon, temperature, and humidity are repopulated on the new page objects. Existing cached weather is not cleared by a UI rebuild.

The weather clock's local temperature, humidity, and trend arrows are also restored on the rebuilt page. A UI rebuild does not clear sensor samples or restart the four-hour trend calculation.

The top date, weather alert, and sound, Wi-Fi, and alarm icons are restored from their current state after the rebuild. Rebuilding the UI does not change alert rotation or the enabled state of those features.

The main time, seconds, status animation, second progress, and low-battery indicator are also restored after a UI rebuild and continue using the same minute, second, and partial-refresh rules.

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

Check access to the primary manifest. The GitHub fallback is synchronized by the source-build completion event and may briefly remain on the previous version while the build or mirror job is still running.

If the serial log shows both `OTA manifest source skipped: GitHub` and `OTA manifest source skipped: Gitee`, the firmware was built without production OTA endpoints and did not make an HTTP request. This is not caused by Wi-Fi or manifest length. Recover by flashing a corrected App image over serial while preserving NVS, or by provisioning a valid custom OTA endpoint with the desktop client.

### Time was lost and data is initially blank

With an implausible RTC time, the device first shows placeholders and attempts NTP. Local sensors sample immediately when no cached value exists. A failed automatic NTP attempt retries after about 15 seconds, while a manual request bypasses that delay. Successful synchronization immediately refreshes date, weekday, and time; staggered weather and daily text then continue in the background. Repeated `task_wdt` reports naming `network_sync` indicate an abnormal busy loop rather than normal waiting and should be retained for diagnosis.

The last-sync value shown under About Device is read from the same task-mutex snapshot that the network task updates. SNTP waiting, RTC writes, and UI notifications remain outside that mutex, so viewing the value cannot extend a sync attempt or change its result.

Alarm, Pomodoro, last-NTP-sync, About Device, daily saying, manual weather city, network credentials, network diagnostics, complete weather data, and pending Xiaozhi weather-city requests now share the same application-lifetime static-mutex owner internally. This removes repeated initialization code without changing countdowns, alarm timing, synchronization, provisioning, weather updates, network checks, page timeouts, memory allocation, or controls; resources that require explicit rollback keep their existing lifecycle.

### Startup screen does not continue

In the rare event of a display-resource or panel-register startup failure, the firmware releases any acquired SPI bus, panel interface, reset GPIO, display buffers, lookup tables, LVGL buffers, timer, and lock instead of rebooting repeatedly or continuing periodic wake-ups in an unusable state. Maintenance checks also require every display buffer to be released exactly once and one initial fault to produce one matching error, reducing the risk of heap corruption or duplicate diagnostics after later changes. RLCD lifecycle and transfer diagnostics share one module tag instead of storing an unnecessary pointer in the single display object; normal log text and rollback behavior are unchanged. The long-lived LVGL display lock, handler-task stack, and task control block use static storage to reduce internal-heap allocation and long-term fragmentation at startup; page, button, and refresh behavior are unchanged. Power-cycle the device; if it still cannot enter a work page, inspect the serial log for `RLCD display resources unavailable`, `RLCD panel register initialization failed`, or `LVGL initialization failed` and the preceding specific error.

The startup animation writes each complete frame into its canvas buffer before requesting one display refresh, avoiding repeated per-pixel redraw scheduling. After it hands off to the first work page, the firmware releases the approximately 25 KiB PSRAM canvas used only by that animation. This leaves more startup CPU and memory headroom for weather networking, page rendering, and audio without changing the startup screen, frame timing, duration, or page transition; no user action is required.

If the shared I2C master bus itself cannot be created, startup stops before RTC, sensor, audio, networking, and application tasks are initialized instead of entering a reset loop. Power-cycle the device and inspect the serial log for `I2C master bus unavailable` and the preceding driver error. A missing individual RTC or temperature/humidity sensor remains a separate recoverable device error and does not by itself stop the clock.

When a desktop-provisioned custom asset package is present, startup still validates the complete package with header and payload CRC checks. The optional four-frame image-density diagnostic now runs only when DEBUG logging is enabled and continues to use fixed-size chunked reads. Normal startup, the wire format, rendering, and fallback to built-in assets after corruption remain unchanged.

The custom asset catalog is loaded once before work tasks start. Pages, weather-city selection, and OTA configuration then use the validated catalog as read-only data. This maintenance does not change the WCA1 format, asset priority, built-in fallback, or desktop workflow.

During normal operation, a temporary SPI display allocation or timeout error is retried within a fixed limit. If it still fails, only that frame is skipped and `RLCD command/data tx failed` is logged instead of rebooting the device. The network/OTA DMA protection mode uses an allocation-free atomic snapshot, so display transfers no longer disable cross-core interrupts merely to read the active protection tier; chunk sizes, retry counts, and rendered output are unchanged. Repeated messages warrant checking power stability and the logged DMA headroom.

The display driver's pixel lookup tables now use the actual screen height and reject geometries that cannot be represented safely before startup. This prevents row aliasing, out-of-bounds access, and corrupted output after future orientation or maintenance changes. The current device's 400x300 pixels, PSRAM use, page layout, and refresh behavior are unchanged, and no setting change is required.

Shared bitmap, label, and font declarations use lightweight internal interfaces. This maintenance does not change Chinese glyphs, icon pixels, page coordinates, trend arrows, or partial-refresh behavior.

The weather clock and temperature/humidity clock now share one read-only DSEG bitmap set, removing about 5 KiB of duplicated firmware data. Weather-clock digits are also written to the canvas buffer as one batch before the existing final refresh, reducing repeated second-level display work. Digit shapes, sizes, positions, and refresh timing remain unchanged.

Battery, alert, sound, Wi-Fi, alarm, trend, temperature, and humidity icons now also share one read-only resource set across pages. Their 1-bit pixels are written to the canvas buffer as one batch before a single final refresh, reducing repeated display work. Icon pixels, line weight, positions, visibility rules, and update timing remain unchanged.

Canvas buffers, base configuration, and partial invalidation are also maintained through one lightweight internal contract. Day progress, history charts, the Settings OTA panel, and status animation retain their existing pixels, coordinates, and refresh timing; no user setting or interaction changes.

The boot screen and startup progress now use a dedicated internal interface, while alarms, Pomodoro, daily text, weather-city updates, setup, and Xiaozhi use one lightweight UI notification boundary. Startup budgets, Wi-Fi/NTP ordering, alerts, page return, setup feedback, and user interaction remain unchanged.

Sensor history, OTA, network synchronization, and Wi-Fi radio control now also use only their required internal interfaces instead of importing the complete UI declaration set. Ordinary modules have read-only access to the radio state; only the low-level Wi-Fi owner can publish a state after a real start or stop succeeds, preventing unrelated features from misreporting the status icon or disturbing low-power cleanup. Sensor sampling, firmware updates, network timing, setup, page rendering, and button interaction remain unchanged.

The maintenance workflow now records whether each optimization is awaiting validation, validated, awaiting Gitea synchronization, or synchronized. This prevents maintenance records from getting ahead of the actual repository state and does not change device features, versions, or update behavior.

Background daily-saying cache checks now read only availability and the last successful synchronization time. The complete text is copied only when the Gallery Clock actually renders it; content, same-day caching, next-day refresh, manual refresh, and page appearance are unchanged.

Complete daily-saying reads and updates also avoid an extra internal text copy, reducing transient task-stack use during Gallery Clock rendering and network refresh. Text content, truncation, timestamps, and user interaction remain unchanged.

When an hourly temperature and humidity sample is saved, the device now refreshes it together with the current reading and trend through one consolidated UI notification. Sampling cadence, history charts, trend arrows, and stored data remain unchanged.

Internal battery-ADC and sensor-trend interfaces are now narrower, so every page continues to consume one complete state snapshot. Battery calculation, charging display, low-battery mode, temperature/humidity values, and sampling cadence are unchanged.

The network task's page checks, waits, and NTP retry helpers now remain private implementation details. This maintenance does not change Wi-Fi connections, weather/daily-text synchronization, NTP retry timing, or setup interaction.

The network task still uses one internal top-level runtime state. Its NTP boot request, daily-midnight request, retry deadline, and failure counter form one task-private child state, while startup weather, Daily Saying, deadlines, and resource deferrals form another. Success, failure, disabled pages, current caches, low-power/offline blocking, resource deferral, and setup completion now use the corresponding tested transitions. Existing synchronization timing, retry behavior, network traffic, and user interaction are unchanged.

After a startup weather or Daily Saying refresh has finished, the network task no longer checks unrelated page switches during other network events. Automatic time synchronization with no manual or page request also skips an irrelevant request-cancellation read. Disabling a page still cancels its pending startup refresh, while manual synchronization, visible-page refreshes, midnight time synchronization, and user operation remain unchanged.

The SDL startup-page objects, animation, and screenshot flow are now maintained in a dedicated preview module, with a byte-identical fixed-frame image. This development-tool cleanup does not change the device startup screen, startup timing, or firmware behavior.

The SDL work-page date, battery, sensor summary, and sound/Wi-Fi/alarm icons are now maintained by one shared status-bar module. Thirteen fixed preview states remain byte-identical, so device pages, icons, refresh behavior, power use, and controls are unchanged.

The SDL weather-clock preview now maintains its time, weather, sensor, GIF, alert, low-battery, and setup states in a dedicated module. All 22 fixed preview modes remain byte-identical, so this development-tool cleanup does not change firmware or user operation.

Internal work-page order validation and enabled-page lookup are now maintained by a separate pure policy, while the runtime page catalog remains the sole owner of page names, switches, and user order. Default order, Settings controls, BOOT page switching, disabled-page skipping, offline mode, and NVS data formats are unchanged; no user reconfiguration is required.

The eight page names, Settings mapping, network requirements, and minute-level low-refresh classification now come from one internal page description table. The default page order remains an independently checked user-facing policy. This reduces the chance that a future page is added to Settings but omitted from offline or power-saving rules without tying future default-order changes to page IDs. The current page order, labels, refresh timing, controls, and saved configuration are unchanged.

Weather and Daily Saying data requirements for Weather Clock, Weather Board, and Gallery Clock now also come from that single internal page description table. On-demand visible-page refresh and staggered startup prefetch share the same rule, reducing the risk of unnecessary Wi-Fi activation or missed data after future maintenance. Current synchronization timing, traffic, page switches, offline restrictions, and displayed content are unchanged.

## 12. Safety and Use Restrictions

- Before release, the project runs host tests, sanitizers, SDL pixel regression, an ESP-IDF build, and a real ESP32-S3 core-board smoke test when that optional board is online. The core board does not replace full-device display, audio, sensor, or power validation.
- Development-time optimization scanning prioritizes production firmware, excludes local virtual environments and generated builds, and classifies tests, SDL, tools, and experiments separately. It never runs on the clock itself and does not change pages, controls, networking, storage, or power behavior.
- Never expose Wi-Fi passwords, QWeather keys, Xiaozhi credentials, or private OTA endpoints in public logs or repositories.
- Keep stable power during OTA or full flashing.
- Xiaozhi AI draws much more current and warms the PCB; leave the page or enable auto-return when battery runtime matters.
- This project is for personal learning, research, and non-commercial use only. Commercial use is prohibited. Third-party components remain governed by their own licenses.

Weather location, current conditions, alerts, forecasts, and air quality retain their existing synchronization and cache behavior. Only the internal response-memory placement changed to reduce startup network pressure; no API key, city, page, or network setting needs to be changed.

When a weather or other text endpoint actually returns gzip-compressed content, the short-lived decompression copy now prefers the device's PSRAM. This reduces contiguous internal-memory pressure while HTTPS, Wi-Fi, and display work overlap without changing data, synchronization timing, retries, or page output.

The mobile setup portal, its fields, network validation, and success or failure feedback are unchanged. The firmware only reduces internal-memory pressure while generating those pages; setup steps and user interaction remain the same.

The OTA service now declares a narrower set of internal build dependencies. Update checks, download confirmation, progress display, verification, and restart behavior remain unchanged, with no settings or upgrade-procedure changes required.

Temperature/humidity trend and hourly-history code now uses narrower internal build dependencies. Day/night sampling cadence, the four-hour trend, history charts, hourly storage, and page display remain unchanged.

The Gallery Clock, Temperature/Humidity History, Calendar, Weather Board, and Xiaozhi pages now update their shared sensor summary and sound, Wi-Fi, and alarm indicators only when the underlying state changes. Page content, time display, icon rules, and controls remain unchanged.

The Weather Clock's local sensor values and trend arrows are also refreshed only when sensor data or related display state changes. UI scheduling no longer takes the sensor mutex merely to confirm that data is unchanged; sampling cadence, readings, trend behavior, and appearance remain unchanged.

Alarm status icons now refresh only when the alarm enabled state changes and share one consistent snapshot with the sound and Wi-Fi icons. Alarm time, ringing, replacement confirmation, persistence, and user operation remain unchanged.

When no Pomodoro is running, the Xiaozhi page now skips unnecessary full timer-state reads. Pomodoro timing, display, completion sound, automatic return, and voice controls remain unchanged.

The five-minute Xiaozhi automatic-return check now reads a lightweight session-state and activity-sequence snapshot instead of repeatedly copying subtitle, detail, and emotion text. Voice, subtitles, expressions, Pomodoro behavior, and automatic-return rules are unchanged.

Repeated Xiaozhi subtitle updates within the same second no longer rerun the shared clock and hourly-chime checks. Clock seconds, page changes, time synchronization, and hourly chimes continue to update at the same moments and require no change in operation.

Weather-alert rotation now checks a lightweight internal status and reads only the title currently being shown. Alert timing, multiple-alert rotation, low-battery behavior, and screen appearance are unchanged; newly synchronized alert text can refresh promptly even when the number of alerts stays the same.

The weather board now refreshes its full weather content only when weather data or its waiting state changes. Ordinary minute updates touch only the minute clock and sunrise/sunset countdown, reducing background processing while preserving the same layout, values, network timing, and operation.

The picture clock now reads the daily saying only when that text is updated. Time and picture changes continue normally, while repeated minute and status refreshes no longer copy the same cached sentence.

The Temperature/Humidity History page now avoids rereading its full 48-hour sample cache when neither the stored history nor the current hour has changed. Sampling, hourly storage, chart values, layout, and controls remain unchanged.

The display loop now rereads the complete battery state only when a visible battery value or charging state changes. Battery sampling, charging animation, voltage reporting, low-battery protection, and all displayed values remain unchanged.

The System Info request is now checked through a lightweight internal flag before its complete timeout state is read. Opening System Info, its automatic return, KEY navigation, and OTA status behavior remain unchanged.

The UI now reuses an already converted local-time snapshot within the same second, reducing duplicate work during Xiaozhi subtitle and expression updates. Second-level clocks, network time synchronization, hourly chimes, page rendering, and controls remain unchanged.

Within one visible work-page refresh, the clock, day-progress strip, and temperature-humidity clock date also share one time snapshot. This is an internal efficiency improvement only; displayed time, refresh timing, and controls are unchanged.

The System Info page now refreshes only when displayed Wi-Fi, synchronization, weather, battery, or version text actually changes. In its ordinary state it waits for a button or real data change and still returns exactly when the 30-second timeout expires, without waking once per second. Necessary status checks remain active during OTA. Its content and KEY navigation are unchanged.

The Network Diagnostics page now updates immediately whenever an individual check publishes a result, while avoiding repeated full-page checks during unchanged request waits. Diagnostic items, timeout messages, final results, KEY navigation, and automatic return behavior remain unchanged.

After provisioning succeeds, the device still waits for the phone to display the success result before closing the setup hotspot. This wait is now event-driven instead of repeatedly polled; feedback, timeout, and setup steps are unchanged.

The display driver now reserves SPI DMA descriptors for the actual maximum transfer chunk instead of the full pixel count, reducing persistent internal-memory use. Full and partial refreshes, network protection, OTA progress, and page appearance are unchanged; no user setting needs to be changed.

When a static page has no animation or pending display work, the UI framework pauses its internal periodic display refresh and waits for a real interface event instead of checking an unchanged frame once per second. Data and interaction changes still refresh immediately while the screen remains visible. Second-level clocks, minute clocks, Xiaozhi status, button response, and page pixels remain unchanged, with no setting changes required.

The four-hour temperature and humidity trend cache now uses the device's existing PSRAM, leaving more internal memory available for networking, display, and audio work. Sampling cadence, trend arrows, history charts, page appearance, and operation are unchanged.

The runtime cache for current weather, alerts, forecasts, and air quality now also uses the device's existing PSRAM, leaving more internal memory available during weather synchronization. Weather content, synchronization timing, page appearance, and operation are unchanged.

When the Weather Board performs a complete update of location, current conditions, alerts, the six-day forecast, and air quality, it also reuses one PSRAM working area to reduce the UI task's temporary stack pressure. Page content, minute updates, sunrise/sunset countdowns, network timing, and controls are unchanged.

Weather Board date, temperature, air-quality, humidity, wind, sunrise/sunset, countdown, and alert text now share another page-private external-memory formatting area. This leaves more UI-task stack headroom while preserving the same text, long-alert handling, minute updates, partial refreshes, and network behavior.

When Temperature/Humidity History actually redraws its charts, it reuses one PSRAM working area to reduce the temporary stack pressure from the history snapshot and plotting window. Sensor sampling, 48-hour storage, the 24-hour charts, hourly updates, rendered pixels, and controls are unchanged.

OTA status checks now use a lighter internal read path during display and background activity. The display path no longer copies the complete OTA state for every screen flush merely to decide whether a once-per-minute diagnostic line may be printed. Update checking, confirmation, percentage and speed display, verification, and restart behavior remain unchanged.

Battery percentage, charging, and low-battery checks now use one internally consistent lightweight snapshot. Battery sampling, charging indication, low-battery protection, button response, and all displayed values remain unchanged.

The background sensor scheduler now reuses that lightweight battery snapshot when selecting charging and low-battery wake times. Sampling intervals, charging detection, sensor data, and visible behavior remain unchanged.

The button task and UI task now reuse the same internal snapshot of setup and auxiliary-page activity when selecting idle waits. Button response, debounce, short and long presses, page navigation, and setup behavior remain unchanged.

Work-page visibility now uses only the low-battery and setup mode already confirmed for the same display update, instead of rereading them through an unused legacy entry. Page rendering, weather and daily-text refreshes, and power behavior remain unchanged.

The display task now reuses one consistent internal snapshot of setup, Settings, System Info, and Network Diagnostics state during each screen update. Page visibility, charging animation, Xiaozhi lifecycle, data refresh, and sleep timing keep the same user-visible behavior while avoiding contradictory decisions within one update.

The sound, Wi-Fi, and alarm icons in each work-page header now use one consistent device-state snapshot per refresh. Their position, visibility rules, and controls are unchanged; changing the time of an already enabled alarm no longer causes a refresh with no visible result.

The alarm runtime no longer exposes an internal version field that the UI does not use. Replacement confirmation remains protected by a private generation under the same lock, while setting, changing, disabling, ringing, restart recovery, and Xiaozhi voice control remain unchanged.

The Weather Clock now stores the current custom status-animation frame and its differential baseline in the device's existing PSRAM, leaving more internal memory available for networking, display, and audio work. Built-in and custom animation playback, one-second updates, partial refreshes, and resource-failure fallback are unchanged; no resource upload or setting change is required.

Custom pictures, status animation, weather city, and OTA endpoint data written by the desktop client now use a more internal-memory-efficient resource index. Resource formats, upload steps, priority, appearance, and failure fallback are unchanged; no resource needs to be uploaded or configured again.

The nine network-diagnostics result lines now use the device's existing PSRAM, leaving more internal memory available for HTTPS, display, and audio work during a check. Diagnostic items, order, page content, timeout feedback, automatic return, and controls are unchanged.

The firmware version, download URL, and checksum cached after a successful update check now use the device's existing PSRAM, leaving more internal memory available for the later HTTPS download, display, and flash write. OTA source priority, check results, confirmation, progress, validation, backup source, and reboot behavior are unchanged.

The temporary manifest response used while checking for updates now also prefers the device's PSRAM and automatically falls back to internal memory when needed. Update sources, status messages, version checks, downloads, and validation are unchanged, and no setting needs to be adjusted.

If the primary firmware download fails and the device checks a backup source, that lookup now reuses the separate backup manifest area already reserved in PSRAM instead of creating another complete temporary manifest on the OTA task stack. Source order, version/checksum/size matching, fallback behavior, and user interaction are unchanged.

The daily saying text cache used by the gallery clock now uses the device's existing PSRAM, leaving more internal memory available for networking, display, and audio tasks. Text content, fetch timing, cross-day refresh, manual update, page display, and failure fallback are unchanged.

The internal ownership of work-page time, content, day-progress, and weather-alert updates has been clarified. Page content, pixels, refresh timing, navigation, networking, and power-saving behavior are unchanged, and no setting needs to be adjusted.

The runtime snapshot of saved Wi-Fi credentials, weather API key, and API Host uses the device's existing PSRAM, leaving more internal memory available for networking, display, and audio work. Passwords, full API keys, and the full API Host are not written to the serial log.

When temperature, humidity, and battery sampling fall in the same minute, the display task is now awakened once after both updates are published. Daytime, nighttime, and charging sampling rates are unchanged, as are page data, low-battery protection, and charging indication.

Picture Clock, Weather Board, Calendar, and Temperature/Humidity History use minute-level, change-driven updates while left on screen. They remain on a minute-level wait even when RTC time is temporarily unavailable; a successful network or setup time update wakes the display immediately. Required background work such as button wake-up, local sensor and battery sampling, midnight time synchronization, and alarms still runs on its own schedule; hidden pages and idle network or audio services do not continuously redraw or poll merely because the visible page is static.

If the display task is briefly busy while a button or status update arrives, the firmware retries the pending UI work with a bounded delay instead of waiting for the next minute-level refresh. Normal second/minute refresh timing is unchanged, and a persistent display-lock fault is capped at one retry per second to avoid a busy loop.

If the temperature/humidity sensor cannot be read, the device still retries on the normal one-minute daytime or two-minute nighttime schedule. One transient failure preserves the latest valid reading without adding another I2C transaction; a second consecutive failure changes the display to unavailable. Further failures with no visible state change do not wake the UI, and a successful recovery is shown immediately.

The Picture Clock now updates only the large minute-digit band during an ordinary minute change and adds the hour band only on the hour. Its displayed time, digit style, pictures, and Daily Saying are unchanged.

The shared status bar remembers the last `HH:MM` shown on each non-weather-clock page. Sensor, network, or Xiaozhi updates within the same minute no longer reprocess identical time text; real minute boundaries and recovery from an invalid clock still update normally.

The Weather Clock second display no longer rereads the large weather cache every second. Successful-sync time and forecast completeness are reread only after the weather version changes. Missing-data requests, top-of-hour expiry, retry behavior, and page output are unchanged.

The Weather Clock status panel now only presents cached weather, syncing, or waiting states instead of repeating the same automatic-fetch decision. Missing-data handling, top-of-hour expiry, retry limits, 30-minute backoff, and displayed results are unchanged.

If you leave Weather Clock, Weather Board, or Picture Clock before its queued automatic data refresh starts, the device now cancels that page-only network request instead of powering Wi-Fi after the page is no longer needed. Manual synchronization from Settings, startup refreshes, retries, alarms, sensor sampling, and midnight time synchronization are unchanged.

If leaving the page consumes the final automatic attempt, the normal backoff starts at that moment. Automatic refresh becomes eligible again when the backoff expires instead of remaining stuck in a permanent waiting state.

After a network update finishes, the display is now awakened once by the component that owns that result. Settings feedback, visible weather updates, and Daily Saying publication continue to appear immediately, while redundant wake-ups no longer cause an extra display-loop pass. Synchronization timing, cache content, retries, and controls are unchanged.

Work-page time updates now reuse the chime, low-battery, and setup state already collected for the current display pass. This avoids rereading the same internal state on every second or minute boundary. Clock updates, hourly chimes, low-battery behavior, setup mode, and all visible content are unchanged.

Automatic page data checks now stop at the page boundary: weather refresh state is inspected only while Weather Clock or Weather Board is visible, and Daily Saying state is inspected only while Picture Clock is visible. Cache timing, offline behavior, retries, manual synchronization, startup synchronization, and displayed content are unchanged.

The shared work-page status update now reads weather network events only when Weather Clock needs its waiting or synchronizing message. Other pages no longer inspect an unused weather event snapshot. Wi-Fi, sound and alarm icons, weather updates, and all display timing are unchanged.

The display loop now validates the active page once per frame instead of repeating the same check at the end of ordinary minute, sensor, or battery refreshes. Disabling the current page, switching offline mode, and returning from settings still select a valid page immediately.

The uncharged low-battery minimal screen now keeps a minute-level UI wait even while RTC time is temporarily invalid, instead of waking once per second with no visible work. Charging animation, buttons, sensor and battery sampling, alarms and Pomodoro deadlines, midnight synchronization, and time-recovery notifications continue to run on their existing schedules.

While a non-Xiaozhi page is visible, second- or minute-level display wake-ups no longer resubmit the same Xiaozhi-inactive request. Entering and leaving Xiaozhi, five-minute automatic return, alarm suspension and recovery, Pomodoro, and voice controls remain unchanged.

Across work and auxiliary pages, background Wi-Fi synchronization, audio playback, charging, or update state no longer makes the button task poll every few dozen milliseconds. Settings, Device Info, Network Diagnostics, and provisioning also wait for a GPIO edge while both keys are released. Polling resumes only after a press begins, preserving the same controls while removing idle checks.

Weather synchronization now reuses the device's existing PSRAM workspace while it sequentially resolves the city and retrieves current conditions, alerts, the six-day forecast, and air quality. This leaves more network-task stack headroom for HTTPS, Wi-Fi, display, and audio work. Weather location, request order, cached data, failure fallback, refresh timing, and controls are unchanged; no reconfiguration is required.

The Picture Clock now refreshes only the large time digit that actually changed. Ordinary minute transitions usually update the ones digit, while decimal rollovers and hour changes update both digits when needed. Font shapes, positions, image rotation, Daily Saying, and the one-minute schedule are unchanged.

Update checks now reuse one temporary manifest buffer while falling back among custom, GitHub, and Gitee sources. This reduces heap allocation churn during weak-network fallback without changing source priority, update contents, validation rules, controls, or error messages.

While Settings waits for user input or an OTA state change, its next refresh is now scheduled from one consistent update-status snapshot. Update checks, install confirmation, download progress, status holds, and automatic return behavior are unchanged.

Each Settings refresh now uses one update-status snapshot for both redraw decisions and the OTA progress panel, avoiding a duplicate background read. Text, progress, download speed, and controls are unchanged.

Weather Clock, Weather Board, and Picture Clock now reuse one OTA-state check when deciding whether visible-page data needs a network refresh. Weather and Daily Saying synchronization, cancellation, retries, and backoff behavior are unchanged.

When visible weather data is complete and still current, the device now avoids an unnecessary update-state read during each Weather Clock second refresh. Cached data validity, OTA protection, automatic retries, and all display behavior remain unchanged.

Update-status reads now fall back to a deterministic idle state if their synchronization object is not yet available during an abnormal startup. This prevents random update-state decisions without changing normal update checks, downloads, prompts, or controls.

During startup, the device now checks the weather synchronization time and extended forecast availability from one consistent cache snapshot. Startup refresh timing, weather content, Wi-Fi behavior, and controls are unchanged.

If the internal weather-state lock is temporarily unavailable during an abnormal startup or recovery path, a weather read now safely falls back to empty data instead of retaining stale caller memory. Normal weather synchronization, cached content, page display, and controls are unchanged.

If local sensor or hourly-history state is temporarily unavailable during an abnormal startup, the device now uses safe placeholder data instead of retaining stale temporary content. Normal sampling and display resume under the existing schedule.

Low-level local temperature/humidity writes are now owned only by the sensor sampling service. Pages and Xiaozhi consume the same complete read-only snapshot, preventing unrelated modules from changing values, trends, or availability while preserving the existing cadence, failure grace, arrows, display, and voice queries.

The charging-battery animation and second-level page refresh now share one second-boundary calculation, reducing duplicate background work without changing animation timing, clock display, or minute-level idle behavior.

Local sensor and uncharged-battery scheduling now share one aligned minute-boundary calculation when regular sampling is reset. A battery already confirmed as charging keeps its separate roughly one-second cadence. Daytime, nighttime, and charging sample intervals remain unchanged.

The uncharged battery and local sensor now also share the same next-sample rule after each reading, preventing their maintenance paths from drifting apart. The visible daytime, nighttime, and charging cadences are unchanged.

When weather and Daily Saying requests are both queued in low-battery mode, the device now completes both blocked requests in one background pass. Low-battery protection, network restrictions, and user messages are unchanged.

After a missing weather API key or low-battery mode blocks weather or Daily Saying, the device now continues any permitted time synchronization in the same pass, or sleeps until the next real deadline instead of adding a fixed one-second wake. Messages, low-battery network restrictions, manual time synchronization, and page controls are unchanged.

When the single-use alarm is disabled, its background task now returns directly to sleep after a state or time notification instead of performing an unused clock conversion. Setting, changing, disabling, ringing, and Xiaozhi voice control remain unchanged.

The device still fully validates custom GIF, gallery, and configuration assets written by the desktop app during startup. Large packages are now checked with fewer small Flash reads; damaged packages still fall back safely, with no change to page content, image rotation, or user controls.

The temporary provisioning DNS service now reuses one bounded packet buffer while answering captive-portal requests. This leaves more internal memory available during setup without changing automatic portal discovery, the `192.168.4.1` fallback address, validation feedback, or any user action.

Hourly chimes, sound previews, and the provisioning prompt now use a smaller bounded playback block, leaving 2 KiB more internal memory available throughout normal operation. Audio samples, volume, fade behavior, prompt duration, stop controls, and Xiaozhi voice playback remain unchanged.

The provisioning page now keeps its temporary Wi-Fi scan list in PSRAM when available, with an automatic internal-memory fallback. This leaves up to about 2.9 KiB more internal memory available while the hotspot, web page, display, and setup prompt are active; the visible network list and setup steps are unchanged.

NTP now runs only for the existing startup, midnight, diagnostic, or manual synchronization requests. The client stops after each success or timeout instead of retaining an hourly background poll; time servers, retries, RTC updates, alarms, and displayed time are unchanged.

When the Daily Saying service repeatedly returns network or response errors, the device now ends that synchronization pass after three consecutive failures and turns Wi-Fi off instead of spending all eight HTTPS attempts. The existing eight-attempt allowance remains available when valid sayings are merely too long; cached text, visible-page refreshes, and manual updates are unchanged.

If a device shows “voice listener initialization failed” when entering the Xiaozhi page, the existing automatic retry behavior remains active. The retry window now immediately returns to the wake-listening power profile instead of holding realtime session power, without changing an hourly chime, alarm, or setup prompt that currently owns shared audio. Serial diagnostics identify audio ownership, processed-stream, or WakeNet pipeline failures so temporary contention, a missing model partition, and audio hardware faults can be distinguished without changing page controls or saved configuration.

Automatic weather location still checks the current IP coordinates whenever a weather synchronization is needed. When the coordinates are unchanged, the device reuses a short-lived city-resolution result to avoid one duplicate network request. A location change, manual city selection, restart, or cache expiry automatically restores the full lookup, with no setting change required.

After new Wi-Fi details are saved in the provisioning page, the device now finishes the previous router connection and clears its old IP state before validating the new connection. If the driver cannot disconnect cleanly, provisioning reports a failure and keeps the setup hotspot available instead of mistaking the old network for a successful new configuration. Form fields, timeout, and success feedback are unchanged.

When saved Wi-Fi details are cleared while the device is running and setup is entered, the old router connection is likewise cleared only after the driver confirms that it disconnected. In the rare event of a driver failure, the provisioning page remains available while the status bar continues to show the real Wi-Fi state instead of hiding a connection that may still consume power.

Project maintenance validation now includes production-code static analysis with the same ESP32-S3 target toolchain used for firmware builds. This internal quality gate helps detect potential pointer, resource-lifetime, and error-path defects before changes are committed; it does not alter device features, UI, settings, networking, refresh behavior, power policy, or user workflows.

The same maintenance gate now checks per-function static stack frames for all project-owned production sources. Oversized frames and unbounded dynamic stack use are rejected before submission, helping prevent future internal-memory regressions without changing firmware behavior, task sizes, pages, networking, audio, or stored settings.

Compiler-report parsing and memory-budget checks in the maintenance tooling now live in a dedicated side-effect-free module, while the existing entry point still coordinates builds, logs, and commits. This internal organization does not change firmware, validation thresholds, pages, networking, power behavior, or user controls.

Optimization-log formatting, section parsing, and release-archive conversion now also live in a dedicated side-effect-free module, while the existing workflow remains the only owner of files, Git operations, validation, and Gitea synchronization. This internal maintenance cleanup does not change firmware, versions, release data, pages, networking, power behavior, or user controls.

The maintenance workflow now checks every date-based optimization log whenever it records an item and restores missing main-index links in reverse chronological order. This documentation-only safeguard does not change firmware, version numbers, UI, settings, networking, power behavior, or user controls.

Each UI implementation now includes only the internal contracts it actually uses, reducing the chance that one shared maintenance change affects unrelated pages. This internal cleanup does not change any of the eight work pages, Settings, the status bar, refresh timing, networking behavior, stored configuration, or user controls.

The shared work-page status bar and the Gallery, Sensor History, Calendar, and Weather Board pages now use explicit internal interfaces. This prevents unrelated dependencies from leaking into page maintenance without changing content, layout, icons, partial refresh behavior, sensor data, networking, or controls.

All production firmware modules now include only the internal contracts they actually need instead of relying on a historical aggregate header. This maintenance change does not alter startup, task scheduling, NVS data, pages, networking, audio, power policy, or user workflows.

Application startup, button handling, Weather Clock runtime updates, weather placeholder text, and Settings actions now also use their own explicit internal interfaces. This maintenance-only cleanup reduces future cross-module impact without changing the UI, refresh timing, controls, networking, stored configuration, audio, or user workflows.
If RTC time is lost while the network is unavailable, the device first retries time synchronization quickly and then gradually backs off to no more than once every five minutes. When RTC time remains valid but a scheduled NTP request repeatedly fails, retries widen through 5, 10, 20, 40, and 60 minutes, then remain capped at once per hour. This prevents Wi-Fi from repeatedly powering up while the clock can continue locally. Time synchronization resumes automatically when the network recovers, and a manual time sync from Settings / Network still runs immediately.

If a weather or daily-message startup refresh has not started yet and you disable its page in Settings, the device now cancels that page-only refresh before using Wi-Fi. Manual synchronization, provisioning validation, time synchronization, and normal refresh after re-enabling the page are unchanged.

If a battery sample briefly fails while charging, the device keeps the most recent valid battery state and retries quickly for a short bounded period. Normal display resumes automatically after recovery; persistent failures eventually fall back to an unknown battery reading instead of keeping the charging animation or high-rate sampling indefinitely.

Local temperature and humidity continue to use the existing day and night sampling schedule. When the values, trends, and availability are unchanged, the device skips an unnecessary UI notification; hourly history samples still update on time, with no change to the history chart or displayed data.

When a manual weather city is configured, the device temporarily remembers that city's resolved weather location. If the city is unchanged, later hourly weather updates avoid one repeated location lookup and may keep Wi-Fi active for less time. Changing the city, returning to automatic location, restarting, or cache expiry automatically performs a fresh lookup.

During the first minute after startup, an automatic weather update may occasionally pause if internal display and network memory is temporarily busy. Repeated pauses now wait progressively longer, up to one minute, instead of reconnecting Wi-Fi every ten seconds. The first retry remains quick, and normal weather updates, manual synchronization, page controls, and cached data are unchanged.

On minute-level pages, charging no longer keeps the interface waking every second after the battery animation has stopped or become hidden. The icon still blinks at the same rate while visible, and battery percentage, charging changes, and unplug detection still update immediately.

The low-battery screen follows the same rule: charging animation remains unchanged while visible, but charging alone no longer keeps the interface on a one-second refresh after the animation is hidden. Fast charging checks and automatic exit from low-battery mode continue normally.

Compact OTA notes now use one shared rule for both full-details and omitted-item footers, preventing different publishing paths from producing inconsistent messages. Update checks, version selection, download URLs, validation, and user controls are unchanged.

Release verification now uses one shared contract for the OTA and full-flash firmware names, hashes, and sizes, reducing field drift between publishing targets. Device-side update checks, downloads, validation, fallback sources, and user workflows are unchanged.

The primary and fallback OTA repositories now share the same firmware hash and size validation rules, so malformed manifests are rejected consistently before publication. Normal update checks, downloads, verification, and controls are unchanged.

The alarm task now sleeps on a notification between repeated rings instead of waking at a fixed short interval. Any button, Xiaozhi alarm command, or Settings action still stops it immediately; the selected sound, five-second repeat gap, and one-minute maximum duration are unchanged.

If the interface is rebuilt while the gallery image and current minute have not changed, the Image Clock now redraws both the picture and large time immediately. Normal minute-level partial refreshes, image rotation, Daily Saying content, and saved settings are unchanged.

The maintenance workflow can now complete isolated validation and Gitea records while preserving uncommitted user previews and test measurements. Those local files are not added to maintenance commits. This change affects project maintenance only and does not alter device features, UI, configuration, or operation.

Every routine code-optimization validation now runs both the normal host suite and ASan/UBSan automatically. This catches memory-access and undefined-behavior regressions before firmware compilation and submission; it does not change device features, UI, configuration, or operation.

If an automatic startup weather request is postponed by temporary memory pressure, an earlier time-synchronization deadline can now wake the network task first. Weather retry timing, Wi-Fi behavior, manual synchronization, cached data, and normal user operation remain unchanged.

The project maintenance tools can inspect production code, tooling, and tests as separate optimization scopes. Even a repository-wide scan no longer reports Python virtual environments, HIL build output, SDL, or host tests as production firmware. Classification labels and machine-readable JSON output remain protected by tests. This improves maintenance accuracy only and does not change device features, UI, configuration, networking, or operation.

Loading the saved temperature and humidity history at startup now uses substantially less task-stack memory. Saved 48-slot history, migration from the older 24-slot format, sampling cadence, trend arrows, charts, and all controls remain unchanged.

Firmware-update redirect URLs now use a managed external-memory workspace, leaving more task-stack headroom for networking, verification, and Flash writing. Update sources, checks, progress, integrity validation, fallback behavior, and user steps are unchanged.

Update checks and installation now keep the primary and fallback firmware manifests in a private external-memory workspace instead of stacking both copies in the update task. Source priority, version checks, fallback matching, download validation, progress, and reboot behavior are unchanged.

Daily weather forecasts now use less network-task stack memory while retaining the same transactional cache protection. Seven-day forecasts, three-day fallback, Weather Board content, refresh timing, and saved settings are unchanged.

Weather-alert processing now uses less network-task stack memory while still replacing the previous alert cache only after the complete response has been processed. Alert ordering, compact titles, timestamps, page display, refresh timing, and saved settings are unchanged.

QWeather city lookup, current conditions, alerts, daily forecasts, and air quality now share one verified request-address capacity, reducing network-task stack use without changing accepted city names, endpoints, encoding, weather data, refresh timing, or user controls.

The Settings page now keeps its temporary menu text in a dedicated external-memory workspace, leaving more UI-task stack headroom. Menu labels, page switches, page order, update status, layout, and controls are unchanged.

The provisioning DNS service now keeps its temporary request and response packet in the device's external memory, leaving more task-stack headroom while the hotspot and setup page are active. Automatic portal opening, manual access, validation feedback, timeouts, and setup controls are unchanged.

The Network Diagnostics page now keeps its temporary nine-line display snapshot in external memory, leaving more UI-task stack headroom while a check is running. Diagnostic items, live progress, timeout messages, layout, automatic return, and controls are unchanged.

The provisioning service now keeps the submitted POST body or compatibility GET query in one private external-memory workspace, leaving more HTTP-task stack headroom during validation. Form fields, length checks, error feedback, validation, and hotspot behavior are unchanged.

The setup form and connection-result page now reuse a private external-memory text workspace while preparing SSID, city, hotspot-name, and feedback text. Page content, Chinese text escaping, Wi-Fi scanning, validation results, and setup steps are unchanged.

Startup validation of custom GIF, gallery, city, and update-address resources now uses less task-stack memory. Resource integrity checks, accepted formats, fallback behavior, page content, and all user steps remain unchanged.

When the device starts, migration of an older 24-hour temperature and humidity history now uses temporary external memory only when that legacy data is actually needed. Saved history compatibility, sampling, charts, and normal startup behavior are unchanged.

Startup restoration of Wi-Fi, weather, page, and reminder settings now uses a temporary external-memory workspace that is wiped after the live settings have been published. Saved values, defaults, offline behavior, and all user controls are unchanged.

The Settings and System Information pages now use a smaller consistent OTA timing snapshot when they only need to schedule their next update. OTA messages, percentage, speed, confirmation timeout, download display, and controls are unchanged.

During update checks and firmware downloads, the low-frequency temperature, humidity, and battery scheduler now sleeps until the update state changes instead of waking every five seconds. Normal sampling intervals are unchanged, and successful network or manual time correction only realigns the next scheduled sample.

Network time synchronization now waits directly for the completion event instead of waking once per second while the time server is unavailable. The normal success response and the existing 30-second maximum timeout are unchanged.

Device startup and Wi-Fi connection read only the credentials needed for each operation: the driver receives one consistent SSID/password pair, the startup screen reads only the SSID, and weather requests read one consistent API Key/API Host pair.

Saving or restoring network settings now updates the protected external-memory credential state as one transaction without creating another full task-stack copy. Replacing a longer value with a shorter one also clears the unused tail. Configuration formats, saved results, reconnection, and user controls are unchanged.

During setup, the QWeather API-key probe now reuses a dedicated external-memory result buffer, leaving more task-stack headroom for networking and HTTPS. The fixed probe city, optional manual-city validation, success or failure feedback, hotspot retention, and user steps are unchanged.

QWeather requests now keep their temporary request address inside the same external-memory allocation as that request's response. This leaves more task-stack headroom without adding another allocation, and simultaneous background weather updates and Xiaozhi city validation retain separate buffers. Weather content, refresh timing, setup, and user controls are unchanged.

The setup service parses the submitted Wi-Fi name, password, QWeather API Key, API Host, and optional weather city in a private external-memory workspace. That workspace is securely cleared after every success or failure path.

The device now copies the saved Wi-Fi name and password directly into the wireless-driver configuration during one protected read, reducing internal-memory use while networking starts. Existing settings, setup, open and WPA2 networks, automatic reconnection, and weather synchronization are unchanged; no reconfiguration is required.

The most recent weather-update time shown in System Info now comes from the same internal cache snapshot as weather-board completeness, preventing metadata from different update generations from being combined at a synchronization boundary. Display content, weather refreshes, caching, and controls are unchanged.

Weather, location, daily saying, and update-manifest requests now keep the ESP-IDF request descriptor in a protected external-memory workspace. This leaves more internal task-stack headroom when Wi-Fi, TLS, and response parsing overlap. Network order, certificates, timeouts, displayed data, and controls are unchanged.

Firmware validation now also checks the final linked internal-data total and reports the largest internal-memory objects. This helps prevent future maintenance changes from silently reducing the memory headroom needed by Wi-Fi, secure connections, display transfers, and audio. Device features, UI, settings, protocols, and user steps are unchanged.

The maintenance record now receives the final firmware memory totals directly from the same verified analysis result, reducing manual transcription errors. This affects project validation only and does not change firmware behavior or user operation.

The Settings screen now renders related text, switch indicators, and the visible page-order list from one consistent state snapshot. Page ordering no longer reloads the page-enable mask for every item or repeatedly normalizes and rewrites an unchanged order during ordinary redraws. Page management still skips temporary menu text that would be immediately replaced. This reduces duplicate reads and locked writes while preserving all visible text, layouts, controls, timeouts, and saved settings.

When deciding whether startup weather and Daily Saying data should be prefetched, the device now reads the current page switches once and evaluates them as one consistent snapshot. Disabling the related pages no longer risks retaining an unnecessary prefetch from mixed switch states. Manual synchronization, on-entry refreshes, time synchronization, and all controls are unchanged.

Each page-switch action in Display Settings now uses the same validated selection and navigation state for validation, saving, rollback, and result feedback. An invalid selection stops with a save-failed message instead of falling back to Weather Clock. Available pages, offline restrictions, Xiaozhi Pomodoro protection, ordering, and controls are unchanged.

At startup, the device now derives both online and offline page availability from the same loaded configuration instead of rereading an intermediate global state. The default home page, page order, offline restrictions, and existing settings are unchanged.

Each page-order swap is now validated in a temporary copy and published to the running device only after storage succeeds. A failed save leaves the original order active without applying and rolling back a temporary order. KEY exit and the 30-second timeout also avoid resaving swaps that were already committed. Page names, feedback, ordering controls, restart restoration, and button behavior are unchanged.

Opening Network Diagnostics or About Device, and entering provisioning after Factory Reset, now keeps the navigation state from the same BOOT action through the complete transition. Rapid button input can no longer mix a later Settings focus into the current action. Confirmation steps, saved data, screen content, return targets, and controls are unchanged.

Before the Settings screen performs its 30-second inactivity return, it now confirms that no button or status activity arrived while the timeout was being evaluated. Activity at that boundary keeps the Settings screen open and restarts the existing timeout from the latest operation. The timeout duration, controls, saved page order, and return behavior are unchanged.

Network Diagnostics now starts its 30-second result-display period only after all checks have finished. A long KEY press or another Settings activity at the timeout boundary takes priority and cancels that automatic return. The checks, layout, Wi-Fi behavior, and controls are unchanged.

Entering page-order mode now reuses the already normalized first-enabled-page lookup instead of processing the same order twice. Page switches, the Xiaozhi home-page restriction, ordering display, and button controls are unchanged.

Before powering on Wi-Fi, the device now confirms that every request in the current network-work snapshot is still active. If a Settings sync has already timed out, the user has left a page that requested missing data, or another state has canceled the request, Wi-Fi will not be started late for that stale work. Other valid or newly arrived requests continue normally. Sync controls, messages, weather, Daily Saying, time synchronization, and user operation are unchanged.

When complete weather data for the current hour is already available and no retry state is pending, the Weather Clock now finishes its once-per-second network-need check earlier instead of rereading weather configuration and runtime gates. Hour changes, missing data, incomplete Weather Board details, offline mode, and configuration changes still follow the existing rules. Display content, weather refresh timing, and controls are unchanged.

When today's Daily Saying is already available and no retry state is pending, the Gallery Clock now reuses lightweight cache metadata and finishes its network-need check earlier. Day changes, missing text, manual updates, offline mode, and weak-network retries still follow the existing rules. Images, displayed text, switching intervals, and controls are unchanged.

Page switching now automatically covers every registered work page and Settings auxiliary page so that only the selected page remains visible. Page order, controls, content, and refresh behavior are unchanged.

The four fixed text buffers used for Xiaozhi reply subtitles now reside in the device's existing PSRAM, leaving 768 bytes more internal memory available when voice, networking, and display work overlap. Subtitle content, reveal timing, line cropping, page output, and voice interaction are unchanged.

The shared status-bar, battery-icon, and Weather Board forecast-card reference tables for all eight work pages now reside in the device's existing PSRAM. This preserves more internal memory when Wi-Fi, secure connections, display transfers, and audio overlap. Page content, layout, partial refreshes, status icons, battery display, and weather data are unchanged, and no setting change is required.

Additional internal object-reference tables used by work pages, Settings auxiliary pages, the Temperature and Humidity Clock, and Temperature and Humidity History now also reside in the device's existing PSRAM. Page order, navigation, history charts, second-level clock updates, layouts, and controls are unchanged.

Low-frequency object-reference tables used by Settings, Network Diagnostics, System Info, setup status, and the all-day progress bars now also reside in the device's existing PSRAM. This preserves more internal memory for networking, secure connections, display transfers, and audio while leaving every screen, message, control, setup flow, and refresh rule unchanged.

The Weather Clock's weather panel, local sensor area, top status bar, and main canvas object references now also reside in the device's existing PSRAM. This preserves more internal memory for weather networking, display transfers, and audio while leaving its content, icons, second-level updates, low-battery view, navigation, and controls unchanged.

The Weather Board's city, current conditions, air quality, humidity, wind, sunrise/sunset, alert, and advice object references are now grouped in one page-owned table in the device's existing PSRAM. This leaves additional internal memory available when display, secure networking, and audio overlap. Weather content, layout, minute updates, partial refreshes, and controls are unchanged.

The fixed captive-portal address advertised during setup now uses an exactly sized buffer in the device's existing PSRAM. Setup Wi-Fi, automatic browser redirection, validation feedback, and all user steps are unchanged.

The short text buffer used for Settings feedback now resides in the device's existing PSRAM. Feedback wording, duration, button response, network checks, and manual synchronization behavior are unchanged.

The setup hotspot name and local IP text snapshots now reside in the device's existing PSRAM. Hotspot discovery, setup-page text, connection validation, feedback, and all setup steps are unchanged.

Manual weather-city text and the pending city submitted by Xiaozhi now reside in external RAM, while synchronization and pending-generation controls remain in internal RAM. City input, validation, automatic-location restore, saving, and weather refresh behavior are unchanged.

OTA status text now resides in external RAM while update state, progress, speed, timeout, reboot, synchronization, and atomic controls remain in internal RAM. Update checks, confirmation, downloading, progress display, result messages, and reboot behavior are unchanged.

The Weather Board now keeps its weather-version, minute, waiting-state, and sunrise/sunset refresh keys in one page-private PSRAM cache. Full weather updates and minute-only sunrise/sunset countdown updates follow the same rules as before, with no change to the page, network use, controls, or displayed information.

The display refresh callback now keeps its partial-refresh ranges and low-frequency diagnostic counters in one LVGL-task-owned PSRAM state. Pixel conversion, partial and full refresh decisions, OTA display quieting, screen output, and all visible behavior are unchanged.

The Gallery Clock now keeps its canvas and label references, draw keys, and reusable-buffer references in one page-owned PSRAM state. This leaves another 40 bytes of internal RAM available when display, secure networking, and audio work overlap. Images, large time digits, Daily Saying text, image rotation, partial refreshes, and all controls are unchanged.

The weather, networking, sensor, page, alarm, and related runtime states required during startup are now initialized through two internal catalogs in their original order. A failed critical initializer still records its original error and stops subsequent startup immediately. Boot order, pages, networking, audio, NVS data, and controls are unchanged.

The Settings screen now uses one low-frequency render cache to compare menu selection, page-management modes, and OTA progress, and resets that cache whenever the page is rebuilt. Layout, text, switches, buttons, automatic return, and update display behavior are unchanged.

The Temperature and Humidity Clock build and second-level runtime paths now include only the display interfaces they actually use, reducing unrelated maintenance coupling. Time, lunar date, sensor values, trend arrows, comfort icons, layout, and refresh timing are unchanged.

The device now explicitly checks that weather business-code fields and Xiaozhi MCP tool parameters exist and have the expected JSON type before reading them. Missing fields, wrong types, or non-object parameters follow the existing failure response without changing weather cache, volume, alarm, pomodoro, or weather-city state. Normal weather and Xiaozhi controls are unchanged.

Daily forecast responses are now accepted only when the service returns the expected JSON array. Each parse starts from a clean temporary forecast, so a malformed response cannot append to stale days or preserve stale advice. The last valid on-screen weather remains available when a new request fails; forecast content, refresh timing, and controls are unchanged.

Current-weather and air-quality fields are now committed only after every required value in that response has been validated. A missing or incorrectly typed field cannot leave a mixture of new and stale values. City, coordinates, timestamps, refresh timing, and all user-visible behavior are unchanged.

Weather-city lookup results are now committed only after both the Location ID and city name are valid. A malformed service response cannot leave a partially updated city result. Manual city selection, automatic location, setup validation, weather content, and controls are unchanged.

Weather alerts now replace the cached alert list only after the service returns a successful business code and a valid alert array. A temporary HTTP or service-response failure keeps the last valid alerts for the same location, while changing location still removes alerts from the previous city. A successful empty alert list continues to clear current alerts normally.

Daily Saying responses that contain several compatible text fields now skip fields containing only whitespace and continue to the next valid field, including nested response objects. The existing field priority, 22-character limit, retry behavior, refresh timing, and Gallery Clock display remain unchanged.

OTA manifests with an overlong version, firmware URL, or SHA256 value are now rejected instead of being silently truncated. The device then follows the existing source fallback order, preventing a malformed manifest from reaching version comparison or firmware verification. Valid manifests, update controls, source priority, and online JSON format are unchanged.

If a weather, location, Daily Saying, network-diagnostics, or update-manifest service returns an abnormally large response that cannot fit in the device's fixed receive buffer, the incomplete response is discarded instead of being displayed or saved. Existing weather and Daily Saying caches remain available under the original rules, and later visible-page, manual-sync, or backoff retries continue normally. No setting change is required.

When weather data is complete and still current, each Weather Clock second refresh now uses a lightweight state check instead of entering shared network-event synchronization. Weather display, hourly refreshes, retry behavior, offline mode, and user controls are unchanged.

After an OTA image passes its SHA256 check, the device now also verifies that the version embedded in the image matches the manifest and that the image belongs to the `weather_clock` project. A mismatched image is rejected before the boot partition is changed, and the existing backup-source path remains available. Normal updates, manifest fields, progress display, and user operation are unchanged.

When manual date and time are saved for offline setup, the result page is now allowed to finish sending before the network task closes the setup portal and Wi-Fi. This avoids a rare setup-service stall while keeping the same offline time, RTC, settings, and user steps.

If the setup web service encounters a transient shutdown failure, the device now preserves the real portal state and continues cleanup in the background instead of reporting a false stop or leaving display resources reserved. Normal setup, result feedback, and re-entering setup work the same way.

If the setup DNS socket encounters an unexpected permanent receive error, the background DNS task now stops and releases the socket instead of repeatedly consuming CPU. Normal captive-portal timeouts, browser redirection, manual access to `192.168.4.1`, and setup steps are unchanged.

The local optimization workflow now keeps its Git/Gitea handling in a dedicated maintenance module. This does not change firmware behavior, device setup, versioning, or release access; it only reduces duplicated automation responsibilities and keeps repository credentials out of command arguments.

The device now rechecks power-management initialization before normal background services start. If temporary resource pressure prevents a sleep or network/audio protection lock from being created early in boot, only the missing resource is retried. Pages, networking, audio, and controls are unchanged.

The primary and backup OTA manifest buffers now use one dedicated PSRAM lifecycle module. This internal cleanup keeps update metadata out of the OTA task stack and reliably clears both buffers after each operation. Update checks, source fallback, download progress, verification, and restart behavior are unchanged.

Wi-Fi reconnect handling now rejects deliberate shutdown, an already stopped radio, and missing credentials before querying the driver, then checks shutdown ownership again immediately before reconnecting. Unexpected router disconnects still reconnect automatically; setup, OTA, Xiaozhi, icons, and user controls are unchanged.

While the setup hotspot is open, weather, time, Daily Saying, and network-diagnostic requests now remain queued instead of interrupting the phone's setup connection. They continue automatically after setup closes. Setup fields, validation feedback, and normal synchronization behavior are unchanged.

If saving the automatic disable state of a single-use alarm fails because NVS is temporarily unavailable, the alarm service now retries in the background with progressively longer intervals. This prevents an alarm that has already rung from being restored after a later restart. Alarm time, sound, button stop, and Xiaozhi voice controls are unchanged.

After a manual time, weather, Daily Saying, or network-diagnostics operation, the device now retires the previous network request before accepting another operation. Rapid repeated use can no longer leave a new request stuck in the syncing state because an older completion cleared it. Synchronization content, timeout duration, and controls are unchanged.

The local optimization tool now rejects file inputs outside the repository, preventing maintenance validation or records from accidentally targeting unrelated files on the computer. This does not change device firmware, versioning, pages, networking, settings, or user operation.

Settings activity, the 30-second automatic return, and network-diagnostics result retention now coordinate through a task-level mutex instead of consuming processor time in a cross-task busy wait. Settings content, controls, timeout duration, and page output are unchanged.

The device now confirms that startup-page cleanup and construction of the first work page have completed before normal background tasks begin. A temporarily busy display lock is retried automatically. The normal startup screen, network order, and controls are unchanged, while this guard prevents a rare mixed startup/work-page display state.

If startup networking or animation takes longer than expected under an exceptional condition, the device now waits for the temporary task to exit and release its network, display, and memory resources before starting normal services. Normal startup speed and pages are unchanged; this guard reduces startup resource contention under weak-network or low-memory conditions.

Startup task ownership is now managed in one place before permanent services are created. This internal maintenance removes a duplicate network wait without changing startup timing, weather refreshes, Daily Saying refreshes, settings, or user operation.

Before a queued network diagnostic powers Wi-Fi, the device now checks the latest offline, battery, OTA, and setup state again. If conditions changed while the request was waiting, the diagnostic is deferred or receives the existing status feedback without opening an unnecessary network session. Normal diagnostic items, order, results, and timeouts are unchanged.

The Wi-Fi connection stage of network diagnostics now also reacts immediately to offline mode, low battery, OTA, or setup-mode changes. The device releases the current network session instead of waiting for the full 45-second connection timeout, while preserving the diagnostic request for the existing retry or status path. Normal successful checks and weak-network timeout behavior are unchanged.

Weather, time, Daily Saying, and network diagnostics now share the same Wi-Fi connection validation. This internal consistency keeps offline, low-battery, OTA, and setup-mode transitions handled uniformly. Connection limits, synchronization content, diagnostic results, and controls are unchanged.

Page switches, Xiaozhi power saving, and Gallery Clock rotation now share one internal persistence transaction. Device storage is committed only when a setting value actually changes. Settings, defaults, controls, failure handling, and restored values after restart are unchanged.

Settings feedback and log text now have a single internal source of truth. This maintenance cleanup does not change any displayed wording, feedback duration, button action, saved setting, or device behavior.

Fixed text used by startup, settings, status, and work pages now follows the same single-source maintenance rule. This internal cleanup does not change any screen text, logs, page layout, refresh timing, controls, networking, or saved data.

Network diagnostics and IP-based weather location text now follow the same internal single-source rule. Diagnostic targets, results, city parsing, network timing, power behavior, and user controls are unchanged.

The local maintenance workflow now keeps ESP-IDF compile-database parsing and Xtensa analysis-command construction in one dedicated module. This internal tooling change does not alter firmware, pages, power behavior, settings, OTA, or device operation.

Multi-step weather and Daily Saying synchronization now rechecks offline mode, low battery, OTA activity, and setup mode between network requests. If the operating state changes, the device finishes the request already in progress safely but does not start another high-power request. Normal synchronization, cached data, retry limits, and controls are unchanged.

The short memory-settling gaps between time, weather, Daily Saying, individual weather requests, and Daily Saying retries now react immediately when the device enters Offline Mode, low-battery protection, OTA, or setup. Normal synchronization keeps the same request order, retry limits, and settling times, while a changed operating state can release Wi-Fi power resources sooner.

An active NTP wait now stops early when the device enters Offline Mode, low-battery mode, OTA, or setup. Unfinished synchronization or network-diagnostics work is returned to the next matching state pass instead of continuing with later high-power requests. Normal time synchronization, the daily midnight schedule, RTC updates, manual synchronization, and retry behavior are unchanged.

While an OTA check or installation is waiting for Wi-Fi, entering Offline Mode, low-battery protection, or setup mode now ends that network attempt immediately instead of keeping Wi-Fi awake for the full connection timeout. Normal update checks, downloads, verification, backup sources, and restart behavior are unchanged.

The Wi-Fi connection and power-lock lifecycle used by update checks and installations is now managed by one internal session, ensuring network resources are released reliably after an interrupted attempt. Update sources, progress, verification, fallback behavior, restart, and controls are unchanged.

Daily weather forecasts no longer trigger an immediate second forecast request after a network timeout, memory shortage, malformed response, or service rate limit. Existing weather remains visible and the normal retry policy handles recovery. A single 3-day compatibility request is used only when the service explicitly reports that access to the 7-day endpoint is restricted. Normal forecast content, hourly synchronization, and controls are unchanged.

During automatic IP-based positioning, the device still tries the city name once when the coordinates explicitly have no matching city. Network timeouts, authorization failures, rate limits, and malformed responses no longer trigger a duplicate city lookup; existing cache and retry behavior handle recovery. Manual city settings, displayed weather, and synchronization timing are unchanged.

If Daily Saying synchronization encounters a local memory, argument, or runtime-state error that cannot recover within the same attempt, the device now ends that network batch and uses the existing backoff before trying again. Temporary network failures still follow the original bounded retry rules, while cached text, the 22-character display limit, refresh timing, and controls remain unchanged.

During an OTA installation, the device now tries the backup source only when a connection, transfer, incomplete-response, or checksum failure may be resolved by changing download sources. Local memory, OTA partition, Flash-write, final image-validation, or boot-partition failures stop the current attempt instead of downloading the same firmware again. Update sources, progress, verification, restart behavior, and controls are unchanged.

While weather, time, Daily Saying, or network diagnostics waits for the router, a stale internal state notification no longer closes and immediately reopens Wi-Fi when the live operating conditions are unchanged. The device continues within the remaining part of the same 45-second connection budget. Entering Offline Mode, low-battery protection, OTA, or setup, changing credentials, or cancelling the request still ends the attempt immediately. Synchronization content, timeout limits, failure feedback, and controls are unchanged.

If the Xiaozhi page is left while it is waiting for Wi-Fi, or an alarm suspends Xiaozhi, the device now ends that connection wait immediately and releases Xiaozhi network and voice resources. Other pages can return to their normal low-power state without waiting for the full 30-second connection timeout. Normal Xiaozhi startup, wake-word, and conversation behavior are unchanged.

Local temperature and humidity reads now return deterministic safe values if the shared sensor state is temporarily unavailable during startup. This prevents stale caller data from appearing in an exceptional path; normal sampling, four-hour trends, history, and display behavior are unchanged.

If an OTA source fails while its manifest is being read, the device now discards that source's incomplete metadata before trying the next source. If every source fails, no partial manifest is retained. Custom-source priority, GitHub/Gitee fallback order, update controls, and verification remain unchanged.

The local maintenance workflow now runs regular and memory-safety tests in isolated directories and in parallel, reducing validation time. This tooling-only change does not alter device features, UI, power behavior, settings, or update procedures.

When the device deliberately turns Wi-Fi off, automatic reconnect is now suppressed throughout setup-portal teardown. This avoids an occasional redundant connection and brief extra power draw during shutdown. Normal disconnect recovery, setup feedback, weather synchronization, Xiaozhi, and OTA behavior remain unchanged.

Display memory protection now retains every overlapping network, setup, and OTA owner instead of using the previous small nesting limit. This prevents an exceptional high-concurrency window from restoring larger display transfers too early. Screen content, refresh behavior, and controls are unchanged.

Network synchronization now uses one internal runtime snapshot for credentials, Offline Mode, low-battery state, OTA, and setup transitions before opening a network window. This maintenance change keeps the existing synchronization schedule, retry limits, Wi-Fi behavior, and controls unchanged.

Weather, time, Daily Saying, and network diagnostics now share one internal owner for the complete Wi-Fi connection wait, including the fixed deadline and live request validation. This maintenance cleanup does not change the 45-second limit, synchronization behavior, power policy, feedback, or controls.

Network diagnostics now runs IP geolocation and DNS in the same order shown on the page and publishes each result immediately. If Offline Mode, low-battery protection, OTA, or setup begins during the checks, the device does not start the next network operation. The nine checks, timeout feedback, and controls are unchanged.

Xiaozhi face animation, temperature/humidity history charts, calendar graphics, and the inverted trend arrows now write each pixel batch directly into the display buffer before requesting one refresh. This reduces repeated UI scheduling and dirty-area merging while preserving every pixel, animation interval, date, trend rule, layout, and control.

The most recent successful NTP synchronization record is now maintained only inside the time service, while normal pages read it through the unified query interface. This reduces the risk of unrelated modules changing time state. Network time synchronization, RTC updates, the daily midnight schedule, manual synchronization, displayed information, and controls are unchanged.

The local maintenance workflow can now safely register tracked files removed during source refactoring while still rejecting misspelled or never-tracked missing paths. This tooling-only change does not alter device firmware, features, UI, settings, power behavior, or release permissions.

Battery-state updates are now restricted internally to the startup and battery-sampling owners, while pages and services retain read-only access to the same consistent snapshot. Battery sampling, charging detection, displayed values, low-battery protection, power behavior, and controls are unchanged.

Alarm and Pomodoro runtime state is now private to their service implementations. Pomodoro startup initialization and Xiaozhi tool registration are also restricted to application startup, preventing unrelated modules from bypassing confirmation, persistence, scheduling, registration, or completion rules. Alarm setting, replacement confirmation, ringing, Pomodoro timing, audio, UI, and voice controls are unchanged.

If a weather update is interrupted before it starts by OTA, low-battery protection, Offline Mode, or setup, the device now keeps the last valid weather and resolved-city cache. Normal page and backoff rules resume synchronization later, avoiding a blank display or a repeated location lookup; the state is cleared only when the required weather configuration is actually missing.

While OTA waits for Wi-Fi, low-battery checks now use the lightweight battery status instead of repeatedly locking the full battery record. Low-battery thresholds, update checks, connection timeout, download, verification, progress display, and controls are unchanged.

Internally saved Wi-Fi credentials, the QWeather API key, and the API Host can now be changed only by setup/configuration transactions, factory reset, and startup loading. Pages, weather, OTA, and other services retain read-only access to the snapshots they need. Setup fields, validation, reconnection, weather synchronization, and controls are unchanged.

The internally stored manual weather city can now be changed only by configuration persistence and startup loading, and its runtime state is initialized only during application startup. Weather, setup-page rendering, settings, and Xiaozhi continue to read the same city state. Setting a city through setup, the desktop client, or Xiaozhi, restoring automatic location, weather synchronization, NVS data, and controls are unchanged.

When setup or settings sounds remain busy through their final retry, the background attempt now ends immediately instead of waiting through an interval that cannot lead to another attempt. Normal sounds, retry timing between attempts, and successful playback are unchanged.

The gallery rotation period is now published to the running device only after its storage transaction succeeds. If saving fails, the previous period remains active without a temporary switch and rollback. The built-in gallery remains fixed at `24h`; custom-gallery choices, time boundaries, UI, and controls are unchanged.

Work-page toggles are now published to the running device only after their storage transaction succeeds. If saving fails, weather, Daily Saying, Xiaozhi, and other page-specific services continue using the previous setting instead of briefly starting or stopping from an unsaved value. Page restrictions, ordering, feedback, and controls are unchanged.

Xiaozhi Energy Saving is now applied only after its storage transaction succeeds. If saving fails, the previous five-minute automatic-return setting remains active without a temporary switch. The default state, status dot, and controls are unchanged.

Volume, sound selection, hourly chime, and all-day chime settings are now applied together only after the complete setting group is saved successfully. If saving fails, the previous sound configuration remains active without a temporary switch. Feedback, previews, and Xiaozhi voice-volume controls are unchanged.

Setup-portal stop completion and validation-result handoff are now restricted to the internal provisioning transaction. Normal pages, update checks, and other network tasks can request or observe setup activity but cannot end the portal lifecycle early. Setup feedback, automatic exit after success, portal retention after failure, Wi-Fi channel transitions, and controls are unchanged.

The legacy application aggregate entry has been retired internally. Firmware metadata and business interfaces are now maintained through their direct owners. This maintenance-only change does not alter device features, UI, settings, power behavior, or update procedures.

The local optimization workflow now replaces a single matching pending summary even when its release wording is refined during commit, while preserving ambiguous same-module entries. This prevents duplicate maintenance notes without changing firmware, versions, device behavior, or release permissions.

If network diagnostics cannot continue because of Offline Mode, low-battery protection, missing Wi-Fi configuration, Wi-Fi startup failure, or a connection timeout, the page now publishes all nine terminal results together instead of briefly mixing them with results from the previous run. Normal online diagnostics still updates each check in order and in real time; the checks, wording, 30-second automatic return, and controls are unchanged.

The device now validates work-page identifiers through one shared rule before publishing a page switch. An invalid out-of-range value cannot replace the currently visible page. Page names, order, toggles, BOOT navigation, home-page rules, and displayed content are unchanged.

When setup settings are saved repeatedly in quick succession, the newest attempt now supersedes the previous validation even if it is rejected locally for missing fields. The older validation will not keep waiting for 30 seconds, overwrite the latest feedback, or close the portal early. A valid current attempt still follows the existing phone-feedback and automatic-exit flow.

Wi-Fi startup and shutdown are now serialized across Xiaozhi, weather synchronization, diagnostics, setup, and OTA. Cleanup from an older operation cannot shut down the radio just as a newer operation takes ownership, while the radio still powers down under the existing policy after all users finish. Pages, synchronization intervals, setup, OTA, and controls are unchanged.

Power-management locks are now released more reliably when network or audio work finishes, preventing a rare task overlap from leaving the device in a higher-power state and blocking light sleep. Pages, audio, networking flows, and controls are unchanged.

You can long-press KEY to leave Network Diagnostics while it is running. The device now cancels all checks that have not started and powers down Wi-Fi after the current bounded check finishes, rather than completing the hidden diagnostics sequence in the background. Normal checks, order, and result display are unchanged.

If Network Diagnostics is opened again immediately after that return, the new run uses an independent request generation. The older session stops after its current bounded check and cannot continue, write into the new page, or retire the new request, preventing a duplicate full diagnostics run. Entry, checks, order, and messages are unchanged.

If a manual time, weather, or Daily Saying synchronization reaches its timeout, the device now also cancels any connection wait that has not completed and powers down Wi-Fi sooner. A single network request that has already started still finishes through its bounded cleanup path. Timeout duration, messages, and normal synchronization controls are unchanged.

If you leave Weather Clock, Weather Board, or Gallery while an automatic data refresh is still waiting for a connection, or if Offline Mode or OTA takes over, the device now cancels that connection wait and powers down Wi-Fi sooner. A network request that has already started still completes its bounded cleanup. Page content, caches, retry rules, and controls are unchanged.

The total wait for a phone to receive a provisioning result remains capped at 30 seconds. A feedback or new-save event arriving near the deadline now consumes only the remaining time instead of starting another 30-second wait; success feedback and the retry flow after a failed validation are unchanged.

Internal write access to sound settings is now restricted to the persistence, startup-loading, and Xiaozhi volume owners that actually need it. Volume, sound selection, hourly and all-day chimes, Xiaozhi volume control, previews, and NVS behavior are unchanged.

The CPU high-performance lock used by live Xiaozhi audio is now accessible only to the shared audio owner. Xiaozhi listening and conversation performance, other sounds, standby behavior, and user controls are unchanged.

If an automatic weather or Daily Saying refresh at startup encounters a temporary Wi-Fi or service failure, the device retries after about two minutes, with at most two additional attempts. Success, disabling the related page, Offline Mode, or low-battery protection clears the pending work; OTA pauses it while active, so Wi-Fi cannot retry forever and existing cache data is preserved.

The current-page state can now be changed only by the internal page-navigation owners, while display and background consumers retain read-only access. Page identifiers, order, toggles, BOOT navigation, automatic return, UI, and power behavior are unchanged.

Internally pending weather-city changes validated through Xiaozhi can now be written or retired only by the weather-city save transaction. Other modules retain snapshot and pending-status access. Voice city changes, automatic IP location, QWeather validation, NVS storage, and weather refresh behavior are unchanged.

The About Device hold deadline can now be changed only by the internal page and OTA coordinators, preventing unrelated features from extending the page lifetime. Opening About Device, its 30-second automatic return, KEY navigation, and OTA notices are unchanged.

The automatic return after Network Diagnostics now confirms that the screen still belongs to the same diagnostics request. Reopening diagnostics near the 30-second boundary can no longer be closed by the older timeout. Checks, results, layout, KEY navigation, and timing are unchanged.

If time, weather, Daily Saying, or Network Diagnostics is retried immediately after a timeout, the older network window now exits promptly instead of reopening Wi-Fi or repeating the same operation alongside the new request. Synchronization content, timing, feedback, and controls are unchanged.

If an internal resource error interrupts startup, the device now shuts down any Wi-Fi or setup hotspot that was already enabled and parks the audio pins, preventing the failed startup state from continuing to draw high power. Normal startup, setup, and page behavior are unchanged.

If a network or audio power-management lock temporarily disagrees with the driver state, the device now reconciles the state so the next session reacquires the correct performance protection. Normal pages, networking, audio, and light-sleep policies are unchanged.

Provisioning hotspot startup, validation-result delivery, feedback waiting, and failure cleanup now share one internal transaction owner. This prevents unrelated features from ending setup or releasing its network power lock early. Successful setup still exits automatically, while failed validation keeps the hotspot available for correction.

The global OTA runtime state is now initialized only during startup, while normal pages, networking, audio, and sensor services retain read-only access. This prevents future maintenance from accidentally resetting an active update state. Update checks, download progress, failure messages, successful reboot, and controls are unchanged.

The runtime state for saved Wi-Fi credentials, the QWeather API key, and the API Host is now also initialized only during startup. Weather, pages, OTA, and Xiaozhi retain read-only access. Provisioning, automatic reconnection, weather updates, factory reset, NVS data, and controls are unchanged.

The global runtime state used by Network Diagnostics is now initialized only during device startup. Settings and other features still use request and read-only result interfaces. The nine checks, display order, 30-second return, Wi-Fi behavior, and controls are unchanged.

The global runtime state used by the About Device page is now initialized only during device startup. Page entry, the 30-second automatic return, key return, OTA result hold timing, and controls are unchanged.

The global runtime state used by the provisioning portal is now initialized only during device startup. The hotspot name, local IP, credential validation, result feedback, correction after failure, and hotspot shutdown after success are unchanged.

The global work-page catalog state is now initialized only during device startup. Page names, toggles, ordering, the home page, BOOT navigation, Offline Mode restrictions, and network-trigger behavior are unchanged.

The pending weather-city state used by Xiaozhi is now initialized only by the internal weather-city tool owner. Voice city changes, automatic IP location, online validation, NVS storage, and the weather refresh after saving are unchanged.

The Settings activity state is now initialized only during device startup. Key controls, the 30-second automatic return, secondary-menu navigation, Network Diagnostics result timing, and OTA page notices are unchanged.

Settings feedback and manual synchronization state are now also initialized only during device startup. Time, weather, and Daily Saying synchronization, timeout messages, immediate retries, and network-request cleanup are unchanged.

The alarm service now loads saved state and registers voice controls only during device startup. Setting, replacing, disabling, ringing, stopping with any key, and restoring after reboot are unchanged.

Startup networking and OTA now preserve a deferred Wi-Fi shutdown request if a temporary lifecycle conflict prevents the first stop attempt. The resident network service completes that cleanup after the session releases its power lock, reducing the chance of Wi-Fi remaining powered after startup or an update failure. Update controls, network timing, and user-visible behavior are unchanged.

Enabling Offline Mode now also preserves a deferred Wi-Fi shutdown request when the first stop attempt is temporarily blocked. The resident network service completes the shutdown instead of entering its offline wait with the radio still powered. Offline page restrictions, setup access, saved settings, and controls are unchanged.

The HTTPS/WSS transaction lock that serializes weather, Daily Saying, Xiaozhi networking, and OTA is now initialized only during device startup. Runtime services can only acquire and release it, preventing maintenance changes from accidentally resetting the shared network boundary. Network order, timeouts, pages, OTA behavior, and controls are unchanged.

The internal generation state used to distinguish repeated manual time, weather, Daily Saying, and Network Diagnostics requests is now initialized only during startup. Manual synchronization, immediate retries, Xiaozhi weather-city updates, timeout feedback, and network behavior are unchanged.

The Wi-Fi driver, event handlers, and setup-hotspot foundation remain owned by one network lifecycle. If startup initialization fails because of a transient resource condition and cleanup completes safely, a later provisioning or network action can make one more bounded recovery attempt instead of requiring a reboot. Normal networking, radio shutdown, power policy, and controls are unchanged.

The device's light-sleep configuration and network/audio power-management locks are now initialized and recovered only by the startup flow. Weather, OTA, Xiaozhi, and audio still acquire and release those locks through the existing interfaces. Networking, playback, standby power policy, and controls are unchanged.

Saved Wi-Fi, weather, audio, page, Offline Mode, and Xiaozhi power-saving settings are now loaded together only during startup. Runtime changes still use the existing save flows and cannot be unexpectedly replaced by another full configuration reload. Stored values, defaults, and controls are unchanged.

The operation that powers down idle amplifier and I2S pins is now owned only by the shared audio lifecycle and startup-failure cleanup. Normal pages and networking cannot directly park audio hardware while another audio owner is active. Hourly chimes, alarms, Pomodoro, Xiaozhi voice, setup prompts, volume, and controls are unchanged.

The internal state and handler for the voice weather-city tool are now initialized only once during startup. Xiaozhi can still set a city, restore automatic IP location, and safely save and refresh weather after the conversation. City names, voice feedback, Settings display, NVS data, and controls are unchanged.

Deferred Wi-Fi shutdown retries are now serviced only by startup networking and the persistent network coordinator. Other features can request shutdown but cannot concurrently operate the radio. Weather, daily sayings, OTA, provisioning, Xiaozhi, Offline Mode, status icons, and controls are unchanged.

Weather, Daily Saying, and Xiaozhi continue to share display-memory protection during HTTPS work. Startup weather memory checks are now restricted to the internal network flow; network order, page display, voice behavior, power policy, and controls are unchanged.

OTA firmware writes now use one scope-owned handle. Interrupted downloads, allocation failures, and checksum failures automatically abandon the unfinished write, while a completed image transfers ownership exactly once to final validation. Update sources, progress, fallback behavior, partitions, and user controls are unchanged.

If the large temporary workspace needed for weather synchronization is unavailable, the device now ends that request safely instead of consuming critical internal memory shared by networking, display, and audio. Existing weather remains visible, and the original visible-page, manual-sync, or backoff retry rules handle recovery. Normal weather content, synchronization timing, settings, and controls are unchanged.

Large page canvases now remain in PSRAM and no longer fall back to internal memory shared by Wi-Fi, display DMA, and audio. Normal rendering, page navigation, partial refreshes, and controls are unchanged. Under extreme memory pressure, only the canvas that cannot be created is skipped safely with a diagnostic log instead of turning one allocation failure into wider networking or system instability.

Project validation now records actual internal-memory usage from each final firmware ELF audit, while long-lived documentation keeps only the fixed budget. This prevents measurements from older builds from being mistaken for the current result. Device features, UI, power behavior, and usage are unchanged.

The setup hotspot name and local-IP snapshot now derive their validity result directly from the text delivered to the caller. This makes the internal contract explicit and protects future maintenance without changing setup fields, page content, connection steps, or controls.

The manual weather-city snapshot now derives its configured result directly from the city text delivered to the caller. This makes the internal contract explicit and protects future maintenance without changing city input, weather synchronization, settings, or Xiaozhi voice controls.

Large provisioning pages now remain in PSRAM instead of falling back to internal memory needed by Wi-Fi and system tasks. The hotspot, form, scan, validation, and result flows are unchanged. Under extreme PSRAM pressure, the portal returns a clear memory warning and asks the user to retry instead of continuing an incomplete save flow.

The provisioning portal now handles Wi-Fi names and weather-city values containing an apostrophe without breaking or truncating the form attribute. Existing Chinese text, spaces, special-character handling, fields, and setup controls are unchanged.

The sensor service now performs the startup preparation for both hourly history and the local temperature-and-humidity state, so the application entry no longer operates those lower-level states separately. Sampling, four-hour trends, history, displayed values, saved data, and user controls are unchanged, and no action is required after an upgrade.

Gallery Clock, Weather Board, Temperature and Humidity Clock, Calendar, Temperature and Humidity History, and Xiaozhi now reuse one shared creator for the black separator below the status bar. Its position, size, color, page content, partial refresh behavior, and controls are unchanged, and no settings need to be adjusted after an upgrade.

Other fixed black separators and inverted panels in the firmware and SDL previews now use the same shared base-widget pattern. Existing page-specific corner radii, clipping, content, refresh timing, power policy, and controls are unchanged.

The internal work-page catalog now rejects unregistered page capability flags during compilation. This maintenance safeguard helps prevent future page additions from bypassing network or low-refresh rules; current pages, settings, synchronization, display, and controls are unchanged.

The resident-task catalog now rejects duplicate task functions or names and requires exactly one UI-notification owner. This maintenance safeguard helps prevent future changes from accidentally starting duplicate networking, sampling, alarm, or Pomodoro work. Current tasks, timing, pages, power behavior, and controls are unchanged.

When Wi-Fi has not been configured, weather and Daily Saying pages no longer queue automatic network work that cannot succeed. After setup, missing or stale data is still refreshed with the existing retry rules. Page content, setup steps, synchronization behavior, and controls are unchanged.

Local temperature and humidity readers now share one consistent internal snapshot path. Sampling intervals, four-hour trends, displayed values, Xiaozhi sensor queries, page refresh timing, and user controls are unchanged.

Gallery Clock now reads the Daily Saying text, synchronization time, and matching version as one consistent snapshot. If the device is briefly busy and that read cannot complete, the page retries on the next cycle instead of remaining blank until another network update. Daily Saying content, fetch timing, day changes, display, and controls are unchanged.

Weather Clock and Weather Board now update their content only after a complete weather snapshot is available. If an internal read cannot complete, the last valid weather remains visible and the page can retry later instead of replacing it with empty data. Weather services, refresh timing, layout, placeholders, and controls are unchanged.

Battery status now remains on the last valid reading if an internal snapshot is briefly unavailable. Normal sampling intervals, charging animation, low-battery protection, About Device information, OTA behavior, and controls are unchanged.

If the first battery-state read is temporarily unavailable during an abnormal startup, work pages now use a deterministic safe fallback and keep retrying instead of consuming uninitialized data for low-battery or charging UI decisions. Normal startup, sampling, and page behavior are unchanged.

Weather cache timing metadata now also keeps its last valid value if an internal read is briefly unavailable. About Device will not replace a valid last-weather-sync time with a placeholder during that transient condition. Weather content, update timing, networking, layout, and controls are unchanged.

Network Diagnostics now updates its summary and result rows only after a complete internal snapshot is available. If the device is briefly busy, the last visible results remain in place instead of returning to placeholders. Diagnostic checks, timeouts, networking, layout, and controls are unchanged.

About Device now remains open if its internal page state is briefly unavailable, instead of unexpectedly returning to a work page. Normal automatic return timing, buttons, OTA status, layout, and controls are unchanged.

Settings feedback now remains visible if its internal state is briefly unavailable, instead of disappearing before its normal timeout. Synchronization, save and confirmation messages, settings timing, layout, and controls are unchanged.

If button interrupt or Light Sleep wakeup setup fails during a rare hardware fault, the firmware now slows only the idle fallback polling: interactive screens remain responsive at 50 ms, ordinary work pages use 250 ms, and minute-level pages use 500 ms. A held button still uses 20 ms tracking. Normal devices continue to wake immediately from GPIO edges, so debounce, short and long presses, page controls, display timing, and normal Light Sleep behavior are unchanged.

After startup networking, weather synchronization, OTA, or an Offline Mode transition finishes, the device now uses one internal path to check whether Wi-Fi is still running and registers deferred shutdown only when needed. Radio shutdown order, network data, retries, display behavior, and controls are unchanged. This maintenance reduces the chance that a future failure path omits low-power cleanup.

The display layer now confirms that the SPI queue is empty after each full-screen or partial pixel transfer before the next refresh can modify the shared buffer. Already queued work is also drained after a transfer error. Page content, partial-refresh regions, refresh rates, and controls are unchanged; this reduces the risk of an occasional corrupted frame during rapid refreshes or network-memory pressure.

Network Diagnostics now publishes the Weather or Daily Saying result as soon as that check finishes, before starting NTP or the Internet probe. Completed rows no longer remain at “Checking” while the next blocking operation runs. The nine checks, order, request count, timeouts, Wi-Fi use, and controls are unchanged.

Network Diagnostics now distinguishes a successful public-IP HTTP response from successful IPv4 parsing. If the service responds but its payload format is temporarily unsupported, the Public IP row still reports failure while Internet access is accepted without opening a duplicate HTTPS request to the same endpoint. A real transport failure still runs the existing fallback probe.
