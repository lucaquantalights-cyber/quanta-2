#include "Ota.h"
#include <esp_task_wdt.h>

OtaManager::OtaManager(const char* runningVersion) : _running(runningVersion) {}

OtaManager::Result OtaManager::finish(Result r, const String& msg) {
    _status.result = r;
    _status.message = msg;
    _status.busy = (r == INSTALLED);              // INSTALLED = about to restart
    _status.phase = (r == INSTALLED) ? "restarting" : "";
    Serial.printf("OTA: %s\n", msg.c_str());
    return r;
}

int OtaManager::compareVersions(const String& a, const String& b) {
    int ia = 0, ib = 0;
    for (int part = 0; part < 4; part++) {
        long va = 0, vb = 0;
        while (ia < (int)a.length() && !isdigit(a[ia])) ia++;
        while (ia < (int)a.length() && isdigit(a[ia])) va = va * 10 + (a[ia++] - '0');
        while (ib < (int)b.length() && !isdigit(b[ib])) ib++;
        while (ib < (int)b.length() && isdigit(b[ib])) vb = vb * 10 + (b[ib++] - '0');
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

String OtaManager::manifestUrlFromInputUrl(String url) {
    url.trim();
    if (url.indexOf('?') >= 0) return url;          // full link with parameters (e.g. Firebase Storage): use as given
    if (url.endsWith(".json")) return url;
    if (url.endsWith(".bin")) { url.remove(url.length() - 4); return url + ".json"; }
    if (!url.endsWith("/")) url += "/";
    return url + "firmware.json";                  // a folder URL: use its firmware.json
}

String OtaManager::absoluteUrlFromManifestUrl(const String& manifestUrl, const String& maybeRelativeUrl) {
    String fw = maybeRelativeUrl;
    fw.trim();
    if (fw.startsWith("http://") || fw.startsWith("https://")) return fw;
    int q = manifestUrl.indexOf('?');
    if (q >= 0) {                                   // Storage-style manifest link: swap the file name inside the object path
        String base = manifestUrl.substring(0, q);
        int cut = max(base.lastIndexOf("%2F"), base.lastIndexOf("%2f"));
        if (cut >= 0) return base.substring(0, cut + 3) + fw + manifestUrl.substring(q);
    }
    int schemeEnd = manifestUrl.indexOf("://");
    if (schemeEnd < 0) return fw;
    int pathStart = manifestUrl.indexOf('/', schemeEnd + 3);
    if (pathStart < 0) return manifestUrl + "/" + fw;
    String origin = manifestUrl.substring(0, pathStart);
    if (fw.startsWith("/")) return origin + fw;
    int lastSlash = manifestUrl.lastIndexOf('/');
    if (lastSlash < pathStart) return origin + "/" + fw;
    return manifestUrl.substring(0, lastSlash + 1) + fw;
}

// Plain paths (".../quanta/firmware.bin") and Firebase Storage style ("...o/quanta%2Ffirmware.bin").
bool OtaManager::insideQuantaFolder(const String& url) {
    String u = url;
    u.toLowerCase();
    return u.indexOf("/" OTA_FOLDER "/") >= 0 || u.indexOf("/" OTA_FOLDER "%2f") >= 0;
}

bool OtaManager::downloadManifest(const String& manifestUrl, JsonDocument& doc) {
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    esp_task_wdt_reset();
    if (!http.begin(manifestUrl)) return false;
    int code = http.GET();
    esp_task_wdt_reset();
    if (code != HTTP_CODE_OK) {
        Serial.printf("OTA manifest HTTP %d: %s\n", code, http.errorToString(code).c_str());
        http.end();
        return false;
    }
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) Serial.printf("OTA manifest JSON parse failed: %s\n", err.c_str());
    return !err;
}

OtaManager::Result OtaManager::check(const String& url, bool doInstall, uint32_t nowEpoch) {
    _status.checkedAt = nowEpoch;
    _status.busy = true; _status.phase = "checking"; _status.percent = 0;
    tick();
    if (url.length() == 0) return finish(BLOCKED, "No update server set.");
    if (WiFi.status() != WL_CONNECTED) return finish(FAILED, "Not connected to home Wi-Fi, so updates can't be checked.");

    String manifestUrl = manifestUrlFromInputUrl(url);
    Serial.printf("OTA: checking %s (running %s)\n", manifestUrl.c_str(), _running);

    JsonDocument doc;
    if (!downloadManifest(manifestUrl, doc)) return finish(FAILED, "Couldn't read the update information. Check the server address.");

    String product = doc["product"] | "";
    String version = doc["version"] | "";
    String fw = doc["firmware"] | "";
    if (fw.length() == 0) fw = doc["firmwareUrl"] | "";
    if (fw.length() == 0) fw = doc["url"] | "";
    if (fw.length() == 0) fw = doc["binUrl"] | "";
    String md5 = doc["md5"] | "";
    _status.latestVersion = version;
    _status.notes = doc["notes"] | "";

    if (product != OTA_PRODUCT) return finish(BLOCKED, "Blocked: that update is not for this product (" + product + ").");
    if (version.length() == 0) return finish(BLOCKED, "Blocked: the update has no version number.");
    if (fw.length() == 0) return finish(BLOCKED, "Blocked: the update has no firmware file.");
    String firmwareUrl = absoluteUrlFromManifestUrl(manifestUrl, fw);
    if (!insideQuantaFolder(firmwareUrl)) return finish(BLOCKED, "Blocked: the firmware file is not in the quanta folder.");
    if (md5.length() && md5.length() != 32) return finish(BLOCKED, "Blocked: the MD5 checksum in the update is malformed.");

    if (compareVersions(version, _running) <= 0)
        return finish(UP_TO_DATE, String("Up to date (version ") + _running + ").");
    if (!doInstall)
        return finish(UPDATE_AVAILABLE, "Version " + version + " is available.");

    Serial.printf("OTA: installing %s from %s\n", version.c_str(), firmwareUrl.c_str());
    _status.phase = "downloading"; _status.percent = 0;
    tick();
    if (!install(firmwareUrl, md5)) return _status.result;   // install() set the message
    return INSTALLED;                                         // not reached: install restarts
}

bool OtaManager::install(const String& firmwareUrl, const String& md5) {
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    esp_task_wdt_reset();
    if (!http.begin(firmwareUrl)) { finish(FAILED, "Download failed to start."); return false; }
    int code = http.GET();
    esp_task_wdt_reset();
    if (code != HTTP_CODE_OK) {
        finish(FAILED, "Download failed (HTTP " + String(code) + ").");
        http.end();
        return false;
    }
    int len = http.getSize();
    if (len <= 0) { finish(FAILED, "Download failed: unknown file size."); http.end(); return false; }
    if (!Update.begin((size_t)len, U_FLASH)) {
        finish(FAILED, String("Not enough room for the update: ") + Update.errorString());
        http.end();
        return false;
    }
    if (md5.length() == 32) Update.setMD5(md5.c_str());

    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buffer[4096];                   // static: keeps the loop task's stack small
    size_t total = 0;
    unsigned long lastData = millis(), lastLog = 0;
    while (http.connected() && total < (size_t)len) {
        size_t avail = stream->available();
        if (avail) {
            size_t want = min(min(avail, sizeof(buffer)), (size_t)len - total);
            int got = stream->readBytes(buffer, want);
            if (got <= 0) { Update.abort(); http.end(); finish(FAILED, "Download failed: no data."); return false; }
            if (Update.write(buffer, got) != (size_t)got) {
                Update.abort(); http.end(); finish(FAILED, String("Writing the update failed: ") + Update.errorString()); return false;
            }
            total += got;
            lastData = millis();
            _status.percent = (uint8_t)(100ULL * total / len);
            tick();
            if (millis() - lastLog > 2000 || total == (size_t)len) {
                Serial.printf("OTA: %u/%d bytes (%.0f%%)\n", (unsigned)total, len, 100.0 * total / len);
                lastLog = millis();
            }
        } else {
            if (millis() - lastData > 15000) { Update.abort(); http.end(); finish(FAILED, "Download stalled."); return false; }
            delay(1);
        }
        esp_task_wdt_reset();
        yield();
    }
    http.end();
    if (total != (size_t)len) { Update.abort(); finish(FAILED, "Download incomplete."); return false; }
    _status.phase = "installing"; tick();
    if (!Update.end(true) || !Update.isFinished()) {
        finish(FAILED, String("Update check failed: ") + Update.errorString());   // includes MD5 mismatch
        return false;
    }
    finish(INSTALLED, "Update installed. Restarting.");
    Serial.flush();
    for (int i = 0; i < 30; i++) { tick(); delay(50); }   // let the page see "restarting"

    ESP.restart();
    return true;
}
