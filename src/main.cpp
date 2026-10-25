// Quanta Controller firmware — ESP32, 4 x 3.5 mm jacks, 8 x 0-10 V outputs, web GUI.
//
// Reliability rules this file is built around:
//  1. The schedule runs entirely on the box from a battery-backed clock. No internet, phone or
//     cloud is needed for the lights to do the right thing.
//  2. At power-up the outputs are set from the schedule within the first second, BEFORE Wi-Fi
//     starts, then fade up gently. Wi-Fi problems can never delay or change the lights.
//  3. The DACs hold their voltage while the ESP32 restarts (watchdog, update, crash), so a
//     software restart doesn't blink the lights.
//  4. Settings are saved in two copies with a checksum. A power cut during a save keeps the
//     previous copy. Anything loaded is validated and clamped before use.
//  5. New firmware must run cleanly for a minute or the box rolls back to the old version.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

#include "config.h"
#include "core.h"
#include "hal.h"
#include "web_ui.h"
#include "Ota.h"
#include "provision_page.h"
#include <vector>
#include <algorithm>

using namespace qc;

#define FW_VERSION "P-0.0.1"

// ======================================================================= state
static Config   cfg;
static Override ovr;
static Preferences prefs;
static uint32_t cfgGen = 0;

static hal::Mcp4728 dacA(ADDR_DAC_A), dacB(ADDR_DAC_B);
static hal::MonitorAdc adc;
static hal::Ds3231  rtc;
static bool pwmMode = false;                   // bench prototype: no DAC chips, drive PWM modules
static const uint8_t pwmPins[CHANNELS] = PWM_PINS;
static bool outputsOk() { return pwmMode || (dacA.ok && dacB.ok); }

static uint16_t curLvl[CHANNELS];              // what each output shows now (permille)
static uint16_t tgtLvl[CHANNELS];
static uint16_t codeOut[CHANNELS];
static uint16_t grpLvl[MAX_GROUPS];
static uint8_t  modeNow = MODE_AUTO;
static bool     rampActive = false;            // cold-boot soft start in progress
static OutReading outRd[CHANNELS];
static int16_t  ampMv[CHANNELS];

enum TimeSrc : uint8_t { TS_NONE = 0, TS_RTC, TS_NTP, TS_PHONE, TS_ESTIMATED };
static uint8_t  timeSrc = TS_NONE;
static uint32_t lastTimeSync = 0;              // epoch of last NTP/phone sync

static int8_t   identifyJack = -1;
static uint32_t identifyUntilMs = 0;
static uint32_t ovrStartMs = 0;                // for ending manual/picture mode if the clock is unknown

static uint32_t bootMs = 0, boots = 0, lastOutageStart = 0, lastOutageEnd = 0;
static esp_reset_reason_t resetReason;
static float    todayWh = 0, yesterdayWh = 0;
static int      todayYday = -1;

static WebServer server(80);
static OtaManager ota(FW_VERSION);
static String otaUrl;
static bool   otaAuto = true;
static uint8_t otaRequest = 0;                 // 1 = check now, 2 = check and install (from the page)
static DNSServer dns;
static bool apOn = false;
static uint32_t apUntilMs = 0;                 // 0 = keep on while needed
static String staSsid, staPass;
static bool provActive = false;               // Wi-Fi setup page is connecting
static uint32_t provStartMs = 0;


// ======================================================================= events (small persistent log)
enum EvCode : uint8_t { EV_BOOT = 1, EV_OUTAGE, EV_TIME, EV_MODE, EV_CONFIG, EV_CONFIG_RECOVERED,
                        EV_FAULT, EV_FAULT_CLEAR, EV_UPDATE, EV_WIFI, EV_CLOCK_LOST, EV_HW };
struct Event { uint32_t t; uint8_t code, a; uint16_t b; };
constexpr uint8_t EV_N = 40;
static Event evlog[EV_N];
static uint8_t evHead = 0;

// ======================================================================= time helpers
static uint32_t nowEpoch() { return (uint32_t)time(nullptr); }
static bool timeValid() { return timeSrc != TS_NONE; }
static uint32_t secOfDay() {
  time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
  return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
}
static void setSystemTime(uint32_t e) { struct timeval tv = {(time_t)e, 0}; settimeofday(&tv, nullptr); }
static void applyTz() { setenv("TZ", cfg.tz, 1); tzset(); }

static void logEvent(uint8_t code, uint8_t a = 0, uint16_t b = 0) {
  evlog[evHead] = {timeValid() ? nowEpoch() : 0, code, a, b};
  evHead = (evHead + 1) % EV_N;
  prefs.putBytes("ev", evlog, sizeof(evlog));
  prefs.putUChar("evh", evHead);
}

// ======================================================================= config <-> JSON
static void configToJson(JsonDocument &d) {
  JsonArray ga = d["groups"].to<JsonArray>();
  for (uint8_t g = 0; g < cfg.nGroups; g++) {
    JsonObject o = ga.add<JsonObject>();
    o["name"] = cfg.group[g].name; o["color"] = cfg.group[g].color;
    JsonArray pa = o["points"].to<JsonArray>();
    for (uint8_t i = 0; i < cfg.group[g].n; i++) {
      JsonArray p = pa.add<JsonArray>(); p.add(cfg.group[g].p[i].min); p.add(cfg.group[g].p[i].lvl);
    }
  }
  JsonArray ja = d["jacks"].to<JsonArray>();
  for (uint8_t j = 0; j < JACKS; j++) {
    JsonObject o = ja.add<JsonObject>();
    o["fixture"] = fixtureId(cfg.jack[j].fixture); o["count"] = cfg.jack[j].count; o["name"] = cfg.jack[j].name;
    JsonArray w = o["watts"].to<JsonArray>(); w.add(cfg.jack[j].watts[0]); w.add(cfg.jack[j].watts[1]);
    o["len"] = cfg.jack[j].len;
    JsonArray ca = o["ch"].to<JsonArray>();
    for (uint8_t s = 0; s < 2; s++) {
      const Channel &c = cfg.ch[j * 2 + s];
      JsonObject co = ca.add<JsonObject>(); co["group"] = c.group; co["trim"] = c.trim; co["cal"] = c.cal;
    }
  }
  JsonArray pa = d["photos"].to<JsonArray>();
  for (uint8_t p = 0; p < cfg.nPhoto; p++) {
    JsonObject o = pa.add<JsonObject>();
    o["name"] = cfg.photo[p].name; o["minutes"] = cfg.photo[p].minutes;
    JsonArray l = o["lvl"].to<JsonArray>(); for (uint8_t g = 0; g < cfg.nGroups; g++) l.add(cfg.photo[p].lvl[g]);
  }
  d["intensity"] = cfg.intensity; d["floor"] = cfg.floorPct; d["rampSec"] = cfg.rampSec; d["fadeSec"] = cfg.fadeSec;
  JsonObject a = d["accl"].to<JsonObject>();
  a["on"] = cfg.acclOn; a["startPct"] = cfg.acclStartPct; a["days"] = cfg.acclDays; a["start"] = cfg.acclStart;
  d["tz"] = cfg.tz; d["expert"] = cfg.expert; d["setupDone"] = cfg.setupDone; d["costMils"] = cfg.costMils;
  d["tank"] = cfg.tank; d["spectrum"] = cfg.spectrum; d["off"] = cfg.offMin;
}

// Merge: only keys present in `d` change. Result is always sanitized.
static void configFromJson(JsonVariantConst d, Config &c) {
  if (d["groups"].is<JsonArrayConst>()) {
    JsonArrayConst ga = d["groups"];
    c.nGroups = 0;
    for (JsonObjectConst o : ga) {
      if (c.nGroups >= MAX_GROUPS) break;
      Group &G = c.group[c.nGroups++];
      copyName(G.name, o["name"] | "Group");
      G.color = (uint8_t)clampi(o["color"] | 0, 0, COL_COUNT - 1);
      G.n = 0;
      for (JsonArrayConst p : o["points"].as<JsonArrayConst>()) {
        if (G.n >= MAX_POINTS) break;
        G.p[G.n++] = {(uint16_t)clampi(p[0] | 0, 0, 1439), (uint16_t)clampi(p[1] | 0, 0, LEVEL_MAX)};
      }
    }
  }
  if (d["jacks"].is<JsonArrayConst>()) {
    uint8_t j = 0;
    for (JsonObjectConst o : d["jacks"].as<JsonArrayConst>()) {
      if (j >= JACKS) break;
      if (o["fixture"].is<const char *>()) c.jack[j].fixture = fixtureFromId(o["fixture"].as<const char *>());
      if (o["len"].is<int>()) c.jack[j].len = (uint8_t)clampi(o["len"], 0, HELIX_LENS - 1);
      if (o["count"].is<int>()) c.jack[j].count = (uint8_t)clampi(o["count"], 1, 8);
      if (o["name"].is<const char *>()) copyName(c.jack[j].name, o["name"].as<const char *>());
      if (o["watts"].is<JsonArrayConst>()) for (uint8_t s = 0; s < 2; s++) c.jack[j].watts[s] = (uint16_t)clampi(o["watts"][s] | 0, 0, 1000);
      if (o["ch"].is<JsonArrayConst>()) for (uint8_t s = 0; s < 2; s++) {
        JsonObjectConst co = o["ch"][s];
        if (co.isNull()) continue;
        Channel &ch = c.ch[j * 2 + s];
        if (co["group"].is<int>()) ch.group = (int8_t)clampi(co["group"], -1, MAX_GROUPS - 1);
        if (co["trim"].is<int>()) ch.trim = (uint8_t)clampi(co["trim"], 0, 100);
        if (co["cal"].is<int>()) ch.cal = (uint8_t)clampi(co["cal"], 80, 120);
      }
      j++;
    }
  }
  if (d["photos"].is<JsonArrayConst>()) {
    c.nPhoto = 0;
    for (JsonObjectConst o : d["photos"].as<JsonArrayConst>()) {
      if (c.nPhoto >= MAX_PHOTO) break;
      Photo &P = c.photo[c.nPhoto++];
      copyName(P.name, o["name"] | "Picture");
      P.minutes = overrideMinutes(o["minutes"] | 30);
      memset(P.lvl, 0, sizeof(P.lvl));
      uint8_t g = 0;
      for (JsonVariantConst v : o["lvl"].as<JsonArrayConst>()) { if (g < MAX_GROUPS) P.lvl[g++] = (uint16_t)clampi(v | 0, 0, LEVEL_MAX); }
    }
  }
  if (d["intensity"].is<int>()) c.intensity = (uint8_t)clampi(d["intensity"], 10, 100);
  if (d["floor"].is<int>()) c.floorPct = (uint8_t)clampi(d["floor"], 0, 40);
  if (d["rampSec"].is<int>()) c.rampSec = (uint16_t)clampi(d["rampSec"], 0, 1800);
  if (d["fadeSec"].is<int>()) c.fadeSec = (uint16_t)clampi(d["fadeSec"], 0, 60);
  if (d["accl"].is<JsonObjectConst>()) {
    JsonObjectConst a = d["accl"];
    if (a["on"].is<bool>()) c.acclOn = a["on"];
    if (a["startPct"].is<int>()) c.acclStartPct = (uint8_t)clampi(a["startPct"], 10, 100);
    if (a["days"].is<int>()) c.acclDays = (uint16_t)clampi(a["days"], 1, 120);
    if (a["start"].is<uint32_t>()) c.acclStart = a["start"];
  }
  if (d["tz"].is<const char *>()) { strncpy(c.tz, d["tz"].as<const char *>(), sizeof(c.tz) - 1); c.tz[sizeof(c.tz) - 1] = 0; }
  if (d["expert"].is<bool>()) c.expert = d["expert"];
  if (d["setupDone"].is<bool>()) c.setupDone = d["setupDone"];
  if (d["costMils"].is<int>()) c.costMils = (uint16_t)clampi(d["costMils"], 0, 5000);
  if (d["tank"].is<int>()) c.tank = (uint8_t)clampi(d["tank"], 0, TANK_COUNT - 1);
  if (d["spectrum"].is<int>()) c.spectrum = (uint8_t)clampi(d["spectrum"], 0, SP_COUNT - 1);
  if (d["off"].is<int>()) c.offMin = (uint16_t)clampi(d["off"], 0, 1439);
  sanitize(c);
}

// ======================================================================= persistence (A/B slots)
// Slot layout: [gen u32][crc u32][json bytes]
static bool loadSlot(const char *key, Config &c, uint32_t &gen) {
  if (!prefs.isKey(key)) return false;             // first boot: nothing saved yet
  size_t n = prefs.getBytesLength(key);
  if (n < 9 || n > 8192) return false;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[n]);
  if (prefs.getBytes(key, buf.get(), n) != n) return false;
  uint32_t g, crc; memcpy(&g, buf.get(), 4); memcpy(&crc, buf.get() + 4, 4);
  if (crc32(buf.get() + 8, n - 8) != crc) return false;
  JsonDocument d;
  if (deserializeJson(d, (const char *)buf.get() + 8, n - 8)) return false;
  defaultConfig(c);
  configFromJson(d.as<JsonVariantConst>(), c);
  gen = g;
  return true;
}

static void saveConfig() {
  JsonDocument d; configToJson(d);
  String js; serializeJson(d, js);
  size_t n = js.length() + 8;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[n]);
  uint32_t g = cfgGen + 1, crc = crc32((const uint8_t *)js.c_str(), js.length());
  memcpy(buf.get(), &g, 4); memcpy(buf.get() + 4, &crc, 4); memcpy(buf.get() + 8, js.c_str(), js.length());
  prefs.putBytes((g & 1) ? "cfgA" : "cfgB", buf.get(), n);   // alternate slots; the other keeps the last good copy
  cfgGen = g;
}

static void loadConfig() {
  Config a, b; uint32_t ga = 0, gb = 0;
  bool okA = loadSlot("cfgA", a, ga), okB = loadSlot("cfgB", b, gb);
  if (okA && (!okB || ga > gb)) { cfg = a; cfgGen = ga; }
  else if (okB) { cfg = b; cfgGen = gb; }
  else { defaultConfig(cfg); cfgGen = 0; }
  bool hadAny = prefs.isKey("cfgA") || prefs.isKey("cfgB");
  if (hadAny && (okA != okB)) logEvent(EV_CONFIG_RECOVERED);   // one copy was damaged; the other was used
}

struct OvrStore { uint32_t crc; Override o; };
static void saveOverride() {
  OvrStore s; s.o = ovr; s.crc = crc32((const uint8_t *)&s.o, sizeof(s.o));
  prefs.putBytes("ovr", &s, sizeof(s));
}
static void loadOverride() {
  OvrStore s;
  if (prefs.isKey("ovr") && prefs.getBytes("ovr", &s, sizeof(s)) == sizeof(s) && s.crc == crc32((const uint8_t *)&s.o, sizeof(s.o))) ovr = s.o;
  else ovr = Override{};
  if (ovr.mode > MODE_PHOTO) ovr = Override{};
}

// ======================================================================= outputs
static void pwmInit() {
  for (uint8_t c = 0; c < CHANNELS; c++) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(pwmPins[c], PWM_FREQ, PWM_BITS);
    ledcWrite(pwmPins[c], 0);
#else
    ledcSetup(c, PWM_FREQ, PWM_BITS); ledcAttachPin(pwmPins[c], c); ledcWrite(c, 0);
#endif
  }
}

static void writeOutputs() {
  if (pwmMode) {                                   // module: 0-100 % duty -> 0-10 V (set its trim pot for 10 V at 100 %)
    for (uint8_t c = 0; c < CHANNELS; c++) {
      uint32_t duty = (uint32_t)curLvl[c] * cfg.ch[c].cal / 100 * ((1u << PWM_BITS) - 1) / LEVEL_MAX;
      if (duty > (1u << PWM_BITS) - 1) duty = (1u << PWM_BITS) - 1;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
      ledcWrite(pwmPins[c], duty);
#else
      ledcWrite(c, duty);
#endif
    }
    return;
  }
  uint16_t a[4] = {codeOut[0], codeOut[1], codeOut[2], codeOut[3]};
  uint16_t b[4] = {codeOut[4], codeOut[5], codeOut[6], codeOut[7]};
  if (dacA.ok) dacA.write(a);
  if (dacB.ok) dacB.write(b);
}

static uint16_t codeToLevel(uint16_t code, uint8_t cal) {
  if (!code) return 0;
  float v = code * OUT_FS_V / DAC_MAX;
  return (uint16_t)clampi((int)(v / 10.0f / (cal / 100.0f) * 1000.0f + 0.5f), 0, LEVEL_MAX);
}

static void controlTick(uint32_t dtMs) {
  bool tv = timeValid();
  uint32_t e = nowEpoch();
  // picture/manual expiry (also works with no clock, using the uptime counter)
  bool expired = ovr.mode != MODE_AUTO && !overrideActive(ovr, tv, e);
  if (!expired && !tv && ovr.mode != MODE_AUTO && millis() - ovrStartMs > (uint32_t)overrideMinutes(ovr.mins) * 60000UL) expired = true;
  if (expired) { ovr = Override{}; saveOverride(); logEvent(EV_MODE, MODE_AUTO); }

  modeNow = groupLevels(cfg, ovr, tv, e, secOfDay(), grpLvl);
  for (uint8_t c = 0; c < CHANNELS; c++) tgtLvl[c] = channelLevel(cfg, grpLvl, c);

  // identify: pulse one jack so the user can see which light is on it
  bool ident = identifyJack >= 0 && (int32_t)(identifyUntilMs - millis()) > 0;
  if (!ident) identifyJack = -1;
  if (ident) {
    bool on = (millis() / 500) % 2;
    for (uint8_t s = 0; s < 2; s++) { uint8_t c = identifyJack * 2 + s; if (channelInUse(cfg, c)) tgtLvl[c] = on ? 700 : 0; }
  }

  uint32_t fadeMs = ident ? 150 : rampActive ? (uint32_t)cfg.rampSec * 1000 : (uint32_t)cfg.fadeSec * 1000;
  bool changed = false, allThere = true;
  for (uint8_t c = 0; c < CHANNELS; c++) {
    curLvl[c] = slew(curLvl[c], tgtLvl[c], dtMs, fadeMs);
    if (curLvl[c] != tgtLvl[c]) allThere = false;
    uint16_t code = levelToCode(curLvl[c], cfg.ch[c].cal);
    if (code != codeOut[c]) { codeOut[c] = code; changed = true; }
  }
  if (rampActive && allThere) rampActive = false;
  static uint32_t lastForce = 0;
  if (changed || millis() - lastForce > 5000) { writeOutputs(); lastForce = millis(); }   // periodic rewrite heals any glitch

  // energy estimate: level x watts x fixtures
  float w = 0;
  for (uint8_t c = 0; c < CHANNELS; c++) if (channelInUse(cfg, c))
    w += curLvl[c] / 1000.0f * cfg.jack[c / 2].watts[c & 1] * cfg.jack[c / 2].count;
  if (tv) {
    time_t t = e; struct tm lt; localtime_r(&t, &lt);
    if (todayYday != lt.tm_yday) { if (todayYday >= 0) yesterdayWh = todayWh; todayWh = 0; todayYday = lt.tm_yday; }
  }
  todayWh += w * dtMs / 3600000.0f;
}

static uint8_t faultMask = 0;
static void monitorTick() {
  if (!adc.ok) return;
  uint8_t mask = 0;
  for (uint8_t c = 0; c < CHANNELS; c++) {
    int mv = adc.readAmpMv(c);
    ampMv[c] = mv;
    if (mv < 0) { outRd[c] = {OUT_UNKNOWN, 0}; continue; }
    outRd[c] = classifyOutput(codeToMilliVolts(codeOut[c]), (uint16_t)mv, channelInUse(cfg, c));
    if (outRd[c].status == OUT_SHORT) mask |= 1 << c;
  }
  for (uint8_t c = 0; c < CHANNELS; c++) {
    bool was = faultMask & (1 << c), is = mask & (1 << c);
    if (is && !was) logEvent(EV_FAULT, c);
    if (!is && was) logEvent(EV_FAULT_CLEAR, c);
  }
  faultMask = mask;
}

// ======================================================================= status LED + button
static void pixel(uint8_t r, uint8_t g, uint8_t b) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_STATUS_LED, r, g, b);
#else
  neopixelWrite(PIN_STATUS_LED, r, g, b);
#endif
}

static uint32_t btnDownAt = 0;
static void ledTick() {
  uint32_t t = millis();
  bool blink = (t / 400) % 2;
  if (btnDownAt) {                                            // feedback while holding the button
    uint32_t held = t - btnDownAt;
    if (held > 20000) pixel(blink ? 60 : 0, 0, 0);
    else if (held > 5000) pixel(0, 0, 60);
    else pixel(20, 20, 20);
    return;
  }
  if (faultMask || !outputsOk()) { pixel(blink ? 50 : 0, 0, 0); return; }      // red blink: output problem
  if (!timeValid() || timeSrc == TS_ESTIMATED) { pixel(blink ? 40 : 0, blink ? 20 : 0, 0); return; }  // amber: set the clock
  if (apOn) { pixel(0, 0, blink ? 40 : 0); return; }                                    // blue: setup hotspot open
  uint8_t k = 4 + (uint8_t)(6 * (1 + sin(t / 1000.0)));                                  // green breathing: all good
  pixel(0, k, k / 3);
}

static void startAp(uint32_t minutes);
static void buttonTick() {
  bool down = digitalRead(PIN_BUTTON) == LOW || digitalRead(0) == LOW;   // GPIO0 = BOOT button on dev boards
  if (down && !btnDownAt) btnDownAt = millis();
  if (!down && btnDownAt) {
    uint32_t held = millis() - btnDownAt; btnDownAt = 0;
    if (held > 20000) {                                       // forget Wi-Fi only; lights and schedule untouched
      prefs.remove("ssid"); prefs.remove("pass"); logEvent(EV_WIFI, 2); delay(100); ESP.restart();
    } else if (held > 5000) {
      startAp(AP_AFTER_BUTTON_MIN);
    }
  }
}

// ======================================================================= Wi-Fi
static String apName() {
  char n[32]; snprintf(n, sizeof(n), "Quanta-%04X", (uint16_t)(ESP.getEfuseMac() >> 32)); return n;
}
static void startAp(uint32_t minutes) {
  if (!apOn) {
    WiFi.mode(WIFI_AP_STA);                        // STA side on too, so the setup page can scan for networks
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(apName().c_str(), AP_PASSWORD);
    dns.start(53, "*", WiFi.softAPIP());                      // captive portal: phone opens the GUI by itself
    apOn = true;
    Serial.printf("Hotspot ON:  Wi-Fi name \"%s\"  %s  page at http://%s\n", apName().c_str(),
                  strlen(AP_PASSWORD) ? (String("password \"") + AP_PASSWORD + "\"").c_str() : "(open, no password)",
                  WiFi.softAPIP().toString().c_str());
  }
  apUntilMs = minutes ? millis() + minutes * 60000UL : 0;
}
static void stopAp() {
  if (!apOn) return;
  dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apOn = false;
  MDNS.end(); MDNS.begin(MDNS_NAME); MDNS.addService("http", "tcp", 80);
  Serial.println("Hotspot OFF (home Wi-Fi is working)");
}

static void wifiTick() {
  static uint32_t lostSince = 0;
  static bool wasConnected = false;
  bool conn = staSsid.length() && WiFi.status() == WL_CONNECTED;
  if (conn && !wasConnected) {
    logEvent(EV_WIFI, 1, WiFi.RSSI() & 0xFF);
    MDNS.end();                                    // re-announce quanta.local on the home network
    if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
    if (provActive) {                              // just set up from the hotspot: keep it up so the address can be written down
      provActive = false;
      if (apOn) apUntilMs = millis() + PROV_HOLD_S * 1000UL;
    }
    Serial.printf("Home Wi-Fi connected: \"%s\"  page at http://%s  or http://%s.local\n",
                  staSsid.c_str(), WiFi.localIP().toString().c_str(), MDNS_NAME);
  }
  if (!conn && wasConnected) Serial.println("Home Wi-Fi lost, retrying...");
  wasConnected = conn;
  if (!staSsid.length()) { if (!apOn) startAp(0); return; }  // never set up: hotspot stays on
  if (conn) {
    lostSince = 0;
    if (apOn && apUntilMs == 0) stopAp();                     // home Wi-Fi works: close the automatic hotspot
  } else {
    if (!lostSince) lostSince = millis();
    if (!apOn && millis() - lostSince > 120000) startAp(0);   // can't reach home Wi-Fi for 2 min: open hotspot
  }
  if (apOn && apUntilMs && (int32_t)(millis() - apUntilMs) > 0) { apUntilMs = 0; if (conn) stopAp(); }
}

static void onNtp(struct timeval *tv) {
  uint32_t e = tv->tv_sec;
  if (e < 1700000000) return;
  bool first = timeSrc != TS_NTP;
  uint32_t r = rtc.read();
  if (!r || (r > e ? r - e : e - r) > 2) rtc.write(e);        // keep the battery clock right
  timeSrc = TS_NTP; lastTimeSync = e;
  if (first) logEvent(EV_TIME, TS_NTP);
}

// ======================================================================= Quanta Cloud (Firebase over HTTPS)
// The controller signs in to Firebase as its own "device" login and
//   1. writes its status to  devices/<id>/state  every minute (the website's "online" check reads this),
//   2. TEST BUILDS ONLY: watches  devices/<id>/commands/testReset  and, when the cloud asks, wipes its
//      setup (Wi-Fi kept), confirms back, and restarts. Same handshake as the AIDoser.
// It runs in its own task on the other core, so slow internet never delays the lights or the web page.
// The lights never depend on any of this.
#ifndef QUANTA_FB_DB_URL
#define QUANTA_FB_DB_URL "https://quanta-f9c4c-default-rtdb.firebaseio.com"
#endif
#ifndef QUANTA_FB_API_KEY                       // Firebase "Web API key" (Project settings > General). Not a secret.
#define QUANTA_FB_API_KEY ""
#endif
#define CLOUD_STATE_EVERY_MS 60000UL
static String cloudId, cloudEmail, cloudPass, cloudKey;
static String cloudToken, cloudErr;
static uint32_t cloudTokenRenewAt = 0;
static volatile bool cloudOk = false, cloudReload = true;
static volatile uint32_t cloudLastOkMs = 0;
static volatile uint8_t cloudResetState = 0;   // 0 idle | 1 cloud asked (task->loop) | 2 wiped (loop->task) | 3 confirmed (task->loop)
static WiFiClientSecure cloudTls;

static bool cloudConfigured() { return cloudId.length() && cloudEmail.length() && cloudPass.length() && cloudKey.length(); }
static void cloudLoadConfig() {
  cloudId = prefs.getString("cId", "");  cloudEmail = prefs.getString("cEmail", "");
  cloudPass = prefs.getString("cPass", ""); cloudKey = prefs.getString("cKey", QUANTA_FB_API_KEY);
  cloudToken = ""; cloudOk = false;
}

static bool cloudReq(const char *method, const String &url, const String &payload, String *out, int *codeOut) {
  static HTTPClient http;
  http.setReuse(true); http.setTimeout(8000);
  if (!http.begin(cloudTls, url)) { *codeOut = -1; return false; }
  http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, (uint8_t *)payload.c_str(), payload.length());
  if (out) *out = code > 0 ? http.getString() : String();
  http.end();
  *codeOut = code;
  return code > 0;
}

static bool cloudSignIn() {
  JsonDocument b; b["email"] = cloudEmail; b["password"] = cloudPass; b["returnSecureToken"] = true;
  String body, resp; serializeJson(b, body); int code = 0;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + cloudKey;
  if (!cloudReq("POST", url, body, &resp, &code) || code != 200) {
    cloudErr = code > 0 ? String("sign-in refused (HTTP ") + code + ")" : "can't reach Google";
    Serial.printf("Cloud: %s\n", cloudErr.c_str());
    return false;
  }
  JsonDocument r;
  if (deserializeJson(r, resp) || !r["idToken"].is<const char *>()) { cloudErr = "sign-in answer not understood"; return false; }
  cloudToken = r["idToken"].as<String>();
  uint32_t exp = (uint32_t)atol(r["expiresIn"] | "3600");
  cloudTokenRenewAt = millis() + (exp > 600 ? exp - 300 : exp / 2) * 1000UL;
  Serial.printf("Cloud: signed in as %s\n", cloudEmail.c_str());
  return true;
}

static String cloudUrl(const char *path) {
  return String(QUANTA_FB_DB_URL) + "/devices/" + cloudId + "/" + path + ".json?auth=" + cloudToken;
}

// What the website shows: online/offline (lastSeen) and the blue/white levels.
static bool cloudPutState() {
  JsonDocument d;
  d["lastSeen"][".sv"] = "timestamp";               // the server stamps the time, so the controller's clock doesn't matter
  uint16_t blue = 0, white = 0;
  for (uint8_t g = 0; g < cfg.nGroups; g++) {
    uint8_t col = cfg.group[g].color;
    if (col == 0 || col == 10) blue = max<uint16_t>(blue, grpLvl[g]);
    else if (col == 1 || col == 11) white = max<uint16_t>(white, grpLvl[g]);
  }
  d["blue"] = (blue + 5) / 10; d["white"] = (white + 5) / 10;
  d["mode"] = modeNow == MODE_PHOTO ? "photo" : modeNow == MODE_MANUAL ? "manual" : "auto";
  d["fw"] = FW_VERSION; d["setupDone"] = cfg.setupDone; d["outputsOk"] = outputsOk();
  d["faults"] = __builtin_popcount(faultMask); d["rssi"] = WiFi.RSSI();
  d["ip"] = WiFi.localIP().toString(); d["uptime"] = (millis() - bootMs) / 1000;
  String body, resp; serializeJson(d, body); int code = 0;
  bool ok = cloudReq("PATCH", cloudUrl("state"), body, &resp, &code) && code == 200;
  if (ok) { cloudOk = true; cloudLastOkMs = millis(); cloudErr = ""; }
  else {
    cloudOk = false; cloudErr = code > 0 ? String("write refused (HTTP ") + code + ")" : "can't reach Firebase";
    if (code == 401 || code == 403) cloudToken = "";   // sign in again next time
    Serial.printf("Cloud: status write failed: %s\n", cloudErr.c_str());
  }
  return ok;
}

#if defined(QUANTA_TEST_BUILD) && QUANTA_TEST_BUILD
// TEST ONLY. The cloud writes {trigger:true, nonce} to commands/testReset; we wipe the setup,
// write back ack=<nonce>, and restart. (The nonce is text, like on the AIDoser.)
static String cloudLastNonce;
static void cloudTestPoll() {
  String resp; int code = 0;
  if (!cloudReq("GET", cloudUrl("commands/testReset"), "", &resp, &code) || code != 200 || resp == "null") return;
  JsonDocument d;
  if (deserializeJson(d, resp) || !(d["trigger"] | false)) return;
  String nonce = d["nonce"] | "";
  if (!nonce.length() || nonce == cloudLastNonce) return;
  cloudLastNonce = nonce;
  Serial.println("Cloud: test reset requested");
  cloudResetState = 1;                                           // loop() does the wipe
  for (int i = 0; i < 100 && cloudResetState != 2; i++) vTaskDelay(100 / portTICK_PERIOD_MS);
  if (cloudResetState != 2) { cloudResetState = 0; return; }
  JsonDocument a; a["ack"] = nonce; a["trigger"] = false;
  String body; serializeJson(a, body);
  cloudReq("PATCH", cloudUrl("commands/testReset"), body, &resp, &code);
  Serial.printf("Cloud: reset confirmed (HTTP %d), restarting\n", code);
  cloudResetState = 3;                                           // loop() restarts
}
#endif

static void cloudTask(void *) {
  cloudTls.setInsecure();                                        // encrypted, but the server's certificate isn't checked
  uint32_t lastState = 0; bool first = true; uint8_t tick = 0; (void)tick;
  for (;;) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    if (cloudReload) { cloudReload = false; cloudLoadConfig(); first = true; }
    if (!cloudConfigured()) { cloudErr = "not set up"; cloudOk = false; continue; }
    if (WiFi.status() != WL_CONNECTED) { cloudOk = false; continue; }
    if (cloudToken.isEmpty() || (int32_t)(millis() - cloudTokenRenewAt) >= 0) {
      if (!cloudSignIn()) { cloudOk = false; vTaskDelay(15000 / portTICK_PERIOD_MS); continue; }
    }
#if defined(QUANTA_TEST_BUILD) && QUANTA_TEST_BUILD
    if (++tick >= 3) { tick = 0; cloudTestPoll(); if (cloudToken.isEmpty()) continue; }
#endif
    uint32_t now = millis();
    if (first || now - lastState >= CLOUD_STATE_EVERY_MS) {
      lastState = now;
      if (cloudPutState()) first = false; else vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
  }
}

// Called from loop(): the parts that must run on the main task.
static void cloudLoopTick() {
#if defined(QUANTA_TEST_BUILD) && QUANTA_TEST_BUILD
  if (cloudResetState == 1) {                                    // wipe like a brand-new unit: settings AND home Wi-Fi (cloud login is kept), so it restarts in hotspot mode
    prefs.remove("cfgA"); prefs.remove("cfgB"); prefs.remove("ovr"); prefs.remove("uiExtra");
    prefs.remove("ssid"); prefs.remove("pass");
    Serial.println("Test reset: settings and Wi-Fi erased, restarting into hotspot mode");
    cloudResetState = 2;
  } else if (cloudResetState == 3) { delay(300); ESP.restart(); }
#endif
}

// ======================================================================= web API
static const char *statusId(uint8_t s, bool inUse) {
  if (!adc.ok) return inUse ? "unmonitored" : "idle";
  switch (s) { case OUT_OK: return "ok"; case OUT_NO_LOAD: return "no_load"; case OUT_SHORT: return "short";
               case OUT_UNKNOWN: return "unknown"; default: return "idle"; }
}
static const char *resetId(esp_reset_reason_t r) {
  switch (r) { case ESP_RST_POWERON: return "power_on"; case ESP_RST_SW: return "restart"; case ESP_RST_PANIC: return "crash";
               case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
               case ESP_RST_BROWNOUT: return "brownout"; default: return "other"; }
}
static const char *timeSrcId() {
  static const char *s[] = {"none", "rtc", "internet", "phone", "estimated"}; return s[timeSrc];
}

static void sendJson(JsonDocument &d, int code = 200) {
  String out; serializeJson(d, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}
static void sendError(int code, const char *msg) {
  JsonDocument d; d["error"] = msg; sendJson(d, code);
}

static void apiState() {
  JsonDocument d;
  d["fw"] = FW_VERSION;
#if defined(QUANTA_TEST_BUILD) && QUANTA_TEST_BUILD
  d["testBuild"] = true;                           // TEST ONLY: lets the dashboard obey the cloud "Reset & Start Over"
#endif
  JsonObject t = d["time"].to<JsonObject>();
  t["epoch"] = nowEpoch(); t["valid"] = timeValid(); t["source"] = timeSrcId(); t["sec"] = secOfDay();
  t["tz"] = cfg.tz; t["lastSync"] = lastTimeSync; t["rtcOk"] = rtc.ok;
  d["mode"] = modeNow == MODE_PHOTO ? "photo" : modeNow == MODE_MANUAL ? "manual" : "auto";
  d["until"] = ovr.until; d["photo"] = ovr.photo; d["mins"] = ovr.mins;
  d["ramping"] = rampActive; d["identify"] = identifyJack;
  d["accl"] = acclFactor(cfg, nowEpoch());
  JsonArray g = d["groups"].to<JsonArray>(); for (uint8_t i = 0; i < cfg.nGroups; i++) g.add(grpLvl[i]);
  JsonArray ca = d["ch"].to<JsonArray>();
  for (uint8_t c = 0; c < CHANNELS; c++) {
    JsonObject o = ca.add<JsonObject>();
    o["lvl"] = curLvl[c]; o["mv"] = codeToMilliVolts(codeOut[c]); o["amp"] = ampMv[c];
    o["status"] = statusId(outRd[c].status, channelInUse(cfg, c)); o["ua"] = outRd[c].microAmps;
  }
  JsonObject s = d["sys"].to<JsonObject>();
  s["uptime"] = (millis() - bootMs) / 1000; s["heap"] = ESP.getFreeHeap(); s["boots"] = boots;
  s["reset"] = resetId(resetReason);
  s["outageStart"] = lastOutageStart; s["outageEnd"] = lastOutageEnd;
  int16_t tq = rtc.tempQuarterC(); if (tq != INT16_MIN) s["tempC"] = tq / 4.0f;
  s["dacOk"] = outputsOk(); s["adcOk"] = adc.ok; s["outMode"] = pwmMode ? "pwm" : "dac";
  s["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : staSsid.length() ? "searching" : "not_set";
  s["ssid"] = staSsid; s["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  s["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  s["ap"] = apOn; s["apName"] = apName();
  s["todayWh"] = todayWh; s["yesterdayWh"] = yesterdayWh;
  JsonObject cl = d["cloud"].to<JsonObject>();
  cl["id"] = cloudId; cl["configured"] = cloudConfigured(); cl["ok"] = cloudOk; cl["err"] = cloudErr;
  cl["age"] = cloudOk ? (int32_t)((millis() - cloudLastOkMs) / 1000) : -1;
  JsonObject o = d["ota"].to<JsonObject>();
  const OtaManager::Status &os = ota.status();
  static const char *res[] = {"none", "up_to_date", "available", "installed", "failed", "blocked"};
  o["url"] = otaUrl; o["auto"] = otaAuto; o["running"] = FW_VERSION; o["result"] = res[os.result];
  o["message"] = os.message; o["latest"] = os.latestVersion; o["notes"] = os.notes; o["checkedAt"] = os.checkedAt;
  o["busy"] = otaRequest != 0 || os.busy;
  o["phase"] = otaRequest ? "checking" : os.phase; o["percent"] = os.percent;
  sendJson(d);
}

// The dashboard's setup assistant remembers which lights were chosen ("setup") and the inside-light
// intensity ("intenIn"). They aren't part of the lighting Config, so they're kept as one small string.
static void addUiExtra(JsonDocument &d) {
  String s = prefs.getString("uiExtra", "");
  if (!s.length()) return;
  JsonDocument x;
  if (deserializeJson(x, s)) return;
  if (x["setup"].is<JsonObject>()) d["setup"] = x["setup"];
  if (x["intenIn"].is<int>()) d["intenIn"] = x["intenIn"];
}
static void saveUiExtra(JsonVariantConst d) {
  if (!d["setup"].is<JsonObjectConst>() && !d["intenIn"].is<int>()) return;
  JsonDocument x;
  if (d["setup"].is<JsonObjectConst>()) x["setup"] = d["setup"];
  if (d["intenIn"].is<int>()) x["intenIn"] = clampi(d["intenIn"], 0, 100);
  String s; serializeJson(x, s);
  if (s.length() < 900) prefs.putString("uiExtra", s);
}

static void apiConfigGet() { JsonDocument d; configToJson(d); addUiExtra(d); sendJson(d); }

static void apiConfigPost() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Settings weren't valid JSON"); return; }
  Config next = cfg;
  configFromJson(d.as<JsonVariantConst>(), next);
  bool tzChanged = strcmp(next.tz, cfg.tz) != 0;
  cfg = next;
  if (tzChanged) applyTz();
  saveConfig();
  saveUiExtra(d.as<JsonVariantConst>());
  logEvent(EV_CONFIG);
  apiConfigGet();
}

static void apiControl() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Request wasn't valid JSON"); return; }
  if (d["time"].is<uint32_t>()) {                            // phone's clock: used when there's no internet
    uint32_t e = d["time"];
    if (e > 1700000000 && (timeSrc != TS_NTP || nowEpoch() - lastTimeSync > 86400)) {
      bool wasBad = timeSrc == TS_NONE || timeSrc == TS_ESTIMATED;
      uint32_t diff = e > nowEpoch() ? e - nowEpoch() : nowEpoch() - e;
      if (wasBad || diff > 5) {
        setSystemTime(e); rtc.write(e); timeSrc = TS_PHONE; lastTimeSync = e; logEvent(EV_TIME, TS_PHONE);
      }
    }
  }
  if (d["identify"].is<int>()) {
    int j = d["identify"];
    if (j >= 0 && j < JACKS) { identifyJack = j; identifyUntilMs = millis() + 6000; }
  }
  if (d["mode"].is<const char *>()) {
    const char *m = d["mode"];
    uint32_t e = nowEpoch();
    if (!strcmp(m, "auto")) { ovr = Override{}; }
    else if (!strcmp(m, "manual")) {
      Override o; o.mode = MODE_MANUAL;
      for (uint8_t g = 0; g < MAX_GROUPS; g++) o.lvl[g] = grpLvl[g];     // start from what's showing
      uint8_t g = 0;
      for (JsonVariantConst v : d["lvl"].as<JsonArrayConst>()) { if (g < MAX_GROUPS) o.lvl[g++] = (uint16_t)clampi(v | 0, 0, LEVEL_MAX); }
      o.mins = overrideMinutes(d["minutes"] | 60);                     // 30 or 60 only, never "until I resume"
      o.until = timeValid() ? e + o.mins * 60 : 1;
      ovr = o;
    } else if (!strcmp(m, "photo")) {
      int i = d["index"] | -1;
      if (i < 0 || i >= cfg.nPhoto) { sendError(400, "That picture setting doesn't exist"); return; }
      Override o; o.mode = MODE_PHOTO; o.photo = (int8_t)i;
      memcpy(o.lvl, cfg.photo[i].lvl, sizeof(o.lvl));
      o.mins = overrideMinutes(cfg.photo[i].minutes);
      o.until = timeValid() ? e + o.mins * 60 : 1;
      ovr = o;
    }
    ovrStartMs = millis();
    saveOverride();
    logEvent(EV_MODE, ovr.mode);
  }
  apiState();
}

static void apiEvents() {
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (uint8_t i = 0; i < EV_N; i++) {
    const Event &e = evlog[(evHead + EV_N - 1 - i) % EV_N];      // newest first
    if (!e.code) continue;
    JsonObject o = a.add<JsonObject>(); o["t"] = e.t; o["code"] = e.code; o["a"] = e.a; o["b"] = e.b;
  }
  sendJson(d);
}

static void apiWifi() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain")) || !d["ssid"].is<const char *>()) { sendError(400, "Enter a network name"); return; }
  prefs.putString("ssid", d["ssid"].as<const char *>());
  prefs.putString("pass", d["pass"] | "");
  JsonDocument r; r["ok"] = true; r["restarting"] = true; sendJson(r);
  delay(400);
  ESP.restart();                                               // outputs hold their level through the restart
}

static void apiOta() {
  if (ota.status().busy || otaRequest) { apiState(); return; }   // an update is already running
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { sendError(400, "Request wasn't valid JSON"); return; }
  if (d["url"].is<const char *>()) { otaUrl = d["url"].as<const char *>(); otaUrl.trim(); prefs.putString("otaUrl", otaUrl); }
  if (d["auto"].is<bool>()) { otaAuto = d["auto"]; prefs.putBool("otaAuto", otaAuto); }
  const char *action = d["action"] | "";
  if (!strcmp(action, "check")) otaRequest = 1;
  if (!strcmp(action, "install")) otaRequest = 2;
  apiState();
}

// ---- Wi-Fi setup (same flow as the reef doser's Provisioner) ----
static void apiScan() {
  int n = WiFi.scanNetworks();                     // blocks a few seconds; outputs keep running in hardware
  struct Net { String ssid; int32_t rssi; bool open; };
  std::vector<Net> nets;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;                     // hidden networks can't be picked from a list
    bool found = false;
    for (auto &e : nets) if (e.ssid == s) {        // mesh routers repeat names: keep the strongest
      found = true;
      if (WiFi.RSSI(i) > e.rssi) { e.rssi = WiFi.RSSI(i); e.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN; }
      break;
    }
    if (!found) nets.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
  JsonDocument d; JsonArray a = d.to<JsonArray>();
  for (auto &e : nets) { JsonObject o = a.add<JsonObject>(); o["ssid"] = e.ssid; o["rssi"] = e.rssi; o["open"] = e.open; }
  sendJson(d);
}

static void apiSaveWifi() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain")) || !d["ssid"].is<const char *>()) { sendError(400, "Enter a network name"); return; }
  staSsid = d["ssid"].as<const char *>(); staSsid.trim();
  staPass = d["pass"] | "";
  prefs.putString("ssid", staSsid); prefs.putString("pass", staPass);
  Serial.printf("Wi-Fi setup: saved \"%s\", connecting (hotspot stays on)...\n", staSsid.c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  provActive = true; provStartMs = millis();
  apUntilMs = 0;                                   // hold the hotspot until connected, then PROV_HOLD_S more
  JsonDocument r; r["ok"] = true; sendJson(r);
}

static void apiConnectStatus() {
  bool conn = WiFi.status() == WL_CONNECTED;
  uint32_t el = provStartMs ? (millis() - provStartMs) / 1000 : 0;
  JsonDocument d;
  d["connected"] = conn;
  d["failed"] = !conn && provStartMs && el >= PROV_FAIL_S;
  d["elapsed"] = el;
  d["ip"] = conn ? WiFi.localIP().toString() : "";
  d["mdns"] = MDNS_NAME;
  d["ssid"] = staSsid;
  sendJson(d);
}

static void apiFactoryReset() {
  prefs.remove("cfgA"); prefs.remove("cfgB"); prefs.remove("ovr"); prefs.remove("uiExtra");
  JsonDocument r; r["ok"] = true; sendJson(r);
  delay(400);
  ESP.restart();
}

// Provisioning: http://quanta.local/api/cloud-setup?id=quantacontroller1&email=...&pass=...&key=...
// (any subset). Saved on the controller; the password is never sent back. Lock this down before shipping.
static void apiCloudSetup() {
  if (server.hasArg("id")) prefs.putString("cId", server.arg("id"));
  if (server.hasArg("email")) prefs.putString("cEmail", server.arg("email"));
  if (server.hasArg("pass")) prefs.putString("cPass", server.arg("pass"));
  if (server.hasArg("key")) prefs.putString("cKey", server.arg("key"));
  cloudReload = true;
  JsonDocument r; r["ok"] = true; r["id"] = prefs.getString("cId", ""); r["email"] = prefs.getString("cEmail", "");
  r["hasPassword"] = prefs.getString("cPass", "").length() > 0; r["hasKey"] = prefs.getString("cKey", QUANTA_FB_API_KEY).length() > 0;
  sendJson(r);
}

static void webInit() {
  server.on("/", HTTP_GET, [] {
    Serial.printf("Page opened by %s\n", server.client().remoteIP().toString().c_str());
    if (!staSsid.length() && !server.hasArg("app") && !provActive) {   // never set up: Wi-Fi setup comes first
      server.sendHeader("Cache-Control", "no-store");
      server.send_P(200, "text/html", WIFI_SETUP_HTML);
      return;
    }
    server.sendHeader("Cache-Control", "no-store");   // phones always get the page from this firmware
    server.sendHeader("Content-Encoding", "gzip");    // web_ui.h holds the dashboard gzip-compressed
    server.send_P(200, "text/html", (PGM_P)INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });
  server.on("/wifi", HTTP_GET, [] { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html", WIFI_SETUP_HTML); });
  server.on("/scan", HTTP_GET, apiScan);
  server.on("/save-wifi", HTTP_POST, apiSaveWifi);
  server.on("/connect-status", HTTP_GET, apiConnectStatus);
  server.on("/api/state", HTTP_GET, apiState);
  server.on("/api/config", HTTP_GET, apiConfigGet);
  server.on("/api/config", HTTP_POST, apiConfigPost);
  server.on("/api/control", HTTP_POST, apiControl);
  server.on("/api/events", HTTP_GET, apiEvents);
  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/factory-reset", HTTP_POST, apiFactoryReset);
  server.on("/api/cloud-setup", HTTP_GET, apiCloudSetup);
  server.on("/api/ota", HTTP_POST, apiOta);
  ota.onTick([] { server.handleClient(); esp_task_wdt_reset(); });   // keep the page answering during an update
  server.on("/api/backup", HTTP_GET, [] {
    JsonDocument d; configToJson(d); addUiExtra(d); String out; serializeJsonPretty(d, out);
    server.sendHeader("Content-Disposition", "attachment; filename=quanta-settings.json");
    server.send(200, "application/json", out);
  });
  server.on("/update", HTTP_POST,
    [] {
      bool ok = !Update.hasError();
      server.send(ok ? 200 : 500, "text/plain", ok ? "Update installed. Restarting." : Update.errorString());
      if (ok) { logEvent(EV_UPDATE); delay(400); ESP.restart(); }
    },
    [] {
      HTTPUpload &u = server.upload();
      if (ota.status().busy) return;                   // fleet update in progress: ignore uploads
      if (u.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
      else if (u.status == UPLOAD_FILE_WRITE) { Update.write(u.buf, u.currentSize); esp_task_wdt_reset(); }
      else if (u.status == UPLOAD_FILE_END) Update.end(true);
    });
  server.onNotFound([] {                                       // captive portal and unknown paths -> the GUI
    server.sendHeader("Location", String("http://") + (apOn ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

// New firmware stays "pending" until it has run cleanly for a minute; otherwise the bootloader
// goes back to the previous version on the next reset.
// Fleet updates. Runs from loop(); downloading blocks for up to a minute or so, during which the
// outputs simply hold their level (DACs and PWM hardware keep running on their own).
static void otaTick() {
  static uint32_t connectedAt = 0, lastAuto = 0;
  bool conn = WiFi.status() == WL_CONNECTED;
  if (!conn) { connectedAt = 0; }
  else if (!connectedAt) connectedAt = millis();
  if (otaRequest) {
    uint8_t r = otaRequest; otaRequest = 0;
    server.handleClient();                         // let the page get its answer first
    ota.check(otaUrl, r == 2, timeValid() ? nowEpoch() : 0);
    return;
  }
  if (!otaAuto || !conn || otaUrl.length() == 0 || millis() - bootMs < OTA_CONFIRM_MS) return;
  bool firstDue = lastAuto == 0 && millis() - connectedAt > OTA_FIRST_CHECK_S * 1000UL;
  bool periodicDue = lastAuto != 0 && millis() - lastAuto > OTA_CHECK_HOURS * 3600000UL;
  if (firstDue || periodicDue) {
    lastAuto = millis();
    ota.check(otaUrl, true, timeValid() ? nowEpoch() : 0);
  }
}

extern "C" bool verifyRollbackLater() { return true; }
static void otaConfirmTick() {
  static bool done = false;
  if (done || millis() - bootMs < OTA_CONFIRM_MS) return;
  done = true;
  const esp_partition_t *p = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY && outputsOk())
    esp_ota_mark_app_valid_cancel_rollback();
}

static void wdtInit() {
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_task_wdt_config_t c = {.timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true};
  if (esp_task_wdt_reconfigure(&c) != ESP_OK) esp_task_wdt_init(&c);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ======================================================================= emulator self-test
// Build with -DQEMU_TEST to run in Espressif's QEMU (no Wi-Fi radio there). Boot 1 sets the clock,
// switches to manual 30 %, saves and restarts; boot 2 checks everything came back.
#ifdef QEMU_TEST
static void qemuReport(const char *tag) {
  Serial.printf("[%s] reset=%s time=%s valid=%d mode=%u outputs=%s cfgGen=%u boots=%u\n", tag, resetId(resetReason), timeSrcId(),
                timeValid(), modeNow, pwmMode ? "pwm" : "dac", cfgGen, boots);
  Serial.printf("[%s] levels:", tag);
  for (uint8_t c = 0; c < CHANNELS; c++) Serial.printf(" %u", curLvl[c]);
  Serial.printf("  (targets:");
  for (uint8_t c = 0; c < CHANNELS; c++) Serial.printf(" %u", tgtLvl[c]);
  Serial.printf(")\n");
}
static void qemuTestSetup() {
  uint32_t stage = prefs.getUInt("qstage", 0);
  Serial.printf("QEMU self-test, stage %u\n", stage);
  if (stage == 0) {
    setSystemTime(1790794800);  timeSrc = TS_PHONE;           // 2026-09-30 14:00 Central
    prefs.putUInt("hb", nowEpoch());
    for (uint8_t j = 0; j < 3; j++) setFixture(cfg, j, j == 2 ? FX_ALTAIR_100 : FX_ALTAIR_210);
    cfg.setupDone = true; saveConfig();
    Override o; o.mode = MODE_MANUAL; o.lvl[0] = 300; o.lvl[1] = 300; o.mins = 60; o.until = nowEpoch() + 3600; ovr = o; saveOverride();
    for (int i = 0; i < 40; i++) { controlTick(CONTROL_MS); delay(CONTROL_MS); }
    qemuReport("boot1");
    prefs.putUInt("qstage", 1);
    Serial.println("restarting...");
    delay(200); ESP.restart();
  } else {
    for (int i = 0; i < 80; i++) { controlTick(CONTROL_MS); delay(CONTROL_MS); }
    qemuReport("boot2");
    bool ok = cfg.setupDone && cfg.jack[0].fixture == FX_ALTAIR_210 && cfg.jack[2].fixture == FX_ALTAIR_100 &&
              ovr.mode == MODE_MANUAL && modeNow == MODE_MANUAL && timeValid() &&
              curLvl[0] == 300 && curLvl[1] == 300 && curLvl[6] == 0 && pwmMode;
    uint8_t n = 0; for (uint8_t i = 0; i < EV_N; i++) if (evlog[i].code) n++;
    Serial.printf("events logged: %u\n", n);
    Serial.println(ok ? "QEMU SELF-TEST PASS" : "QEMU SELF-TEST FAIL");
    prefs.putUInt("qstage", 0);
  }
}
#endif

// ======================================================================= setup / loop
void setup() {
  bootMs = millis();
  resetReason = esp_reset_reason();
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(0, INPUT_PULLUP);                        // BOOT button on dev boards works as the setup button too

  // 1. Talk to the output chips first and learn what the lights are showing right now.
  // MCP4728s come in eight address versions (A0..A7 = 0x60..0x67) and whichever is in stock gets
  // fitted, so search for them instead of assuming. PCB: DAC A alone on bus 1 (GPIO21/22), DAC B
  // alone on bus 2 (GPIO16/17). Breadboard: one DAC on bus 1. Older builds: two DACs on bus 1.
  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire1.begin(PIN_SDA2, PIN_SCL2, I2C_HZ);
  adc.begin(); rtc.begin();
  uint8_t bus1[2] = {0, 0}, n1 = 0, onBus2 = 0;
  for (uint8_t a = 0x60; a <= 0x67; a++) {
    Wire.beginTransmission(a);  if (Wire.endTransmission() == 0 && n1 < 2) bus1[n1++] = a;
    Wire1.beginTransmission(a); if (Wire1.endTransmission() == 0 && !onBus2) onBus2 = a;
  }
  if (n1) { dacA = hal::Mcp4728(bus1[0]); dacA.begin(); }
  if (onBus2) { dacB = hal::Mcp4728(onBus2, &Wire1); dacB.begin(); }
  else if (n1 > 1) { dacB = hal::Mcp4728(bus1[1]); dacB.begin(); }
  Serial.printf("DAC A: %s 0x%02X   DAC B: %s 0x%02X%s\n", dacA.ok ? "found at" : "missing", dacA.addr,
                dacB.ok ? "found at" : "missing", dacB.addr, onBus2 ? " (bus 2, GPIO16/17)" : "");
  uint16_t cur[4], eep[4]; bool ir[4];
  if (dacA.ok && dacA.read(cur, eep, ir)) for (int i = 0; i < 4; i++) codeOut[i] = cur[i];
  if (dacB.ok && dacB.read(cur, eep, ir)) for (int i = 0; i < 4; i++) codeOut[4 + i] = cur[i];
  pwmMode = !dacA.ok && !dacB.ok;                  // no controller board: bench prototype with PWM modules
  if (pwmMode) pwmInit();

  // 2. Settings.
  prefs.begin("quanta", false);
  boots = prefs.getUInt("boots", 0) + 1; prefs.putUInt("boots", boots);
  if (!prefs.isKey("ev") || prefs.getBytes("ev", evlog, sizeof(evlog)) != sizeof(evlog)) memset(evlog, 0, sizeof(evlog));
  evHead = prefs.getUChar("evh", 0) % EV_N;
  loadConfig();
  loadOverride();
  applyTz();
  for (uint8_t c = 0; c < CHANNELS; c++) curLvl[c] = codeToLevel(codeOut[c], cfg.ch[c].cal);

  // 3. Time: battery clock first; if it lost time, assume the outage was short and use the last
  //    "still powered" note (flagged as estimated until the internet or a phone corrects it).
  uint32_t hb = prefs.getUInt("hb", 0);
  uint32_t r = rtc.read();
  if (r) { setSystemTime(r); timeSrc = TS_RTC; }
  else if (hb) { setSystemTime(hb); timeSrc = TS_ESTIMATED; logEvent(EV_CLOCK_LOST); }
  bool cold = resetReason == ESP_RST_POWERON || resetReason == ESP_RST_BROWNOUT;
  if (cold && r && hb && r > hb) {
    lastOutageStart = hb; lastOutageEnd = r;                   // accurate to the heartbeat interval
    prefs.putUInt("outS", hb); prefs.putUInt("outE", r);
    logEvent(EV_OUTAGE, 0, (uint16_t)min<uint32_t>((r - hb) / 60, 65535));
  } else {
    lastOutageStart = prefs.getUInt("outS", 0); lastOutageEnd = prefs.getUInt("outE", 0);
  }
  logEvent(EV_BOOT, (uint8_t)resetReason);
  if (!pwmMode && (!dacA.ok || !dacB.ok || !rtc.ok || !adc.ok)) logEvent(EV_HW, (dacA.ok) | (dacB.ok << 1) | (adc.ok << 2) | (rtc.ok << 3));

  // 4. Lights: soft start after a power cut, carry on seamlessly after a software restart.
  rampActive = cold;
  controlTick(0);
  if (dacA.ok) dacA.ensurePowerUpZero();
  if (dacB.ok) dacB.ensurePowerUpZero();

#ifdef QEMU_TEST
  qemuTestSetup();
  return;
#endif
  // 5. Only now: watchdog, Wi-Fi, web GUI, internet time.
  wdtInit();
  staSsid = prefs.isKey("ssid") ? prefs.getString("ssid", "") : "";
  staPass = prefs.isKey("pass") ? prefs.getString("pass", "") : "";
  otaUrl = prefs.isKey("otaUrl") ? prefs.getString("otaUrl", OTA_DEFAULT_URL) : String(OTA_DEFAULT_URL);
  otaAuto = prefs.isKey("otaAuto") ? prefs.getBool("otaAuto", true) : true;
#if defined(QUANTA_TEST_BUILD) && QUANTA_TEST_BUILD
  otaAuto = false;                                 // TEST BUILDS: never let the hosted release replace this firmware
#endif
  WiFi.persistent(false);
  WiFi.setHostname(MDNS_NAME);
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t) { Serial.println("A phone joined the hotspot"); }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
  WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t) { Serial.println("A phone left the hotspot"); }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
  if (staSsid.length()) {
    Serial.printf("Joining home Wi-Fi \"%s\"...\n", staSsid.c_str());
    WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(staSsid.c_str(), staPass.c_str());
  } else startAp(0);
  webInit();
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
  xTaskCreatePinnedToCore(cloudTask, "cloud", 12288, nullptr, 1, nullptr, 0);   // Quanta Cloud link (see above)
  sntp_set_time_sync_notification_cb(onNtp);
  configTzTime(cfg.tz, "pool.ntp.org", "time.nist.gov");
  Serial.printf("Quanta Controller %s  reset=%s  time=%s  outputs=%s dac=%d/%d adc=%d rtc=%d\n", FW_VERSION,
                resetId(resetReason), timeSrcId(), pwmMode ? "PWM (bench prototype)" : "DAC", dacA.ok, dacB.ok, adc.ok, rtc.ok);
}

void loop() {
#ifdef QEMU_TEST
  delay(1000); return;
#endif
  esp_task_wdt_reset();
  uint32_t now = millis();

  static uint32_t lastCtl = now, lastMon = 0, lastSlow = 0;
  if (now - lastCtl >= CONTROL_MS) { controlTick(now - lastCtl); lastCtl = now; }
  if (now - lastMon >= MONITOR_MS) { monitorTick(); lastMon = now; }

  if (now - lastSlow >= 1000) {
    lastSlow = now;
    static uint32_t lastHb = 0, lastRtc = 0;
    uint32_t e = nowEpoch();
    if (timeValid() && timeSrc != TS_ESTIMATED && e - lastHb >= HEARTBEAT_S) { prefs.putUInt("hb", e); lastHb = e; }
    if (rtc.ok && timeSrc == TS_RTC && e - lastRtc >= RTC_RESYNC_S) {
      uint32_t r = rtc.read(); if (r) setSystemTime(r); lastRtc = e;
    }
    wifiTick();
    otaConfirmTick();
    otaTick();
  }

  if (apOn) dns.processNextRequest();
  cloudLoopTick();
  server.handleClient();
  buttonTick();
  static uint32_t lastLed = 0;
  if (now - lastLed >= 50) { ledTick(); lastLed = now; }
  delay(1);
}