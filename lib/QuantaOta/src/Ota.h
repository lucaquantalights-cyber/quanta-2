// Ota.h - fleet OTA for the Quanta Altair Controller.
// Based on the reefDoser OtaManager, changed so every controller shares one firmware:
//   <server>/quanta/firmware.json   manifest
//   <server>/quanta/firmware.bin    firmware (or any name the manifest gives, inside quanta/)
//
// Manifest:
//   { "product": "quanta-altair", "version": "2.0.1", "firmware": "firmware.bin",
//     "md5": "<optional 32-char hex of the .bin>", "notes": "<optional>" }
//
// Install happens only if: product matches, the firmware URL is inside the quanta/ folder,
// the version is newer than the running one, and (when given) the MD5 matches.
#ifndef QUANTA_OTA_H
#define QUANTA_OTA_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

#define OTA_PRODUCT "quanta-altair"
#define OTA_FOLDER  "quanta"

class OtaManager {
public:
    enum Result : uint8_t { NONE = 0, UP_TO_DATE, UPDATE_AVAILABLE, INSTALLED, FAILED, BLOCKED };

    struct Status {
        Result   result = NONE;
        String   message;          // plain-language result, shown on the Settings page
        String   latestVersion;    // version found in the manifest
        String   notes;            // manifest "notes"
        uint32_t checkedAt = 0;    // epoch of last check (0 = never)
        bool     busy = false;     // a check or install is running right now
        String   phase;            // "checking", "downloading", "installing", "restarting"
        uint8_t  percent = 0;      // download progress
    };

    // Called often while busy, so the caller can keep its web page answering.
    typedef void (*TickFn)();
    void onTick(TickFn f) { _tick = f; }

    explicit OtaManager(const char* runningVersion);

    // Reads the manifest. If install is true and a newer version passes the checks,
    // downloads it, verifies it and restarts (does not return on success).
    Result check(const String& url, bool install, uint32_t nowEpoch);

    const Status& status() const { return _status; }

    // "2.0.10" > "2.0.9". Returns <0, 0, >0.
    static int compareVersions(const String& a, const String& b);

private:
    const char* _running;
    Status _status;
    TickFn _tick = nullptr;
    void tick() { if (_tick) _tick(); }

    Result finish(Result r, const String& msg);
    String manifestUrlFromInputUrl(String url);
    String absoluteUrlFromManifestUrl(const String& manifestUrl, const String& maybeRelativeUrl);
    bool   insideQuantaFolder(const String& url);
    bool   downloadManifest(const String& manifestUrl, JsonDocument& doc);
    bool   install(const String& firmwareUrl, const String& md5);
};

#endif
