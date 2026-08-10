#include "SPIFFS.h"

#include <M5Unified.h>
#include <array>

#include "SHT3X.h"
#include "WiFiInfo.h"
#include "misc.h"

constexpr float FONT_SIZE_GIANT = 17.0;
constexpr float FONT_SIZE_LARGE = 10.0;
constexpr float FONT_SIZE_MIDDLE = 8.0;
constexpr float FONT_SIZE_SMALL = 4.0;
constexpr uint_fast16_t M5PAPER_SIZE_LONG_SIDE = 960;
constexpr uint_fast16_t M5PAPER_SIZE_SHORT_SIDE = 540;

// RTC/Deep Sleep変数
RTC_DATA_ATTR uint32_t update_cnt = 0;
RTC_DATA_ATTR uint32_t cnt = 0;

RTC_DATA_ATTR int16_t rtc_ntp_year = 1970;
RTC_DATA_ATTR int8_t rtc_ntp_mon = 1;
RTC_DATA_ATTR int8_t rtc_ntp_day = 1;
RTC_DATA_ATTR int8_t rtc_ntp_hour = 0;
RTC_DATA_ATTR int8_t rtc_ntp_min = 0;

RTC_DATA_ATTR int8_t rtc_latest_update_hour = -1;
RTC_DATA_ATTR int8_t rtc_latest_update_min = -1;

RTC_DATA_ATTR bool rtc_weather_success = false;
RTC_DATA_ATTR char rtc_weather_text[64] = "";
RTC_DATA_ATTR char rtc_weather_icon[32] = "";
RTC_DATA_ATTR float rtc_weather_temp = 0.0;

SHT3X::SHT3X sht30;

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
		m5::rtc_time_t time{static_cast<int8_t>(datetime.tm_hour),
							static_cast<int8_t>(datetime.tm_min),
							static_cast<int8_t>(datetime.tm_sec)};

		// 順序を {year, month, date, weekDay} に修正
		m5::rtc_date_t date{
			static_cast<int16_t>(datetime.tm_year + 1900), static_cast<int8_t>(datetime.tm_mon + 1),
			static_cast<int8_t>(datetime.tm_mday), static_cast<int8_t>(datetime.tm_wday)};

		M5.Rtc.setTime(time);
		M5.Rtc.setDate(date);

		rtc_ntp_year = date.year;
		rtc_ntp_mon = date.month;
		rtc_ntp_day = date.date;
		rtc_ntp_hour = time.hours;
		rtc_ntp_min = time.minutes;
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

void drawBattery(int percentage) {
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

	M5.Display.fillRect(x - 5, y - 5, totalWidth + 10, iconHeight + 35, TFT_WHITE);
	M5.Display.drawRect(x, y, iconWidth, iconHeight, TFT_BLACK);
	M5.Display.drawRect(x + 1, y + 1, iconWidth - 2, iconHeight - 2, TFT_BLACK);
	M5.Display.fillRect(x + iconWidth, y + (iconHeight / 4), capWidth, iconHeight / 2, TFT_BLACK);

	int32_t innerMargin = border + 1;
	int32_t maxInnerWidth = iconWidth - (innerMargin * 2);
	int32_t innerHeight = iconHeight - (innerMargin * 2);
	int32_t barWidth = (maxInnerWidth * percentage) / 100;

	if (percentage > 0 && barWidth < 2) barWidth = 2;
	M5.Display.fillRect(x + innerMargin, y + innerMargin, barWidth, innerHeight, TFT_BLACK);

	M5.Display.setTextSize(FONT_SIZE_SMALL);
	M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
	M5.Display.setCursor(x - 10, y + iconHeight + 2);
	M5.Display.printf("%3d%%", percentage);
}

void setup(void) {
	constexpr uint_fast16_t SLEEP_SEC = 60;
	constexpr uint_fast32_t TIME_SYNC_CYCLE = (3600 * 24) / SLEEP_SEC;
	constexpr uint_fast32_t UPDATE_SYNC_CYCLE = (3600 * 12) / SLEEP_SEC;
	bool did_update_wifi = false;

	auto cfg = M5.config();
	M5.begin(cfg);
	setCpuFrequencyMhz(80);

	M5.Display.setEpdMode(m5gfx::epd_mode_t::epd_fast);
	M5.Display.setRotation(1);
	// M5.Display.setFont(&myFont::myFont);

	SPIFFS.begin();
	sht30.begin(21, 22, 400000);

	esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

	M5.Display.startWrite();
	M5.Display.fillScreen(TFT_WHITE);
	uint32_t vol = std::min(std::max((uint32_t)M5.Power.getBatteryVoltage(), bt_low), bt_high);
	uint32_t bt_per = get_battery_percentage(vol);
	drawBattery(bt_per);
	M5.Display.endWrite();
	M5.Display.waitDisplay();

	if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
		if (wifi_connecting()) {
			syncNTPTimeJP();
			updateWeatherToRTC();
			did_update_wifi = true;
		}
	}

	update_cnt++;
	cnt++;

	if (update_cnt >= UPDATE_SYNC_CYCLE && !did_update_wifi) {
		if (wifi_connecting()) {
			updateWeatherToRTC();
			did_update_wifi = true;
		}
		update_cnt = 0;
	}

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

	auto rtc_dt = M5.Rtc.getDateTime();

	if (did_update_wifi) {
		rtc_latest_update_hour = rtc_dt.time.hours;
		rtc_latest_update_min = rtc_dt.time.minutes;
	}

	M5.Display.startWrite();
	M5.Display.fillScreen(TFT_WHITE);
	M5.Display.fillRect(0.57 * M5PAPER_SIZE_LONG_SIDE, 0, 3, M5PAPER_SIZE_SHORT_SIDE, TFT_BLACK);

	constexpr uint_fast16_t offset_y = 30;
	constexpr uint_fast16_t offset_x = 20;//45;

	M5.Display.setCursor(0, offset_y);
	M5.Display.setTextSize(FONT_SIZE_GIANT);
	M5.Display.setClipRect(offset_x, offset_y, M5PAPER_SIZE_LONG_SIDE - offset_x,
						   M5PAPER_SIZE_SHORT_SIDE - offset_y);

	M5.Display.printf("%02d:%02d\r\n", rtc_dt.time.hours, rtc_dt.time.minutes);
	M5.Display.setTextSize(FONT_SIZE_MIDDLE);
	M5.Display.setCursor(0, offset_y + 195);

	M5.Display.printf("    %s\r\n", rtc_weather_text);
	M5.Display.printf("    %02.1f℃\r\n", rtc_weather_temp);
	M5.Display.setTextSize(FONT_SIZE_SMALL);
	M5.Display.printf("\r\n");

	if (rtc_weather_success && strlen(rtc_weather_icon) > 0) {
		String icon_path = String("/weather_icons/") + rtc_weather_icon + "@2x.png";
		// キャストなしで SPIFFS をそのまま渡す
		M5.Display.drawPngFile(SPIFFS, icon_path.c_str(), 20, offset_y + 180, 0, 0, 0, 0, 2.0, 2.0);
	}

	M5.Display.setTextSize(FONT_SIZE_MIDDLE);
	M5.Display.printf("%02d%% %02.1f℃\r\n", hum, tmp);
	M5.Display.setTextSize(FONT_SIZE_SMALL);

	if (rtc_latest_update_hour != -1) {
		M5.Display.printf("latest update : %02d:%02d\r\n", rtc_latest_update_hour,
						  rtc_latest_update_min);
	} else {
		M5.Display.printf("latest update : --:--\r\n");
	}

	M5.Display.clearClipRect();

	vol = std::min(std::max((uint32_t)M5.Power.getBatteryVoltage(), bt_low), bt_high);
	bt_per = get_battery_percentage(vol);
	drawBattery(bt_per);

	constexpr float x = 0.61 * M5PAPER_SIZE_LONG_SIDE;
	M5.Display.setTextSize(FONT_SIZE_LARGE);
	M5.Display.setCursor(0, offset_y);
	M5.Display.setClipRect(x, offset_y, M5PAPER_SIZE_LONG_SIDE - offset_x - x,
						   M5PAPER_SIZE_SHORT_SIDE - offset_y);
	M5.Display.printf("%04d\r\n", rtc_dt.date.year);
	M5.Display.printf("%02d/%02d\r\n", rtc_dt.date.month, rtc_dt.date.date);
	M5.Display.println(weekdayToString(rtc_dt.date.weekDay));
	M5.Display.clearClipRect();

	constexpr float offset_y_info = 0.75 * M5PAPER_SIZE_SHORT_SIDE;
	M5.Display.setCursor(0, offset_y_info);
	M5.Display.setTextSize(FONT_SIZE_SMALL);
	M5.Display.setClipRect(x, offset_y_info, M5PAPER_SIZE_LONG_SIDE - x,
						   M5.Display.height() - offset_y_info);

	M5.Display.print("WiFi: ");
	M5.Display.println(did_update_wifi ? "UPDATED" : "SLEEP");
	M5.Display.printf("BAT : %04dmv\r\n", vol);

	M5.Display.print("NTP : ");
	if (rtc_ntp_year == 1970) {
		M5.Display.print("YET");
	} else {
		M5.Display.printf("%02d/%02d %02d:%02d", rtc_ntp_mon, rtc_ntp_day, rtc_ntp_hour,
						  rtc_ntp_min);
	}
	M5.Display.clearClipRect();
	M5.Display.endWrite();

	M5.Display.waitDisplay();
	delay(100);
	Serial.println("Going to sleep now");

	Serial.flush();

	M5.Power.setExtOutput(false);

	esp_sleep_enable_timer_wakeup(SLEEP_SEC * 1000000ULL);
	esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);

	gpio_hold_en(GPIO_NUM_2);
	gpio_deep_sleep_hold_en();

	esp_deep_sleep_start();
}

void loop(void) {}