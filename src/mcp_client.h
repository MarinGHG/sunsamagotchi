#pragma once

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "net_cfg.h"
#include "ui.h"

// ─── MCP Streamable HTTP Client ─────────────────────────────────────────────
// Implements a minimal MCP client over Streamable HTTP transport.
// Uses JSON-RPC 2.0 over HTTPS POST to api.sunsama.com/mcp
// Auth via hardcoded Bearer token in Authorization header.

class MCPClient {
public:
    MCPClient() : _requestId(1), _initialized(false) {}

    // ── Initialize MCP session ──────────────────────────────────────────────
    bool begin() {
        _sslClient.setInsecure(); // Skip cert verification (ESP32 limitation)
        _sslClient.setTimeout(20);

        if (!mcpInitialize()) {
            Serial.println("[MCP] Initialize failed");
            return false;
        }

        // Send initialized notification
        sendNotification("notifications/initialized");
        _initialized = true;
        Serial.println("[MCP] Session established");
        return true;
    }

    // ── Read resource: tasks for a day ──────────────────────────────────────
    bool fetchTasks(const char* dateStr, TaskItem* tasks, uint8_t& count, uint8_t maxCount) {
        char uri[64];
        snprintf(uri, sizeof(uri), "sunsama://tasks/%s", dateStr);

        JsonDocument doc;
        if (!readResource(uri, doc)) return false;

        count = 0;
        JsonArray contents = doc["result"]["contents"].as<JsonArray>();
        if (contents.isNull()) return false;

        // The resource returns JSON text in contents[0].text
        const char* jsonText = contents[0]["text"];
        if (!jsonText) return false;

        JsonDocument taskDoc;
        DeserializationError err = deserializeJson(taskDoc, jsonText);
        if (err) {
            Serial.printf("[MCP] Task parse error: %s\n", err.c_str());
            return false;
        }

        JsonArray taskArr = taskDoc["tasks"].as<JsonArray>();
        if (taskArr.isNull()) return false;

        for (JsonObject t : taskArr) {
            if (count >= maxCount) break;
            TaskItem& item = tasks[count];

            strlcpy(item.title, t["title"] | "", sizeof(item.title));
            sanitizeUTF8Latin(item.title);
            strlcpy(item.channel, t["channel"] | "", sizeof(item.channel));
            sanitizeUTF8Latin(item.channel);
            strlcpy(item.timeEst, t["timeEstimate"] | "", sizeof(item.timeEst));
            strlcpy(item.id, t["_id"] | "", sizeof(item.id));
            item.completed = t["completed"] | false;
            item.overcommitted = t["isOvercommitted"] | false;

            // Projected time entries
            item.projStart[0] = '\0';
            item.projEnd[0] = '\0';
            JsonArray proj = t["projectedTimeEntries"].as<JsonArray>();
            if (!proj.isNull() && proj.size() > 0) {
                strlcpy(item.projStart, proj[0]["startTime"] | "", sizeof(item.projStart));
                strlcpy(item.projEnd, proj[0]["endTime"] | "", sizeof(item.projEnd));
            }

            // Simplify time estimate for display
            simplifyTimeEst(item.timeEst);

            count++;
        }

        Serial.printf("[MCP] Fetched %d tasks\n", count);
        return true;
    }

    // ── Read resource: calendar events for a day ────────────────────────────
    bool fetchEvents(const char* dateStr, EventItem* events, uint8_t& count, uint8_t maxCount) {
        char uri[80];
        snprintf(uri, sizeof(uri), "sunsama://calendar/events/%s", dateStr);

        JsonDocument doc;
        if (!readResource(uri, doc)) return false;

        count = 0;
        JsonArray contents = doc["result"]["contents"].as<JsonArray>();
        if (contents.isNull()) return false;

        const char* jsonText = contents[0]["text"];
        if (!jsonText) return false;

        JsonDocument evDoc;
        DeserializationError err = deserializeJson(evDoc, jsonText);
        if (err) return false;

        JsonArray evArr = evDoc["events"].as<JsonArray>();
        if (evArr.isNull()) return false;

        for (JsonObject e : evArr) {
            if (count >= maxCount) break;
            EventItem& item = events[count];

            strlcpy(item.title, e["title"] | "", sizeof(item.title));
            sanitizeUTF8Latin(item.title);
            strlcpy(item.startTime, e["startTime"] | "", sizeof(item.startTime));
            item.durationMin = e["duration"] | 0;
            item.isAllDay = e["isAllDay"] | false;

            count++;
        }

        Serial.printf("[MCP] Fetched %d events\n", count);
        return true;
    }

    // ── Read resource: active timer ─────────────────────────────────────────
    bool fetchTimer(TimerInfo& timer) {
        JsonDocument doc;
        if (!readResource("sunsama://active-timer", doc)) return false;

        JsonArray contents = doc["result"]["contents"].as<JsonArray>();
        if (contents.isNull()) return false;

        const char* jsonText = contents[0]["text"];
        if (!jsonText) return false;

        JsonDocument timerDoc;
        DeserializationError err = deserializeJson(timerDoc, jsonText);
        if (err) return false;

        timer.active = timerDoc["hasActiveTimer"] | false;
        if (timer.active) {
            JsonObject at = timerDoc["activeTimer"];
            strlcpy(timer.taskTitle, at["taskTitle"] | "Task", sizeof(timer.taskTitle));
            sanitizeUTF8Latin(timer.taskTitle);
            strlcpy(timer.taskId, at["taskId"] | "", sizeof(timer.taskId));
            // MCP resource returns `startTime` (ISO-8601 UTC), not `elapsedSeconds`.
            const char* startIso = at["startTime"] | at["startedAt"] | "";
            strlcpy(timer.startedAt, startIso, sizeof(timer.startedAt));
            timer.elapsedSec = 0;
            if (startIso[0]) {
                struct tm tmUtc = {};
                int ms = 0;
                // Parse "YYYY-MM-DDTHH:MM:SS" (ignore fractional + Z)
                if (sscanf(startIso, "%d-%d-%dT%d:%d:%d",
                           &tmUtc.tm_year, &tmUtc.tm_mon, &tmUtc.tm_mday,
                           &tmUtc.tm_hour, &tmUtc.tm_min, &tmUtc.tm_sec) == 6) {
                    tmUtc.tm_year -= 1900;
                    tmUtc.tm_mon  -= 1;
                    // No timegm on ESP32 newlib — switch TZ to UTC0, mktime, restore.
                    char* oldTz = getenv("TZ");
                    char saved[40] = {0};
                    if (oldTz) strlcpy(saved, oldTz, sizeof(saved));
                    setenv("TZ", "UTC0", 1); tzset();
                    time_t startEpoch = mktime(&tmUtc);
                    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
                    tzset();
                    time_t now; time(&now);
                    if (now > startEpoch && startEpoch > 1600000000) {
                        timer.elapsedSec = (uint32_t)(now - startEpoch);
                    }
                }
                (void)ms;
            }
        } else {
            timer.taskTitle[0] = '\0';
            timer.taskId[0] = '\0';
            timer.startedAt[0] = '\0';
            timer.elapsedSec = 0;
        }

        return true;
    }

    // ── Read resource: plan summary ─────────────────────────────────────────
    bool fetchPlanSummary(const char* dateStr, PlanSummary& plan) {
        char uri[80];
        snprintf(uri, sizeof(uri), "sunsama://total-planned-time/%s", dateStr);

        JsonDocument doc;
        if (!readResource(uri, doc)) return false;

        JsonArray contents = doc["result"]["contents"].as<JsonArray>();
        if (contents.isNull()) return false;

        const char* jsonText = contents[0]["text"];
        if (!jsonText) return false;

        JsonDocument planDoc;
        DeserializationError err = deserializeJson(planDoc, jsonText);
        if (err) return false;

        strlcpy(plan.totalRemaining, planDoc["totalRemainingTime"] | "?", sizeof(plan.totalRemaining));
        strlcpy(plan.totalWork, planDoc["totalRemainingWorkTime"] | "?", sizeof(plan.totalWork));
        strlcpy(plan.totalPersonal, planDoc["totalRemainingPersonalTime"] | "?", sizeof(plan.totalPersonal));
        strlcpy(plan.shutdown, planDoc["preferredShutdownTime"] | "?", sizeof(plan.shutdown));
        strlcpy(plan.overcommitted, planDoc["totalOvercommittedTime"] | "0", sizeof(plan.overcommitted));

        // Debug: print raw plan values so we can diagnose unexpected numbers
        Serial.printf("[Plan] remaining='%s' work='%s' personal='%s' shutdown='%s' over='%s'\n",
                      plan.totalRemaining, plan.totalWork, plan.totalPersonal,
                      plan.shutdown, plan.overcommitted);

        // Simplify display strings
        simplifyDuration(plan.totalRemaining);
        simplifyDuration(plan.overcommitted);

        return true;
    }

    // ── Tool call: start timer ──────────────────────────────────────────────
    bool startTimer(const char* taskId) {
        JsonDocument params;
        params["taskId"] = taskId;
        JsonDocument result;
        return callTool("start_task_timer", params, result);
    }

    // ── Tool call: stop timer ───────────────────────────────────────────────
    bool stopTimer(const char* taskId) {
        JsonDocument params;
        params["taskId"] = taskId;
        JsonDocument result;
        return callTool("stop_task_timer", params, result);
    }

    // ── Tool call: mark task completed ──────────────────────────────────────
    bool completeTask(const char* taskId, const char* dateStr) {
        JsonDocument params;
        params["taskId"] = taskId;
        params["finishedDay"] = dateStr;
        JsonDocument result;
        return callTool("mark_task_as_completed", params, result);
    }

    // ── Tool call: mark task incomplete ─────────────────────────────────────
    bool uncompleteTask(const char* taskId) {
        JsonDocument params;
        params["taskId"] = taskId;
        JsonDocument result;
        return callTool("mark_task_as_incomplete", params, result);
    }

private:
    WiFiClientSecure _sslClient;
    uint32_t _requestId;
    bool _initialized;

    // ── Send JSON-RPC request over HTTPS POST ───────────────────────────────
    bool sendRequest(JsonDocument& request, JsonDocument& response) {
        HTTPClient http;
        String url = String("https://") + MCP_HOST + MCP_ENDPOINT;

        http.begin(_sslClient, url);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json, text/event-stream");
        http.addHeader("Authorization", String("Bearer ") + NetCfg::token());
        http.setTimeout(20000);
        const char* collectHeaders[] = {"Content-Type"};
        http.collectHeaders(collectHeaders, 1);

        String body;
        serializeJson(request, body);
        Serial.printf("[MCP] >> %s\n", request["method"].as<const char*>());

        int httpCode = http.POST(body);

        if (httpCode < 0) {
            Serial.printf("[MCP] HTTP error: %s\n", http.errorToString(httpCode).c_str());
            http.end();
            return false;
        }

        // 202 Accepted = notification accepted, no body
        if (httpCode == 202) {
            http.end();
            return true;
        }

        if (httpCode != 200) {
            Serial.printf("[MCP] HTTP %d\n", httpCode);
            String errBody = http.getString();
            Serial.println(errBody.substring(0, 300));
            http.end();
            return false;
        }

        // Check content type
        String contentType = http.header("Content-Type");

        if (contentType.indexOf("text/event-stream") >= 0) {
            // Parse SSE stream — read line by line, extract data: lines
            WiFiClient* stream = http.getStreamPtr();
            String jsonBody = "";
            unsigned long deadline = millis() + 20000;
            while (http.connected() && millis() < deadline) {
                if (stream->available()) {
                    String line = stream->readStringUntil('\n');
                    line.trim();
                    if (line.startsWith("data:")) {
                        String data = line.substring(5);
                        data.trim();
                        if (data.length() > 0 && data[0] == '{') {
                            jsonBody = data;
                        }
                    }
                } else {
                    delay(10);
                }
            }
            http.end();

            if (jsonBody.length() == 0) {
                Serial.println("[MCP] SSE: no JSON data received");
                return false;
            }

            DeserializationError err = deserializeJson(response, jsonBody);
            if (err) {
                Serial.printf("[MCP] SSE parse error: %s\n", err.c_str());
                return false;
            }
        } else {
            // application/json — HTTPClient handles chunked encoding
            String jsonBody = http.getString();
            http.end();

            if (jsonBody.length() == 0) {
                Serial.println("[MCP] Empty response body");
                return false;
            }

            DeserializationError err = deserializeJson(response, jsonBody);
            if (err) {
                Serial.printf("[MCP] JSON parse error: %s\n", err.c_str());
                Serial.println(jsonBody.substring(0, 200));
                return false;
            }
        }

        // Check for JSON-RPC error
        if (!response["error"].isNull()) {
            String errStr;
            serializeJsonPretty(response["error"], errStr);
            Serial.printf("[MCP] RPC error: %s\n", errStr.c_str());
            return false;
        }

        return true;
    }

    // ── Send notification (no response expected) ────────────────────────────
    bool sendNotification(const char* method) {
        JsonDocument req;
        req["jsonrpc"] = "2.0";
        req["method"] = method;
        JsonDocument resp;
        return sendRequest(req, resp);
    }

    // ── MCP initialize handshake ────────────────────────────────────────────
    bool mcpInitialize() {
        JsonDocument req;
        req["jsonrpc"] = "2.0";
        req["id"] = _requestId++;
        req["method"] = "initialize";
        req["params"]["protocolVersion"] = "2025-03-26";
        req["params"]["capabilities"].to<JsonObject>(); // empty {}
        req["params"]["clientInfo"]["name"] = "CoreInk-Pager";
        req["params"]["clientInfo"]["version"] = "1.0.0";

        JsonDocument resp;
        if (!sendRequest(req, resp)) return false;

        const char* serverName = resp["result"]["serverInfo"]["name"];
        Serial.printf("[MCP] Server: %s\n", serverName ? serverName : "unknown");
        return true;
    }

    // ── Read an MCP resource ────────────────────────────────────────────────
    bool readResource(const char* uri, JsonDocument& response) {
        JsonDocument req;
        req["jsonrpc"] = "2.0";
        req["id"] = _requestId++;
        req["method"] = "resources/read";
        req["params"]["uri"] = uri;

        return sendRequest(req, response);
    }

    // ── Call an MCP tool ────────────────────────────────────────────────────
    bool callTool(const char* toolName, JsonDocument& arguments, JsonDocument& response) {
        JsonDocument req;
        req["jsonrpc"] = "2.0";
        req["id"] = _requestId++;
        req["method"] = "tools/call";
        req["params"]["name"] = toolName;
        req["params"]["arguments"] = arguments;

        return sendRequest(req, response);
    }

    // ── Replace German umlaut UTF-8 sequences with ASCII equivalents ────────
    // ä(C3A4)→ae  ö(C3B6)→oe  ü(C3BC)→ue  Ä(C384)→Ae  Ö(C396)→Oe  Ü(C39C)→Ue  ß(C39F)→ss
    // Each umlaut is exactly 2 UTF-8 bytes → 2 ASCII chars, so in-place is safe.
    static void sanitizeUTF8Latin(char* str) {
        if (!str) return;
        static const struct { uint8_t b1, b2; char c1, c2; } map[] = {
            {0xC3, 0xA4, 'a', 'e'}, {0xC3, 0xB6, 'o', 'e'}, {0xC3, 0xBC, 'u', 'e'},
            {0xC3, 0x84, 'A', 'e'}, {0xC3, 0x96, 'O', 'e'}, {0xC3, 0x9C, 'U', 'e'},
            {0xC3, 0x9F, 's', 's'},
        };
        int len = (int)strlen(str);
        for (int i = 0; i < len - 1; i++) {
            uint8_t c = (uint8_t)str[i];
            if (c < 0xC0) continue;
            for (auto& e : map) {
                if (c == e.b1 && (uint8_t)str[i + 1] == e.b2) {
                    str[i]     = e.c1;
                    str[i + 1] = e.c2;
                    break;
                }
            }
        }
    }

    // ── Simplify "X hours and Y minutes" to "Xh Ym" ────────────────────────
    void simplifyTimeEst(char* str) {
        if (!str || strlen(str) == 0) return;

        int hours = 0, minutes = 0;
        // sscanf returns conversion count even if later literal fails to match,
        // so "40 minutes" would wrongly match "%d hours". Check substring first.
        bool hasHours   = strstr(str, "hour")   != nullptr;
        bool hasMinutes = strstr(str, "minute") != nullptr;
        if (hasHours && hasMinutes) {
            sscanf(str, "%d hours and %d minutes", &hours, &minutes);
            snprintf(str, 16, "%dh%02dm", hours, minutes);
        } else if (hasHours) {
            sscanf(str, "%d", &hours);
            snprintf(str, 16, "%dh", hours);
        } else if (hasMinutes) {
            sscanf(str, "%d", &minutes);
            snprintf(str, 16, "%dm", minutes);
        }
    }

    void simplifyDuration(char* str) {
        simplifyTimeEst(str); // Same format
    }
};
