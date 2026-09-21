# ESP32-S3 RLCD 4.2 天氣時鐘

天氣來源可選和風或 Open-Meteo，預設仍為和風。啟用 Open-Meteo 不需和風 Key/Host，空氣品質採 US AQI，不提供天氣警報，也不會自行混用來源。公共介面僅供非商業使用；資料來自 [Open-Meteo](https://open-meteo.com/) / [CAMS](https://atmosphere.copernicus.eu/)，城市由 [GeoNames](https://www.geonames.org/) 查詢。

简体中文 [切換](README.md) · 繁體中文（目前） · [English](README_EN.md) · [日本語](README_JA.md)

以微雪 ESP32-S3 和 4.2 吋 RLCD 為基礎的桌面時鐘，整合常駐顯示、天氣、本機溫溼度、圖片、月曆、提醒與小智 AI。重點是按需連網與低功耗，不是讓所有功能持續運作。

## 開始使用

- [快速入門](docs/User_zh_TW.md) · [詳細說明](docs/User_Detailed_zh_TW.md)
- [WeatherClock Studio 網頁工具](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/)
- [實機模擬預覽](https://wickenzh.github.io/ESP32-S3-RLCD-4.2/#screens)
- [OTA 韌體與版本](https://github.com/wickenzh/ESP32-S3-RLCD-4.2_UP)
- [Power Demo](docs/Power%20Demo/README.md)

本體介面主要為簡體中文；翻譯文件不代表已提供介面語言選擇。首次使用請連裝置熱點，在設定網頁填 Wi-Fi、自己的 QWeather Key 與帳號專屬 API Host；備用 Wi-Fi 和城市可留白。離線使用只需設定本地日期時間。

## 八種畫面

1. 天氣時鐘：時分秒、天氣、GIF、警報與狀態。
2. 圖片時鐘：星期圖片或自訂圖庫、分鐘時鐘、每日文字。
3. 天氣看板：當前氣象、未來預報、空氣品質、風、日出日落與建議。
4. 溫溼時鐘：高對比數字、本機溫溼度、趨勢與農曆。
5. 月曆：當月日期、今日標示、農曆與節日。
6. 溫溼歷史：最近24小時溫溼度曲線。
7. 小智 AI：喚醒與語音對話、字幕、表情、鬧鐘、番茄鐘與城市設定。
8. 整合時鐘：大時鐘、當天天氣、日期農曆及溫溼度，搭配靜態點陣與天氣裝飾。

可切換頁面開關與順序；至少保留一個非小智頁面，排序第一項是首頁。天氣與感測資料共用快取，隱藏頁面不另外重繪。內建圖庫依星期每天切換，自訂圖庫可選30分鐘到24小時週期。

## 操作與網路

BOOT短按翻頁或確認，KEY短按進入設定或移動游標，長按返回。小智可設定一個單次鬧鐘與獨立番茄鐘，設定結果請以畫面核對。小智節能可在閒置時返回首頁。

Wi-Fi可保存主、備兩組，首選失敗時嘗試備援。手動城市可由設定網頁、網頁工具或語音指定，也能恢復自動定位。離線模式下不啟用聯網頁面。天氣數值不是本機感測器的測量值。

## 低功耗與畫面實作

UI主要採用LVGL：文字、選單與狀態由控件管理，時鐘、圖片和曲線搭配Canvas自繪；RLCD驅動負責單色傳輸，優先局部更新。

ESP-IDF動態電源管理讓CPU依負載在40–240MHz之間調整，FreeRTOS Tickless Idle配合空閒時的Light Sleep；Wi-Fi使用節能模式，一般同步結束關閉射頻，音效結束釋放Codec。正式韌體不以Deep Sleep維持日常運作。

秒級畫面只更新變動數字；低頻畫面依分鐘、日期或資料變化更新。感測器白天約每分鐘、夜間約每兩分鐘取樣。實際續航受電池、開發板和小智使用情況影響，不能只用瞬間電流推算。

## 硬體、工具鏈與目錄

- [微雪官方產品](https://www.waveshare.com/product/esp32-s3-rlcd-4.2.htm) · [官方文件](https://docs.waveshare.com/ESP32-S3-RLCD-4.2)
- ESP32-S3-WROOM-1-N16R8，16MB Flash、8MB PSRAM，400×300 RLCD。
- ESP-IDF v5.5.3；LVGL v8.4.0。
- `RLCD_CLOCK/`包含韌體工程、字型、元件與建置所需檔案；從該目錄執行`idf.py build`。
- `host_web/`是網頁工具，`docs/`是使用文件，`previews/`是模擬預覽，`firmware/`保留OTA清單。

## 更新與自訂素材

App bin供OTA或正確分割區上的應用更新，不可燒到0x0。Merged bin包含bootloader、分割表、OTA選擇、語音模型和App，從0x0完整燒錄，可能覆蓋資料。OTA不更新分割表；升級請依發行說明操作。

網頁工具可製作圖片與GIF、驗證韌體雜湊並透過串列埠寫入。素材不存在或驗證失敗時回退內建內容。正式版與網頁部署分開，不因單純網頁更新就產生新韌體版本。

## 上游來源與授權

小智移植自[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，原始基線為[7b190b78e4f8dfef14126f6cd478c134b3cd3cd8](https://github.com/78/xiaozhi-esp32/commit/7b190b78e4f8dfef14126f6cd478c134b3cd3cd8)。上游MIT版權、授權與免責聲明保留於[第三方聲明](THIRD_PARTY_NOTICES.md)。本專案的非商業限制不取代第三方已授予的權利。

請閱讀[貢獻指南](CONTRIBUTING.md)、[安全政策](SECURITY.md)及[專案授權](LICENSE)。不要公開密碼、API金鑰、Token、NVS映像或私人位址，也不要提交無關依賴和建置產物。

**嚴禁商用。** 原創部分僅供非商業學習、研究與評估；商業使用須取得所有者書面許可。第三方元件仍依原授權條款使用。
