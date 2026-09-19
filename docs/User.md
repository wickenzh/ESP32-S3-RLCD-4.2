# WeatherClock Quick Guide

[Detailed guide](User_Detailed.md) · [Simplified Chinese](User_zh.md) · [Traditional Chinese](User_zh_TW.md) · [Japanese](User_ja.md)

## Set up the device

1. Power on and join the WeatherClock hotspot shown on the display. Use its displayed password.
2. Open the setup portal, or visit `http://192.168.4.1/`. Stay connected if your phone reports no internet.
3. Enter your primary Wi-Fi details, QWeather API Key, and account-specific API Host. Backup Wi-Fi and a manual weather city are optional.
4. Save and wait for validation. Correct any reported errors; successful setup opens a work page.

API Host is your QWeather account's assigned domain, not a key or API path. For offline use, leave Wi-Fi blank and enter a date and time. Offline date/time fields can stay empty for online setup.

## Use the buttons

- Short **BOOT**: next work page; confirm within settings.
- Short **KEY**: open settings; move the selection.
- Hold **KEY**: back one level or leave settings.
- While an alarm or focus-completion sound plays, either button stops it.

Settings close after about 30 idle seconds. The firmware UI is primarily in Simplified Chinese; this guide does not imply a language selector.

## Everyday use

Eight pages provide Weather Clock, Picture Clock, Weather Board, Temperature & Humidity Clock, Calendar, History, Xiaozhi AI, and Aggregate Clock. Display settings control page visibility and order. The first page is home, and at least one non-AI page must remain.

Weather refreshes as needed at hour boundaries. Built-in images follow the weekday; uploaded galleries have configurable rotation. Local readings update about every minute by day and every two minutes at night. AI consumes more power and can warm the board, affecting temperature readings.

AI tools can set a one-shot alarm, focus timer, and weather city. Check the on-screen result. Ordinary delayed reminders use the alarm; explicitly request focus or a Pomodoro timer for focused work. Replacing an existing alarm requires confirmation.

## Updates, assets, and help

- In System settings, check for updates with BOOT, then confirm an available update within 60 seconds.
- Use [WeatherClock Studio](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/) on a desktop browser for assets, previews, flashing, and logs.
- The web UI supports narrow-screen viewing and stacks the simulator vertically. Asset writing, firmware flashing and serial operations still require desktop Chrome/Edge with Web Serial.
- The top-right language selector supports English, Japanese, Simplified Chinese and Traditional Chinese (Taiwan). Your choice is remembered; firmware screens and the Wi-Fi setup demo keep their original language.
- Missing weather: check Key, Host, permissions, and network. Cached values are not proof of a successful refresh.
- The Wi-Fi icon shows radio activity. Battery and charging indicators are estimates; USB connection does not mean continuous charging.
- Offline mode allows local pages only. Returning online requires complete credentials.
- **Never flash an App bin at 0x0. OTA does not change the partition table.** Read the detailed guide and release notes before a first installation or layout migration.
- Factory reset clears network and ordinary settings but preserves AI binding, history, and custom assets. It is not a whole-flash erase.

Keep credentials and private addresses out of public logs, screenshots, and quick-configuration links. See the [detailed guide](User_Detailed.md) for complete instructions.
The firmware currently uses UTC+8 with no timezone selector. A translated guide does not change the device timezone.

## Optional weather provider

Setup and WeatherClock Studio quick configuration now offer a Use Open-Meteo switch, off by default. Off selects QWeather and its existing Key/Host requirements. On uses Open-Meteo without those credentials; saved QWeather credentials are retained, and no automatic provider fallback occurs. Switching back requires valid QWeather credentials.

Open-Meteo provides model-based weather and explicitly labelled US AQI, not China's AQI. This integration does not provide weather alerts and says so on screen. The public API is for non-commercial use and still depends on network availability. Quick-configuration links require firmware supporting both providers; older firmware may ignore the choice. Offline setup still needs only a valid date and time.

Data: [Open-Meteo](https://open-meteo.com/) and [CAMS](https://atmosphere.copernicus.eu/). Location lookup: [GeoNames](https://www.geonames.org/).
