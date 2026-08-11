#pragma once
#include <M5EPD.h>
// #include <FastLED.h>
#include <HTTPClient.h>

#include <ArduinoJson.hpp>
#include <functional>

#include "esp_sntp.h"

inline String WiFiConnectedToString(void) {
	return WiFi.isConnected() ? String("CONNECTED") : String("SLEEP");
}

String weekdayToString(const int8_t weekDay) {
	switch (weekDay) {
		case 0:
			return String("Sun");
		case 1:
			return String("Mon");
		case 2:
			return String("Tue");
		case 3:
			return String("Wed");
		case 4:
			return String("Thu");
		case 5:
			return String("Fri");
		case 6:
			return String("Sat");
	}
	// switch (weekDay) {
	// 	case 0:
	// 		return String("日");
	// 	case 1:
	// 		return String("月");
	// 	case 2:
	// 		return String("火");
	// 	case 3:
	// 		return String("水");
	// 	case 4:
	// 		return String("木");
	// 	case 5:
	// 		return String("金");
	// 	case 6:
	// 		return String("土");
	// }
	return String("");
}

struct weather_t {
	String weather;
	String icon;
	double temperature;
	bool success;
};
weather_t get_weather(void) {
	using namespace ArduinoJson;
	weather_t weather_data;
	weather_data.success = false;
	if (!WiFi.isConnected()) return weather_data;
	constexpr uint16_t HTTP_TIMEOUT = 3000;
	HTTPClient http;

	http.begin(WeatherInfo::API_URL);  // URLを指定
	int httpCode = http.GET();		   // GETリクエストを送信

	if (httpCode > 0) {	 // 返答がある場合

		String payload = http.getString();	// 返答（JSON形式）を取得
		Serial.println(httpCode);
		Serial.println(payload);

		// jsonオブジェクトの作成
		// DynamicJsonDocument doc(1024);
		JsonDocument doc;
		auto err = deserializeJson(doc, payload);
		// パースが成功したかどうかを確認
		if (err) {
			Serial.printf("[JSON] parseObject() failed, error: %s\n", err.c_str());
			return weather_data;
		}
		// 各データを抜き出し
		weather_data.weather = doc["weather"][0]["main"].as<String>();
		weather_data.icon = doc["weather"][0]["icon"].as<String>();
		weather_data.temperature = doc["main"]["temp"].as<double>() - 273.15;
		Serial.print("weather:");
		Serial.println(weather_data.weather);
		Serial.print("temperature:");
		Serial.println(weather_data.temperature);
		weather_data.success = true;
		return weather_data;
	}

	else {
		Serial.println("Error on HTTP request");
	}
	http.end();	 // リソースを解放
	return weather_data;
}

inline void prettyEpdRefresh(LGFX& gfx) {
	gfx.setEpdMode(epd_mode_t::epd_quality);
	gfx.fillScreen(TFT_WHITE);
	gfx.setEpdMode(epd_mode_t::epd_fast);
}

int syncNTPTime(std::function<void(const tm&)> datetimeSetter, const char* tz, const char* server1,
				const char* server2 = nullptr, const char* server3 = nullptr) {
	if (!WiFi.isConnected()) {
		return 1;
	}

	configTzTime(tz, server1, server2, server3);

	// https://github.com/espressif/esp-idf/blob/master/examples/protocols/sntp/main/sntp_example_main.c
	int retry = 0;
	constexpr int retry_count = 50;
	while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && ++retry < retry_count) {
		delay(100);
	}
	if (retry == retry_count) {
		return 1;
	}

	struct tm datetime;
	if (!getLocalTime(&datetime)) return 1;

	datetimeSetter(datetime);

	return 0;
}