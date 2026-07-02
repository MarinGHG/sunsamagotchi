// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Marin Benke
#pragma once

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <ArduinoJson.h>
#include "settings.h"

// Talks to esp_ota_ops directly instead of the Arduino `Update` library.
// `Update` pulls in HttpsOTAUpdate.cpp (esp_https_ota), which pushed the
// CoreInk build ~1KB over the fixed 128KB IRAM region once combined with
// the WiFiClientSecure/HTTPClient this firmware already links for MCP —
// esp_ota_ops is the same underlying primitive with none of that overhead.

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev-local"
#endif

// ─── OTA update check + flash ───────────────────────────────────────────────
// Reuses the same manifest.json the web flasher already serves — no separate
// backend. Two fixed, always-current URLs (Cloudflare Pages branch aliases,
// not the CI-generated deployment URLs, which change every run):
//   beta   -> https://preview.sunsamagotchi.pages.dev/firmware/
//   stable -> https://sunsamagotchi.marinbenke.dev/firmware/
namespace Ota {

static const char* CHANNEL_BASE_URL[OTA_CHANNEL_COUNT] = {
    "",  // OTA_OFF — unused
    "https://preview.sunsamagotchi.pages.dev/firmware/",
    "https://sunsamagotchi.marinbenke.dev/firmware/",
};

#if defined(BOARD_COREINK)
static const char* BOARD_KEY = "coreink";
#else
static const char* BOARD_KEY = "stickc";
#endif

struct CheckResult {
    bool available = false;
    char tag[32]   = {0};
    char url[192]  = {0};  // full app.bin URL for the running board
};

// Fetches manifest.json for the given channel and compares its version
// against FIRMWARE_VERSION. Returns false on any network/parse failure —
// callers should treat that exactly like "no update available" (fail safe,
// never fail toward "update available" on a garbled response).
inline bool check(uint8_t channel, CheckResult& out) {
    if (channel == OTA_OFF || channel >= OTA_CHANNEL_COUNT) return false;

    WiFiClientSecure sslClient;
    sslClient.setInsecure();  // matches mcp_client.h — ESP32 Arduino core has no easy cert store here
    HTTPClient http;
    String url = String(CHANNEL_BASE_URL[channel]) + "manifest.json";
    if (!http.begin(sslClient, url)) return false;
    http.setTimeout(10000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] manifest fetch failed: HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.println("[OTA] manifest parse failed");
        return false;
    }

    const char* version = doc["version"] | "";
    if (!version[0] || strcmp(version, FIRMWARE_VERSION) == 0) {
        return false;  // up to date (or manifest missing a version — fail safe)
    }

    JsonVariant board = doc["boards"][BOARD_KEY];
    if (board.isNull()) {
        Serial.println("[OTA] manifest missing board entry");
        return false;
    }
    const char* appFile = nullptr;
    for (JsonVariant f : board["files"].as<JsonArray>()) {
        const char* name = f["file"] | "";
        if (strstr(name, "-app.bin")) { appFile = name; break; }
    }
    if (!appFile) {
        Serial.println("[OTA] manifest missing app.bin entry");
        return false;
    }

    strlcpy(out.tag, version, sizeof(out.tag));
    snprintf(out.url, sizeof(out.url), "%s%s", CHANNEL_BASE_URL[channel], appFile);
    out.available = true;
    Serial.printf("[OTA] update available: %s -> %s\n", FIRMWARE_VERSION, out.tag);
    return true;
}

// Downloads and flashes the given app.bin URL into the inactive OTA
// partition (never touches the currently-running one). Caller is
// responsible for the boot-validate/rollback dance — see
// rtcOtaValidatePending in main.cpp — since a bad image only becomes "safe"
// once we've proven the device still boots on it.
inline bool performUpdate(const char* url, void (*onProgress)(int pct) = nullptr) {
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        Serial.println("[OTA] no free OTA partition");
        return false;
    }

    WiFiClientSecure sslClient;
    sslClient.setInsecure();
    HTTPClient http;
    if (!http.begin(sslClient, url)) return false;
    http.setTimeout(20000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[OTA] app.bin fetch failed: HTTP %d\n", code);
        http.end();
        return false;
    }
    int len = http.getSize();
    if (len <= 0 || (size_t)len > target->size) {
        Serial.printf("[OTA] bad app.bin size: %d (partition holds %u)\n", len, (unsigned)target->size);
        http.end();
        return false;
    }

    esp_ota_handle_t handle;
    if (esp_ota_begin(target, len, &handle) != ESP_OK) {
        Serial.println("[OTA] esp_ota_begin failed");
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    unsigned long lastData = millis();
    bool writeFailed = false;
    while (written < len && millis() - lastData < 15000) {
        size_t avail = stream->available();
        if (avail == 0) { delay(5); continue; }
        int n = stream->readBytes(buf, min(avail, sizeof(buf)));
        if (n <= 0) continue;
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            Serial.println("[OTA] flash write failed");
            writeFailed = true;
            break;
        }
        written += n;
        lastData = millis();
        if (onProgress) onProgress((written * 100) / len);
    }
    http.end();

    if (writeFailed || written != len) {
        if (!writeFailed) Serial.printf("[OTA] incomplete download: %d/%d bytes\n", written, len);
        esp_ota_end(handle);  // discard — target partition left as-is
        return false;
    }
    if (esp_ota_end(handle) != ESP_OK) {
        Serial.println("[OTA] esp_ota_end failed (image validation)");
        return false;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        Serial.println("[OTA] esp_ota_set_boot_partition failed");
        return false;
    }
    Serial.println("[OTA] flash complete, rebooting into new image");
    return true;
}

} // namespace Ota
