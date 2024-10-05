#pragma once
#include <Arduino.h>

namespace WiFiInfo
{
  constexpr auto SSID = "TP-Link_6FAB";
  constexpr auto PASS = "30994133";
} // namespace WiFiInfo

namespace WeatherInfo
{
  static const String CITY = "toyohashi"; // "Tokyo,jp"
  static const String API_KEY = "31abb09e5d4b51e798fa7dc2ea43a6aa";
  static const String API_URL = "http://api.openweathermap.org/data/2.5/weather?q=" + CITY + "&APPID=" + API_KEY;
} // namespace WiFiInfo