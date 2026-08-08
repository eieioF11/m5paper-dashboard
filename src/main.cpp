#undef ARDUINO_M5STACK_FIRE
#define ARDUINO_M5STACK_Paper
#include <ArduinoOTA.h>
#include <M5EPD.h>

#include <array>

#include "SPIFFS.h"
#define FASTLED_INTERNAL  // suppress pragma message
#include <FastLED.h>

#include "SHT3X.h"
#include "WiFiInfo.h"

#define LGFX_M5PAPER
#define LGFX_USE_V1
#include <ESPmDNS.h>

#include <LGFX_AUTODETECT.hpp>
#include <LovyanGFX.hpp>

#include "misc.h"
#include "myFont.h"

#define M5EPD_TOUCH_INT 36

constexpr float FONT_SIZE_GIANT = 5.0;
constexpr float FONT_SIZE_LARGE = 3.0;
constexpr float FONT_SIZE_MIDDLE = 2.0;
constexpr float FONT_SIZE_SMALL = 1.0;
constexpr uint_fast16_t M5PAPER_SIZE_LONG_SIDE = 960;
constexpr uint_fast16_t M5PAPER_SIZE_SHORT_SIDE = 540;

rtc_time_t time_ntp;
rtc_time_t latest_update_time;
rtc_date_t date_ntp{4, 1, 1, 1970};

SemaphoreHandle_t xMutex = nullptr;
SHT3X::SHT3X sht30;
static LGFX gfx;
std::array<CRGB, 3> leds;
weather_t weather_data;

inline int syncNTPTimeJP(void) {
	constexpr auto NTP_SERVER1 = "ntp.nict.jp";
	constexpr auto NTP_SERVER2 = "time.cloudflare.com";
	constexpr auto NTP_SERVER3 = "time.google.com";
	constexpr auto TIME_ZONE = "JST-9";

	auto datetime_setter = [](const tm& datetime) {
		rtc_time_t time{static_cast<int8_t>(datetime.tm_hour), static_cast<int8_t>(datetime.tm_min),
						static_cast<int8_t>(datetime.tm_sec)};
		rtc_date_t date{
			static_cast<int8_t>(datetime.tm_wday), static_cast<int8_t>(datetime.tm_mon + 1),
			static_cast<int8_t>(datetime.tm_mday), static_cast<int16_t>(datetime.tm_year + 1900)};

		M5.RTC.setTime(&time);
		M5.RTC.setDate(&date);
		// M5.RTC.setDateTime(date, time);
		date_ntp = date;
		time_ntp = time;
	};

	return syncNTPTime(datetime_setter, TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
}

void handleBtnPPress(void) {
	xSemaphoreTake(xMutex, portMAX_DELAY);
	prettyEpdRefresh(gfx);
	gfx.setTextSize(FONT_SIZE_SMALL);

	gfx.startWrite();
	gfx.setCursor(0, 0);
	if (!syncNTPTimeJP()) {
		gfx.println("Succeeded to sync time");
		struct tm timeInfo;
		if (getLocalTime(&timeInfo)) {
			gfx.print("getLocalTime:");
			gfx.println(&timeInfo, "%Y/%m/%d %H:%M:%S");
		}
	} else {
		gfx.println("Failed to sync time");
	}

	rtc_date_t date;
	rtc_time_t time;

	// Get RTC
	// M5.RTC.getDateTime(date, time);
	M5.RTC.getTime(&time);
	M5.RTC.getDate(&date);

	gfx.print("RTC         :");
	gfx.printf("%04d/%02d/%02d ", date.year, date.mon, date.day);
	gfx.printf("%02d:%02d:%02d", time.hour, time.min, time.sec);
	gfx.endWrite();

	delay(1000);

	gfx.setTextSize(FONT_SIZE_LARGE);
	xSemaphoreGive(xMutex);
}

inline void handleBtnRPress(void) {
	xSemaphoreTake(xMutex, portMAX_DELAY);
	prettyEpdRefresh(gfx);
	xSemaphoreGive(xMutex);
}

void handleBtnLPress(void) {
	xSemaphoreTake(xMutex, portMAX_DELAY);
	prettyEpdRefresh(gfx);
	gfx.setCursor(0, 0);
	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.print("Good bye..");
	gfx.waitDisplay();
	M5.disableEPDPower();
	M5.disableEXTPower();
	M5.disableMainPower();
	esp_deep_sleep_start();
	while (true);
	xSemaphoreGive(xMutex);
}

void handleButton(void* pvParameters) {
	while (true) {
		delay(500);
		M5.update();
		if (M5.BtnP.isPressed()) {
			handleBtnPPress();
		} else if (M5.BtnR.isPressed()) {
			handleBtnRPress();
		} else if (M5.BtnL.isPressed()) {
			handleBtnLPress();
		}
	}
}

void wifi_connecting(void) {
	constexpr uint_fast16_t WIFI_CONNECT_RETRY_MAX = 60;  // 10 = 5s
	WiFi.begin(WiFiInfo::SSID, WiFiInfo::PASS);
	for (int cnt_retry = 0; cnt_retry < WIFI_CONNECT_RETRY_MAX && !WiFi.isConnected();
		 cnt_retry++) {
		delay(500);
		gfx.print(".");
	}
}

void setup(void) {
	constexpr uint_fast16_t WAIT_ON_FAILURE = 2000;

	M5.begin(true, false, true, true, true);
	// M5.begin(true, false, true, true, true, true);
	// WiFi.begin(WiFiInfo::SSID, WiFiInfo::PASS);

	FastLED.addLeds<WS2811, 26, GRB>(leds.data(), 3).setCorrection(TypicalSMD5050);
	FastLED.setBrightness(5);

	gfx.init();
	gfx.setEpdMode(epd_mode_t::epd_fast);
	gfx.setRotation(1);
	M5.TP.SetRotation(1);
	// gfx.setFont(&fonts::lgfxJapanGothic_40);
	gfx.setFont(&myFont::myFont);
	gfx.setTextSize(FONT_SIZE_SMALL);

	gfx.print("Connecting to Wi-Fi network");
	wifi_connecting();
	gfx.println("");
	if (WiFi.isConnected()) {
		gfx.print("Local IP: ");
		gfx.println(WiFi.localIP());
		MDNS.begin("m5paper");
	} else {
		gfx.println("Failed to connect to a Wi-Fi network");
		delay(WAIT_ON_FAILURE);
	}

	// ArduinoOTA
	// 	.onStart([]() {
	// 		String type;
	// 		if (ArduinoOTA.getCommand() == U_FLASH)
	// 			type = "sketch";
	// 		else  // U_SPIFFS
	// 			type = "filesystem";

	// 		// NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
	// 		Serial.println("Start updating " + type);
	// 	})
	// 	.onEnd([]() { Serial.println("\nEnd"); })
	// 	.onProgress([](unsigned int progress, unsigned int total) {
	// 		Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
	// 	})
	// 	.onError([](ota_error_t error) {
	// 		Serial.printf("Error[%u]: ", error);
	// 		if (error == OTA_AUTH_ERROR)
	// 			Serial.println("Auth Failed");
	// 		else if (error == OTA_BEGIN_ERROR)
	// 			Serial.println("Begin Failed");
	// 		else if (error == OTA_CONNECT_ERROR)
	// 			Serial.println("Connect Failed");
	// 		else if (error == OTA_RECEIVE_ERROR)
	// 			Serial.println("Receive Failed");
	// 		else if (error == OTA_END_ERROR)
	// 			Serial.println("End Failed");
	// 	});

	// ArduinoOTA.begin();

	// env2 unit
	if (!sht30.begin(21, 22, 400000)) {
		gfx.println("Failed to initialize external I2C");
	}

	xMutex = xSemaphoreCreateMutex();
	if (xMutex != nullptr) {
		xSemaphoreGive(xMutex);
		xTaskCreatePinnedToCore(handleButton, "handleButton", 4096, nullptr, 1, nullptr, 1);
	} else {
		gfx.println("Failed to create a task for buttons");
	}
	gfx.println("Init done");
	delay(1000);
	gfx.setTextSize(FONT_SIZE_LARGE);
	prettyEpdRefresh(gfx);
	gfx.setCursor(0, 0);
	SPIFFS.begin();
	// update
	syncNTPTimeJP();
	rtc_time_t time;
	M5.RTC.getTime(&time);
	latest_update_time = time;
	weather_t weather = get_weather();
	if (weather.success) weather_data = weather;
	delay(1000);
	WiFi.mode(WIFI_OFF);
}
constexpr uint32_t bt_low = 3300;
constexpr uint32_t bt_high = 4350;
uint32_t get_battery_percentage(uint32_t ivolt) {
	uint32_t dmax = bt_high - bt_low;
	ivolt -= bt_low;
	return (uint32_t)std::lround(std::min(((float)ivolt / (float)dmax) * 100.0, 100.0));
}

// 右上にバッテリーを表示する関数（大型化・背景：白仕様）
void drawBattery(LGFX& gfx, int percentage) {
	if (percentage < 0) percentage = 0;
	if (percentage > 100) percentage = 100;

	// --- パーツのサイズ定義（大きく修正） ---
	const int32_t iconWidth = 60;	// アイコン本体の幅
	const int32_t iconHeight = 30;	// アイコン本体の高さ
	const int32_t capWidth = 6;		// 突起の幅
	const int32_t border = 3;		// 枠線の太さ

	// 右上からのマージン（余白）
	const int32_t marginX = 0;
	const int32_t marginY = 10;

	// フォントサイズ設定（大きく）
	const float batteryFontSize = FONT_SIZE_SMALL;

	// 全体幅（アイコン + 突起 + 隙間8px + テキスト幅）
	const int32_t totalWidth = iconWidth + capWidth + 8;

	// 描画開始座標（右上基準）
	int32_t x = M5PAPER_SIZE_LONG_SIDE - totalWidth - marginX;
	int32_t y = marginY;

	// 1. 背景クリア（白で背景をリセット、文字エリアまで含めて広めにクリア）
	gfx.fillRect(x - 5, y - 5, totalWidth + 10, iconHeight + 10, TFT_WHITE);

	// 2. 外枠と突起（黒描画）
	gfx.drawRect(x, y, iconWidth, iconHeight, TFT_BLACK);  // 外枠
	// 線の太さを出すために内側にもう一枚描画
	gfx.drawRect(x + 1, y + 1, iconWidth - 2, iconHeight - 2, TFT_BLACK);

	// プラス極突起（本体高さの中央に配置）
	gfx.fillRect(x + iconWidth, y + (iconHeight / 4), capWidth, iconHeight / 2, TFT_BLACK);

	// 3. 残量バー（黒描画）
	// 枠線の太さを考慮した内側の描画エリア
	int32_t innerMargin = border + 1;
	int32_t maxInnerWidth = iconWidth - (innerMargin * 2);
	int32_t innerHeight = iconHeight - (innerMargin * 2);

	// 残量に応じた幅計算
	int32_t barWidth = (maxInnerWidth * percentage) / 100;

	// 1%以上なら最低2px表示して視認性を確保
	if (percentage > 0 && barWidth < 2) barWidth = 2;

	gfx.fillRect(x + innerMargin, y + innerMargin, barWidth, innerHeight, TFT_BLACK);

	// 4. パーセンテージ文字表示（文字色:黒, 背景色:白、大型フォント）
	gfx.setTextSize(batteryFontSize);
	gfx.setTextColor(TFT_BLACK, TFT_WHITE);

	// アイコンの右側に、高さの中央を合わせて配置
	gfx.setCursor(x-10, y+iconHeight);
	// int32_t textY = y + (iconHeight / 2) - (gfx.fontHeight(batteryFontSize) / 2);
	// gfx.setCursor(x + iconWidth + capWidth + 8, textY);

	gfx.printf("%3d%%", percentage);
}

void loop(void) {
	constexpr uint_fast16_t SLEEP_SEC = 60;
	constexpr uint_fast32_t TIME_SYNC_CYCLE = 3600 * 24 / SLEEP_SEC;
	constexpr uint_fast32_t UPDATE_SYNC_CYCLE = 3600 * 1 / SLEEP_SEC;
	static uint32_t update_cnt = 0;
	static uint32_t cnt = 0;
	bool update = false;
	update_cnt++;
	// if (M5.TP.available()) {
	// 	if (!M5.TP.isFingerUp()) {
	// 		Serial.println("touch");
	// 		// touch = true;
	// 	}
	// }
	if (update_cnt >= UPDATE_SYNC_CYCLE) {
		wifi_connecting();
		gfx.clear();
		weather_t weather = get_weather();
		if (weather.success) weather_data = weather;
		update_cnt = 0;
		update = true;
	}

	xSemaphoreTake(xMutex, portMAX_DELAY);
	// ArduinoOTA.handle();

	float tmp = 0.0;
	uint_fast8_t hum = 0;
	if (!sht30.read()) {
		tmp = sht30.getTemperature();
		hum = sht30.getHumidity();
	}

	rtc_date_t date;
	rtc_time_t time;

	// M5.RTC.getDateTime(date, time);
	M5.RTC.getTime(&time);
	M5.RTC.getDate(&date);

	gfx.startWrite();
	gfx.fillScreen(TFT_WHITE);
	gfx.fillRect(0.57 * M5PAPER_SIZE_LONG_SIDE, 0, 3, M5PAPER_SIZE_SHORT_SIDE, TFT_BLACK);

	constexpr uint_fast16_t offset_y = 30;
	constexpr uint_fast16_t offset_x = 45;

	gfx.setCursor(0, offset_y);
	gfx.setTextSize(FONT_SIZE_GIANT);
	gfx.setClipRect(offset_x, offset_y, M5PAPER_SIZE_LONG_SIDE - offset_x,
					M5PAPER_SIZE_SHORT_SIDE - offset_y);

	// gfx.printf("%02d:%02d:%02d\r\n", time.hour, time.min, time.sec);
	gfx.printf("%02d:%02d\r\n", time.hour, time.min);
	// gfx.setTextSize(FONT_SIZE_SMALL);
	// gfx.setCursor(480, offset_y + 170);
	// gfx.printf("/%02d\r\n", time.sec);
	gfx.setTextSize(FONT_SIZE_MIDDLE);
	gfx.setCursor(0, offset_y + 195);
	gfx.printf("    %s\r\n", weather_data.weather.c_str());
	gfx.printf("    %02.1f℃\r\n", weather_data.temperature);
	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.printf("\r\n");
	gfx.drawPngFile(SPIFFS, "/weather_icons/" + weather_data.icon + "@2x.png", 20, offset_y + 180,
					0, 0, 0, 0, 2.0, 2.0);
	gfx.setTextSize(FONT_SIZE_MIDDLE);
	gfx.printf("%02d%% %02.1f℃\r\n", hum, tmp);
	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.printf("latest update : %02d:%02d\r\n", latest_update_time.hour, latest_update_time.min);
	gfx.setTextSize(FONT_SIZE_LARGE);
	gfx.clearClipRect();

	uint32_t vol = std::min(std::max(M5.getBatteryVoltage(), bt_low), bt_high);
	uint32_t bt_per = get_battery_percentage(vol);
	drawBattery(gfx, bt_per);

	constexpr float x = 0.61 * M5PAPER_SIZE_LONG_SIDE;
	gfx.setTextSize(FONT_SIZE_LARGE);
	gfx.setCursor(0, offset_y);
	gfx.setClipRect(x, offset_y, M5PAPER_SIZE_LONG_SIDE - offset_x - x,
					M5PAPER_SIZE_SHORT_SIDE - offset_y);
	gfx.printf("%04d\r\n", date.year);
	gfx.printf("%02d/%02d\r\n", date.mon, date.day);
	gfx.println(weekdayToString(date.week));
	gfx.clearClipRect();

	constexpr float offset_y_info = 0.75 * M5PAPER_SIZE_SHORT_SIDE;
	gfx.setCursor(0, offset_y_info);
	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.setClipRect(x, offset_y_info, M5PAPER_SIZE_LONG_SIDE - x, gfx.height() - offset_y_info);
	gfx.print("WiFi: ");
	gfx.println(WiFiConnectedToString());

	gfx.printf("BAT : %04dmv\r\n", vol);
	// gfx.printf("BAT : %04dmv %03d%%\r\n", vol, bt_per);
	gfx.print("NTP : ");
	if (date_ntp.year == 1970) {
		gfx.print("YET");  // not initialized
	} else {
		gfx.printf("%02d/%02d %02d:%02d", date_ntp.mon, date_ntp.day, time_ntp.hour, time_ntp.min);
	}
	gfx.clearClipRect();
	gfx.setTextSize(FONT_SIZE_LARGE);
	gfx.endWrite();

	cnt++;
	if (cnt == TIME_SYNC_CYCLE) {
		wifi_connecting();
		syncNTPTimeJP();
		cnt = 0;
		update = true;
	}
	if (update) {
		latest_update_time = time;
		delay(1000);
		WiFi.mode(WIFI_OFF);
	}
	xSemaphoreGive(xMutex);
	// delay(SLEEP_SEC * 1000);
	Serial.flush();	 // Serialをflushさせておく
	esp_sleep_enable_timer_wakeup(SLEEP_SEC * 1000000);
	// esp_sleep_enable_ext0_wakeup(GPIO_NUM_36,
	// 							 LOW);	// タッチで GPIO36 が LOW になるのでこの時も起床
	esp_light_sleep_start();  // ライトスリープ開始
}
