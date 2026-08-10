#undef ARDUINO_M5STACK_FIRE
#define ARDUINO_M5STACK_Paper
#include <M5EPD.h>

#include <array>

#include "SHT3X.h"
#include "SPIFFS.h"
#include "WiFiInfo.h"

#define LGFX_M5PAPER
#define LGFX_USE_V1
#include <LGFX_AUTODETECT.hpp>
#include <LovyanGFX.hpp>

#include "misc.h"
#include "myFont.h"

constexpr float FONT_SIZE_GIANT = 5.0;
constexpr float FONT_SIZE_LARGE = 3.0;
constexpr float FONT_SIZE_MIDDLE = 2.0;
constexpr float FONT_SIZE_SMALL = 1.0;
constexpr uint_fast16_t M5PAPER_SIZE_LONG_SIDE = 960;
constexpr uint_fast16_t M5PAPER_SIZE_SHORT_SIDE = 540;

// --- Deep Sleep 復帰時にリセットされないように、すべて「純粋な数値型(プリミティブ)」で宣言 ---
RTC_DATA_ATTR uint32_t update_cnt = 0;
RTC_DATA_ATTR uint32_t cnt = 0;

// 最後に通信したNTP時刻用
RTC_DATA_ATTR int16_t rtc_ntp_year = 1970;
RTC_DATA_ATTR int8_t rtc_ntp_mon = 1;
RTC_DATA_ATTR int8_t rtc_ntp_day = 1;
RTC_DATA_ATTR int8_t rtc_ntp_hour = 0;
RTC_DATA_ATTR int8_t rtc_ntp_min = 0;

// last update用
RTC_DATA_ATTR int8_t rtc_latest_update_hour = -1;  // -1は未更新の印
RTC_DATA_ATTR int8_t rtc_latest_update_min = -1;

// 天気データ用
RTC_DATA_ATTR bool rtc_weather_success = false;
RTC_DATA_ATTR char rtc_weather_text[64] = "";
RTC_DATA_ATTR char rtc_weather_icon[32] = "";
RTC_DATA_ATTR float rtc_weather_temp = 0.0;

SHT3X::SHT3X sht30;
static LGFX gfx;

constexpr uint32_t bt_low = 3300;
constexpr uint32_t bt_high = 4350;

uint32_t get_battery_percentage(uint32_t ivolt) {
	uint32_t dmax = bt_high - bt_low;
	ivolt = (ivolt < bt_low) ? bt_low : std::min(ivolt, bt_high);
	ivolt -= bt_low;
	return (uint32_t)std::lround(((float)ivolt / (float)dmax) * 100.0f);
}

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

		// バグ防止：NTP取得時刻を純粋な数値としてRTCメモリに保存
		rtc_ntp_year = date.year;
		rtc_ntp_mon = date.mon;
		rtc_ntp_day = date.day;
		rtc_ntp_hour = time.hour;
		rtc_ntp_min = time.min;
	};

	return syncNTPTime(datetime_setter, TIME_ZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
}

bool wifi_connecting(void) {
	constexpr uint_fast16_t WIFI_CONNECT_RETRY_MAX = 60;
	WiFi.begin(WiFiInfo::SSID, WiFiInfo::PASS);
	for (int cnt_retry = 0; cnt_retry < WIFI_CONNECT_RETRY_MAX && !WiFi.isConnected();
		 cnt_retry++) {
		delay(500);
	}
	return WiFi.isConnected();
}

void updateWeatherToRTC() {
	weather_t weather = get_weather();
	if (weather.success) {
		rtc_weather_success = true;
		strncpy(rtc_weather_text, weather.weather.c_str(), sizeof(rtc_weather_text) - 1);
		strncpy(rtc_weather_icon, weather.icon.c_str(), sizeof(rtc_weather_icon) - 1);
		rtc_weather_temp = weather.temperature;
	}
}

void drawBattery(LGFX& gfx, int percentage) {
	if (percentage < 0) percentage = 0;
	if (percentage > 100) percentage = 100;

	const int32_t iconWidth = 60;
	const int32_t iconHeight = 30;
	const int32_t capWidth = 6;
	const int32_t border = 3;

	const int32_t marginX = 20;
	const int32_t marginY = 15;

	const int32_t totalWidth = iconWidth + capWidth + 8;
	int32_t x = M5PAPER_SIZE_LONG_SIDE - totalWidth - marginX;
	int32_t y = marginY;

	gfx.fillRect(x - 5, y - 5, totalWidth + 10, iconHeight + 35, TFT_WHITE);
	gfx.drawRect(x, y, iconWidth, iconHeight, TFT_BLACK);
	gfx.drawRect(x + 1, y + 1, iconWidth - 2, iconHeight - 2, TFT_BLACK);
	gfx.fillRect(x + iconWidth, y + (iconHeight / 4), capWidth, iconHeight / 2, TFT_BLACK);

	int32_t innerMargin = border + 1;
	int32_t maxInnerWidth = iconWidth - (innerMargin * 2);
	int32_t innerHeight = iconHeight - (innerMargin * 2);
	int32_t barWidth = (maxInnerWidth * percentage) / 100;

	if (percentage > 0 && barWidth < 2) barWidth = 2;
	gfx.fillRect(x + innerMargin, y + innerMargin, barWidth, innerHeight, TFT_BLACK);

	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.setTextColor(TFT_BLACK, TFT_WHITE);
	gfx.setCursor(x - 10, y + iconHeight + 2);
	gfx.printf("%3d%%", percentage);
}

void setup(void) {
	constexpr uint_fast16_t SLEEP_SEC = 60;
	constexpr uint_fast32_t TIME_SYNC_CYCLE = (3600 * 24) / SLEEP_SEC;
	constexpr uint_fast32_t UPDATE_SYNC_CYCLE = (3600 * 12) / SLEEP_SEC;
	bool did_update_wifi = false;

	M5.begin(true, false, true, true, true);
	setCpuFrequencyMhz(80);

	gfx.init();
	gfx.setEpdMode(epd_mode_t::epd_fast);
	gfx.setRotation(1);
	gfx.setFont(&myFont::myFont);
	SPIFFS.begin();
	sht30.begin(21, 22, 400000);

	esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

	gfx.startWrite();
	gfx.fillScreen(TFT_WHITE);
	uint32_t vol = std::min(std::max(M5.getBatteryVoltage(), bt_low), bt_high);
	uint32_t bt_per = get_battery_percentage(vol);
	drawBattery(gfx, bt_per);
	gfx.endWrite();
	gfx.waitDisplay();

	// 初回起動時処理
	if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
		if (wifi_connecting()) {
			syncNTPTimeJP();
			updateWeatherToRTC();
			did_update_wifi = true;
		}
	}

	update_cnt++;
	cnt++;

	// 定期天気更新
	if (update_cnt >= UPDATE_SYNC_CYCLE && !did_update_wifi) {
		if (wifi_connecting()) {
			updateWeatherToRTC();
			did_update_wifi = true;
		}
		update_cnt = 0;
	}

	// 定期NTP同期
	if (cnt >= TIME_SYNC_CYCLE && !did_update_wifi) {
		if (wifi_connecting()) {
			syncNTPTimeJP();
			did_update_wifi = true;
		}
		cnt = 0;
	}

	WiFi.mode(WIFI_OFF);
	btStop();

	float tmp = 0.0;
	uint_fast8_t hum = 0;
	if (!sht30.read()) {
		tmp = sht30.getTemperature();
		hum = sht30.getHumidity();
	}

	rtc_date_t date;
	rtc_time_t time;
	M5.RTC.getTime(&time);
	M5.RTC.getDate(&date);

	// Wi-Fi通信に成功した時のみ、取得した時間を数値としてRTCメモリに保存
	if (did_update_wifi) {
		rtc_latest_update_hour = time.hour;
		rtc_latest_update_min = time.min;
	}

	gfx.startWrite();
	gfx.fillScreen(TFT_WHITE);
	gfx.fillRect(0.57 * M5PAPER_SIZE_LONG_SIDE, 0, 3, M5PAPER_SIZE_SHORT_SIDE, TFT_BLACK);

	constexpr uint_fast16_t offset_y = 30;
	constexpr uint_fast16_t offset_x = 45;

	gfx.setCursor(0, offset_y);
	gfx.setTextSize(FONT_SIZE_GIANT);
	gfx.setClipRect(offset_x, offset_y, M5PAPER_SIZE_LONG_SIDE - offset_x,
					M5PAPER_SIZE_SHORT_SIDE - offset_y);

	gfx.printf("%02d:%02d\r\n", time.hour, time.min);
	gfx.setTextSize(FONT_SIZE_MIDDLE);
	gfx.setCursor(0, offset_y + 195);

	gfx.printf("    %s\r\n", rtc_weather_text);
	gfx.printf("    %02.1f℃\r\n", rtc_weather_temp);
	gfx.setTextSize(FONT_SIZE_SMALL);
	gfx.printf("\r\n");

	if (rtc_weather_success && strlen(rtc_weather_icon) > 0) {
		String icon_path = String("/weather_icons/") + rtc_weather_icon + "@2x.png";
		gfx.drawPngFile(SPIFFS, icon_path.c_str(), 20, offset_y + 180, 0, 0, 0, 0, 2.0, 2.0);
	}

	gfx.setTextSize(FONT_SIZE_MIDDLE);
	gfx.printf("%02d%% %02.1f℃\r\n", hum, tmp);
	gfx.setTextSize(FONT_SIZE_SMALL);

	// バグ防止：保存された「数値」を使って表示する
	if (rtc_latest_update_hour != -1) {
		gfx.printf("latest update : %02d:%02d\r\n", rtc_latest_update_hour, rtc_latest_update_min);
	} else {
		gfx.printf("latest update : --:--\r\n");
	}

	gfx.clearClipRect();

	vol = std::min(std::max(M5.getBatteryVoltage(), bt_low), bt_high);
	bt_per = get_battery_percentage(vol);
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
	gfx.println(did_update_wifi ? "UPDATED" : "SLEEP");
	gfx.printf("BAT : %04dmv\r\n", vol);

	gfx.print("NTP : ");
	if (rtc_ntp_year == 1970) {
		gfx.print("YET");
	} else {
		gfx.printf("%02d/%02d %02d:%02d", rtc_ntp_mon, rtc_ntp_day, rtc_ntp_hour, rtc_ntp_min);
	}
	gfx.clearClipRect();
	gfx.endWrite();

	// 描画が完全に終わるのを待機
	gfx.waitDisplay();
	delay(100);
	Serial.println("Going to sleep now");

	Serial.flush();
	M5.disableEPDPower();
	M5.disableEXTPower();

	esp_sleep_enable_timer_wakeup(SLEEP_SEC * 1000000ULL);
	// esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0);
	esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);
	// esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
	// esp_sleep_enable_ext0_wakeup(GPIO_NUM_39, 0);

	gpio_hold_en(GPIO_NUM_2);
	gpio_deep_sleep_hold_en();

	esp_deep_sleep_start();
}

void loop(void) {}