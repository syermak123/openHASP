/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#include "hasplib.h"

#if defined(HASP_USE_CUSTOM) && HASP_USE_CUSTOM > 0

#include <HTTPClient.h>

#if defined(ESP32)
#include <WiFiClientSecure.h>
#include "sys/net/hasp_network.h"
#endif

static constexpr float kLat = 50.386944f;  // Vyshneve: 50°23′13″N
static constexpr float kLon = 30.358056f;  // Vyshneve: 30°21′29″E

static constexpr uint32_t kUpdatePeriodSeconds = 300; // 5 minutes

// Page/objects to update:
// - p1b20: temperature (e.g. "12.3°C")
// - p1b21: condition text (e.g. "Cloudy")
// - p1b22: wind (e.g. "Wind 4.1 m/s")
// - p1b23: updated time (e.g. "Updated 12:34")
static constexpr uint8_t kWeatherPage = 1;
static constexpr uint8_t kObjTemp     = 20;
static constexpr uint8_t kObjCond     = 21;
static constexpr uint8_t kObjWind     = 22;
static constexpr uint8_t kObjUpdated  = 23;
// Daily forecast (today/tomorrow)
static constexpr uint8_t kObjToday    = 30;
static constexpr uint8_t kObjTomorrow = 31;

static uint32_t g_seconds = 0;
static uint32_t g_lastFetchAt = 0;

static const __FlashStringHelper* weather_code_to_text(int code)
{
    // Minimal WMO mapping for readability
    switch(code) {
        case 0: return F("Clear");
        case 1: return F("Mainly clear");
        case 2: return F("Partly cloudy");
        case 3: return F("Overcast");
        case 45: return F("Fog");
        case 48: return F("Rime fog");
        case 51: return F("Drizzle");
        case 53: return F("Drizzle");
        case 55: return F("Drizzle");
        case 56: return F("Freezing drizzle");
        case 57: return F("Freezing drizzle");
        case 61: return F("Rain");
        case 63: return F("Rain");
        case 65: return F("Heavy rain");
        case 66: return F("Freezing rain");
        case 67: return F("Freezing rain");
        case 71: return F("Snow");
        case 73: return F("Snow");
        case 75: return F("Heavy snow");
        case 77: return F("Snow grains");
        case 80: return F("Rain showers");
        case 81: return F("Rain showers");
        case 82: return F("Heavy showers");
        case 85: return F("Snow showers");
        case 86: return F("Snow showers");
        case 95: return F("Thunderstorm");
        case 96: return F("Thunderstorm hail");
        case 99: return F("Thunderstorm hail");
        default: return F("Unknown");
    }
}

static void set_obj_text(uint8_t page, uint8_t id, const String& text)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "p%db%d.text=%s", page, id, text.c_str());
    dispatch_topic_payload("", cmd, true, TAG_CUSTOM);
}

static void set_obj_text(uint8_t page, uint8_t id, const __FlashStringHelper* text)
{
    set_obj_text(page, id, String(text));
}

static void set_status_line(const String& msg)
{
    set_obj_text(kWeatherPage, kObjUpdated, msg);
}

static bool fetch_and_update_one(const String& url, bool is_tls)
{
    HTTPClient http;

#if defined(ESP32)
    WiFiClientSecure tls;
    if(is_tls) {
        tls.setInsecure(); // avoid CA bundle dependency
        tls.setTimeout(6000);
        if(!http.begin(tls, url)) {
            set_status_line("Weather: begin(tls) failed");
            return false;
        }
    } else {
        if(!http.begin(url)) {
            set_status_line("Weather: begin(http) failed");
            return false;
        }
    }
#else
    if(!http.begin(url)) {
        set_status_line("Weather: HTTP begin failed");
        return false;
    }
#endif

    http.setTimeout(6000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    const int code = http.GET();
    if(code < 0) {
        set_status_line(String("Weather: ") + http.errorToString(code));
        http.end();
        return false;
    }
    if(code != HTTP_CODE_OK) {
        set_status_line(String("Weather HTTP: ") + String(code) + " " + http.errorToString(code));
        http.end();
        return false;
    }

    const String body = http.getString();
    http.end();

    // Keep doc reasonably small; Open-Meteo current payload is compact
    DynamicJsonDocument doc(4096);
    const DeserializationError err = deserializeJson(doc, body);
    if(err) {
        set_status_line(String("Weather JSON: ") + err.c_str());
        return false;
    }

    const JsonVariant current = doc["current"];
    if(current.isNull()) {
        set_status_line("Weather: no 'current'");
        return false;
    }

    const float temp = current["temperature_2m"] | NAN;
    const int wcode  = current["weather_code"] | -1;
    const float wind = current["wind_speed_10m"] | NAN;

    // Format
    char tbuf[24];
    if(isnan(temp)) {
        snprintf(tbuf, sizeof(tbuf), "--.-°C");
    } else {
        snprintf(tbuf, sizeof(tbuf), "%.1f°C", temp);
    }

    char wbuf[32];
    if(isnan(wind)) {
        snprintf(wbuf, sizeof(wbuf), "Wind --.- m/s");
    } else {
        snprintf(wbuf, sizeof(wbuf), "Wind %.1f m/s", wind);
    }

    set_obj_text(kWeatherPage, kObjTemp, String(tbuf));
    set_obj_text(kWeatherPage, kObjCond, weather_code_to_text(wcode));
    set_obj_text(kWeatherPage, kObjWind, String(wbuf));

    // Daily forecast (today/tomorrow)
    const JsonVariant daily = doc["daily"];
    if(!daily.isNull()) {
        const JsonArray codes = daily["weather_code"].as<JsonArray>();
        const JsonArray tmax  = daily["temperature_2m_max"].as<JsonArray>();
        const JsonArray tmin  = daily["temperature_2m_min"].as<JsonArray>();

        auto format_day = [&](uint8_t idx, const __FlashStringHelper* prefix, uint8_t objId) {
            if(codes.isNull() || tmax.isNull() || tmin.isNull()) return;
            if(idx >= codes.size() || idx >= tmax.size() || idx >= tmin.size()) return;

            const int dcode   = codes[idx] | -1;
            const float dmax  = tmax[idx] | NAN;
            const float dminv = tmin[idx] | NAN;

            char buf[96];
            if(isnan(dmax) || isnan(dminv)) {
                snprintf(buf, sizeof(buf), "%s: --/--  %s", String(prefix).c_str(),
                         String(weather_code_to_text(dcode)).c_str());
            } else {
                snprintf(buf, sizeof(buf), "%s: %.0f/%.0f°C  %s", String(prefix).c_str(), dmax, dminv,
                         String(weather_code_to_text(dcode)).c_str());
            }
            set_obj_text(kWeatherPage, objId, String(buf));
        };

        format_day(0, F("Today"), kObjToday);
        format_day(1, F("Tomorrow"), kObjTomorrow);
    }

    // Local time from device NTP if configured
    struct tm timeinfo;
    if(getLocalTime(&timeinfo, 10)) {
        char ubuf[32];
        strftime(ubuf, sizeof(ubuf), "Updated %H:%M", &timeinfo);
        set_obj_text(kWeatherPage, kObjUpdated, String(ubuf));
    } else {
        set_obj_text(kWeatherPage, kObjUpdated, F("Updated"));
    }

    return true;
}

static bool fetch_and_update()
{
#if defined(ESP32)
    if(!network_is_connected()) {
        set_status_line("Weather: offline");
        return false;
    }
#endif

    const String qs = String("api.open-meteo.com/v1/forecast?latitude=") + String(kLat, 6) + "&longitude=" +
                      String(kLon, 6) +
                      "&current=temperature_2m,weather_code,wind_speed_10m"
                      "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                      "&forecast_days=2"
                      "&timezone=auto";

    // Prefer TLS, but provide a plain HTTP fallback to diagnose networks that block 443/TLS
    if(fetch_and_update_one(String("https://") + qs, true)) return true;
    return fetch_and_update_one(String("http://") + qs, false);
}

void custom_setup()
{
    // Start with placeholders so the UI shows something immediately
    set_obj_text(kWeatherPage, kObjTemp, F("--.-°C"));
    set_obj_text(kWeatherPage, kObjCond, F("Weather"));
    set_obj_text(kWeatherPage, kObjWind, F("Wind --.- m/s"));
    set_obj_text(kWeatherPage, kObjUpdated, F("Updating..."));
    set_obj_text(kWeatherPage, kObjToday, F("Today: --/--"));
    set_obj_text(kWeatherPage, kObjTomorrow, F("Tomorrow: --/--"));

    // First fetch after boot (give WiFi time to connect)
    g_seconds = 0;
    g_lastFetchAt = 0;
}

void custom_loop()
{
    // keep empty: do not block here
}

void custom_every_second()
{
    g_seconds++;

    // Wait a bit after boot, then refresh periodically
    if(g_seconds < 8) return;
    if(g_lastFetchAt != 0 && (g_seconds - g_lastFetchAt) < kUpdatePeriodSeconds) return;

    g_lastFetchAt = g_seconds;
    fetch_and_update();
}

void custom_every_5seconds()
{
    // not used
}

bool custom_pin_in_use(uint8_t)
{
    return false;
}

void custom_get_sensors(JsonDocument&)
{
    // not used
}

void custom_topic_payload(const char*, const char*, uint8_t)
{
    // not used
}

void custom_state_subtopic(const char*, const char*)
{
    // not used
}

#endif // HASP_USE_CUSTOM

