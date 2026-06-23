// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Marin Benke
// Sunsamagotchi — a Sunsama companion for M5Stack devices
#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include "config.h"
#include "hal.h"
#include "settings.h"
#include "ui.h"
#include "mcp_client.h"

// ─── RTC-retained state (survives deep sleep) ───────────────────────────────
#define RTC_MAGIC 0xCAFE1238  // bump when RTC_DATA_ATTR layout changes

RTC_DATA_ATTR uint32_t  rtcMagic = 0;
RTC_DATA_ATTR Screen    rtcScreen = SCREEN_DASHBOARD;
RTC_DATA_ATTR uint8_t   rtcTaskScrollOff = 0;
RTC_DATA_ATTR uint8_t   rtcTaskSelIdx = 0;
RTC_DATA_ATTR uint8_t   rtcTimerSelIdx = 0;
RTC_DATA_ATTR uint8_t   rtcSettSelIdx = 0;
RTC_DATA_ATTR TaskItem  rtcTasks[MAX_TASKS];
RTC_DATA_ATTR uint8_t   rtcTaskCount = 0;
RTC_DATA_ATTR EventItem rtcEvents[MAX_EVENTS];
RTC_DATA_ATTR uint8_t   rtcEventCount = 0;
RTC_DATA_ATTR TimerInfo rtcTimer;
RTC_DATA_ATTR PlanSummary rtcPlan;
RTC_DATA_ATTR bool      rtcDataValid = false;
RTC_DATA_ATTR time_t    rtcLastDataFetch = 0;  // unix ts of last WiFi data fetch
// Timer sync anchor — allows elapsed time to track wall-clock through deep sleep
RTC_DATA_ATTR uint8_t   rtcEventScrollOff = 0;
RTC_DATA_ATTR uint8_t   rtcTimerScrollOff = 0;
RTC_DATA_ATTR time_t    rtcTimerFetchedAt = 0;       // system time when timer was last fetched
RTC_DATA_ATTR uint32_t  rtcTimerElapsedAtFetch = 0;  // elapsedSec at that moment
// Counts timer-driven wakes.  Minute standby redraws stay on the same fast
// partial waveform used by normal UI navigation.
RTC_DATA_ATTR uint16_t  rtcWakeCounter = 0;
#define FULL_REFRESH_EVERY 0
// Battery-life estimation: store a baseline % + timestamp.  Reset on charge
// (battery jumps up).  ETA = remaining% / drain-rate.
RTC_DATA_ATTR int8_t    rtcBattRefPct = -1;
RTC_DATA_ATTR time_t    rtcBattRefTs  = 0;
// When a timer is active we already track elapsed locally and don't need the
// API for the second-by-second value — we only need to occasionally check if
// the timer was stopped on another device.  This is the floor (in minutes)
// for the API sync interval while a timer is running.
#define TIMER_ACTIVE_MIN_REFRESH_MIN 10

// ─── RAM state ──────────────────────────────────────────────────────────────
static M5Canvas canvas(&M5.Display);
static MCPClient mcp;

static Screen     currentScreen   = SCREEN_DASHBOARD;
static uint8_t    taskScrollOff   = 0;
static uint8_t    taskSelectedIdx = 0;
static uint8_t    timerSelIdx     = 0;
static uint8_t    timerScrollOff  = 0;
static uint8_t    eventScrollOff  = 0;
static uint8_t    settSelIdx      = 0;

static TaskItem   tasks[MAX_TASKS];
static uint8_t    taskCount = 0;
static EventItem  events[MAX_EVENTS];
static uint8_t    eventCount = 0;
static TimerInfo  timerInfo;
static PlanSummary planSummary;
static AppSettings appSettings;

static char       currentTime[8]  = "--:--";
static char       currentDate[20] = "";
static char       todayStr[12]    = "";

static uint32_t   lastActivity    = 0;
static uint32_t   lastRefresh     = 0;
static bool       needsRedraw     = true;
static bool       dataLoaded      = false;
static bool       mcpReady        = false;
static bool       fetchInProgress = false;
static bool       showingIntro    = false;
static bool       settingsEditMode = false;

// Confirmation dialog state
static ConfirmState confirmState = CONFIRM_NONE;

// Deferred WiFi connection (non-blocking wake)
static bool       pendingWiFiConnect = false;
static uint32_t   wifiConnectStart   = 0;

// StickC display-off state (backlight off to save power)
static bool       displayOff         = false;
#define DISPLAY_OFF_MS 30000

// Debounce: ignore first button press on wake from deep sleep
static bool       wakeDebounce    = false;
static uint32_t   wakeTime        = 0;
#define WAKE_DEBOUNCE_MS 600

// ── Helper: push canvas to display ──
void pushDisplay() {
    HAL::pushCanvas(canvas);
}

// ─── Async fetch state (runs on Core 0 so UI never freezes) ─────────────────
static TaskItem    stageTasks[MAX_TASKS];
static uint8_t     stageTaskCount = 0;
static EventItem   stageEvents[MAX_EVENTS];
static uint8_t     stageEventCount = 0;
static TimerInfo   stageTimer;
static PlanSummary stagePlan;
static volatile bool      asyncFetchRunning = false;
static volatile bool      asyncFetchDone    = false;
static volatile bool      asyncFetchOk      = false;
static TaskHandle_t       asyncFetchHandle  = nullptr;

// ─── WiFi ───────────────────────────────────────────────────────────────────
bool connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 60) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.printf("[WiFi] FAILED after %d attempts (status=%d)\n", attempts, WiFi.status());
    return false;
}

// ─── NTP Time ───────────────────────────────────────────────────────────────
void syncTime() {
    struct timeval resetTv = { 0, 0 };
    settimeofday(&resetTv, nullptr);
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);
    struct tm timeinfo;
    int tries = 0;
    while (!getLocalTime(&timeinfo) && tries < 10) {
        delay(500);
        tries++;
    }
    if (tries < 10) {
        struct tm tmLocal;
        getLocalTime(&tmLocal);
        time_t epoch; time(&epoch);
        char ls[20]; strftime(ls, sizeof(ls), "%Y-%m-%d %H:%M:%S", &tmLocal);
        Serial.printf("[NTP] Time synced — local=%s utc_epoch=%lld\n",
                      ls, (long long)epoch);
    } else {
        Serial.println("[NTP] Sync failed");
    }
}

// ── Write system time to hardware RTC (BM8563) after NTP sync ───────────────
// BM8563 stores UTC. Must be called after configTime()/syncTime() so time() is valid.
void syncHardwareRTC() {
    time_t now;
    time(&now);
    if (now < 1600000000) return;  // system time not set yet
    struct tm utc;
    gmtime_r(&now, &utc);
    auto dt = M5.Rtc.getDateTime();
    dt.date.year    = utc.tm_year + 1900;
    dt.date.month   = utc.tm_mon + 1;
    dt.date.date    = utc.tm_mday;
    dt.date.weekDay = utc.tm_wday;
    dt.time.hours   = utc.tm_hour;
    dt.time.minutes = utc.tm_min;
    dt.time.seconds = utc.tm_sec;
    Serial.printf("[RTC] WRITE  y%d-%02d-%02d %02d:%02d:%02d UTC (epoch=%lld)\n",
                  dt.date.year, dt.date.month, dt.date.date,
                  dt.time.hours, dt.time.minutes, dt.time.seconds,
                  (long long)now);
    M5.Rtc.setDateTime(dt);
    auto rb = M5.Rtc.getDateTime();
    Serial.printf("[RTC] RBACK  y%d-%02d-%02d %02d:%02d:%02d UTC\n",
                  rb.date.year, rb.date.month, rb.date.date,
                  rb.time.hours, rb.time.minutes, rb.time.seconds);
}

// Read time directly from hardware RTC (BM8563) — instant, no WiFi needed.
// M5Unified syncs the BM8563 from gmtime() (UTC) after NTP.
// We adjust for TZ_OFFSET_SEC and also set the system clock so time() works.
void updateTimeFromHardwareRTC() {
    auto dt = M5.Rtc.getDateTime();
    Serial.printf("[RTC] READ   y%d-%02d-%02d %02d:%02d:%02d UTC\n",
                  dt.date.year, dt.date.month, dt.date.date,
                  dt.time.hours, dt.time.minutes, dt.time.seconds);
    int tzH    = TZ_OFFSET_SEC / 3600;
    int localH = (dt.time.hours + tzH + 24) % 24;

    if (appSettings.use24h) {
        snprintf(currentTime, sizeof(currentTime), "%02d:%02d",
                 localH, (int)dt.time.minutes);
    } else {
        int h = localH % 12; if (h == 0) h = 12;
        snprintf(currentTime, sizeof(currentTime), "%d:%02d",
                 h, (int)dt.time.minutes);
    }
    static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char* DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    uint8_t m = dt.date.month;   if (m < 1 || m > 12) m = 1;
    uint8_t w = dt.date.weekDay; if (w > 6) w = 0;
    snprintf(currentDate, sizeof(currentDate), "%s, %02d %s",
             DOW[w], (int)dt.date.date, MON[m - 1]);
    snprintf(todayStr, sizeof(todayStr), "%04d-%02d-%02d",
             (int)dt.date.year, (int)dt.date.month, (int)dt.date.date);

    // Set system time from hardware RTC UTC value so time() comparisons work.
    struct tm t = {};
    t.tm_year  = dt.date.year  - 1900;
    t.tm_mon   = dt.date.month - 1;
    t.tm_mday  = dt.date.date;
    t.tm_hour  = dt.time.hours;   // UTC in RTC
    t.tm_min   = dt.time.minutes;
    t.tm_sec   = dt.time.seconds;
    t.tm_isdst = 0;
    // mktime treats input as local time; temporarily switch TZ to UTC0 to get UTC epoch
    setenv("TZ", "UTC0", 1); tzset();
    time_t utcTs = mktime(&t);
    // Restore timezone
    char tzStr[20];
    snprintf(tzStr, sizeof(tzStr), "UTC-%d", tzH);
    setenv("TZ", tzStr, 1); tzset();
    if (utcTs > 0) {
        struct timeval tv = { utcTs, 0 };
        settimeofday(&tv, nullptr);
    }
}

// ─── Battery-life ETA ───────────────────────────────────────────────────────
// Tracks battery level over time across deep-sleep cycles.  Returns -1 if we
// don't yet have enough history for a meaningful estimate.  Resets baseline
// when the battery jumps up (charging).
static void updateBatteryTracking(int curPct) {
    time_t nowTs; time(&nowTs);
    if (nowTs < 1600000000) return;  // clock not set yet
    if (rtcBattRefPct < 0 || curPct > rtcBattRefPct + 5) {
        // First boot, or device was put on charger — reset baseline.
        rtcBattRefPct = (int8_t)curPct;
        rtcBattRefTs  = nowTs;
        Serial.printf("[Batt] Baseline reset: %d%% @ %lld\n",
                      curPct, (long long)nowTs);
    }
}

// Format ETA like "~5d", "~14h", or "" if unknown.
static void formatBatteryEta(char* out, size_t sz, int curPct) {
    out[0] = '\0';
    time_t nowTs; time(&nowTs);
    if (rtcBattRefPct < 0 || rtcBattRefTs == 0 || nowTs <= rtcBattRefTs) return;
    int dropped = rtcBattRefPct - curPct;
    uint32_t elapsed = (uint32_t)(nowTs - rtcBattRefTs);
    // Need at least ~30 min and ≥1% drop for a stable rate.
    if (dropped < 1 || elapsed < 1800) return;
    if (curPct <= 0) { strlcpy(out, "low", sz); return; }
    float pctPerSec = (float)dropped / (float)elapsed;
    if (pctPerSec <= 0.0f) return;
    float secsLeft = (float)curPct / pctPerSec;
    if (secsLeft >= 86400.0f) {
        snprintf(out, sz, "~%dd", (int)(secsLeft / 86400.0f));
    } else if (secsLeft >= 3600.0f) {
        snprintf(out, sz, "~%dh", (int)(secsLeft / 3600.0f));
    } else {
        snprintf(out, sz, "~%dm", (int)(secsLeft / 60.0f));
    }
}

// ─── Refresh-interval helper ────────────────────────────────────────────────
// While a timer is running, we slow down API sync to save power and battery —
// the elapsed time is computed locally from rtcTimerFetchedAt anyway, and we
// only need to occasionally check whether the timer was stopped on another
// device.  Floored at TIMER_ACTIVE_MIN_REFRESH_MIN minutes.
static uint8_t effectiveRefreshMinutes() {
    uint8_t base = appSettings.refreshMinutes;
    if (timerInfo.active && base < TIMER_ACTIVE_MIN_REFRESH_MIN) {
        return TIMER_ACTIVE_MIN_REFRESH_MIN;
    }
    return base;
}

// ─── Timer sync ─────────────────────────────────────────────────────────────
// Computes elapsed seconds anchored to the last cloud fetch, so the timer
// stays in sync with Sunsama's cloud timer through deep-sleep cycles.
uint32_t timerCurrentElapsed() {
    if (!timerInfo.active || rtcTimerFetchedAt == 0) return timerInfo.elapsedSec;
    time_t now; time(&now);
    if (now <= rtcTimerFetchedAt) return timerInfo.elapsedSec;
    return rtcTimerElapsedAtFetch + (uint32_t)(now - rtcTimerFetchedAt);
}

void updateTimeStrings() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        if (appSettings.use24h) {
            strftime(currentTime, sizeof(currentTime), "%H:%M", &timeinfo);
        } else {
            strftime(currentTime, sizeof(currentTime), "%I:%M", &timeinfo);
        }
        strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", &timeinfo);
        strftime(currentDate, sizeof(currentDate), "%a, %d %b", &timeinfo);
    }
}

// ─── Save / Restore RTC state ───────────────────────────────────────────────
void saveStateToRTC() {
    rtcMagic = RTC_MAGIC;
    rtcScreen = currentScreen;
    rtcTaskScrollOff = taskScrollOff;
    rtcTaskSelIdx = taskSelectedIdx;
    rtcTimerSelIdx = timerSelIdx;
    rtcTimerScrollOff = timerScrollOff;
    rtcEventScrollOff = eventScrollOff;
    rtcSettSelIdx = settSelIdx;
    memcpy(rtcTasks, tasks, sizeof(tasks));
    rtcTaskCount = taskCount;
    memcpy(rtcEvents, events, sizeof(events));
    rtcEventCount = eventCount;
    rtcTimer = timerInfo;
    rtcPlan = planSummary;
    rtcDataValid = dataLoaded;
}

bool restoreStateFromRTC() {
    if (rtcMagic != RTC_MAGIC || !rtcDataValid) return false;
    currentScreen = rtcScreen;
    taskScrollOff = rtcTaskScrollOff;
    taskSelectedIdx = rtcTaskSelIdx;
    timerSelIdx = rtcTimerSelIdx;
    timerScrollOff = rtcTimerScrollOff;
    eventScrollOff = rtcEventScrollOff;
    settSelIdx = rtcSettSelIdx;
    memcpy(tasks, rtcTasks, sizeof(tasks));
    taskCount = rtcTaskCount;
    memcpy(events, rtcEvents, sizeof(events));
    eventCount = rtcEventCount;
    timerInfo = rtcTimer;
    planSummary = rtcPlan;
    dataLoaded = true;
    Serial.printf("[RTC] Restored state: screen=%d tasks=%d events=%d\n",
                  currentScreen, taskCount, eventCount);
    return true;
}

// ─── Data Fetch — runs on Core 0 so UI thread never blocks ──────────────────
// Writes into staging buffers; main loop swaps them into live state when done.
static void asyncFetchTaskFn(void* /*arg*/) {
    Serial.println("[Data] Async fetch starting on core 0");
    bool ok = true;
    stageTaskCount  = 0;
    stageEventCount = 0;
    memset(&stageTimer, 0, sizeof(stageTimer));
    memset(&stagePlan,  0, sizeof(stagePlan));

    ok &= mcp.fetchTasks(todayStr, stageTasks, stageTaskCount, MAX_TASKS);
    ok &= mcp.fetchEvents(todayStr, stageEvents, stageEventCount, MAX_EVENTS);
    ok &= mcp.fetchTimer(stageTimer);
    ok &= mcp.fetchPlanSummary(todayStr, stagePlan);

    asyncFetchOk      = ok;
    asyncFetchDone    = true;
    asyncFetchRunning = false;
    asyncFetchHandle  = nullptr;
    Serial.printf("[Data] Async fetch finished ok=%d tasks=%d events=%d\n",
                  (int)ok, stageTaskCount, stageEventCount);
    vTaskDelete(nullptr);
}

// Start a non-blocking fetch on Core 0.  Returns false if one is already running.
bool startAsyncFetch() {
    if (asyncFetchRunning) return false;
    if (!mcpReady)         return false;
    updateTimeStrings();
    asyncFetchRunning = true;
    asyncFetchDone    = false;
    fetchInProgress   = true;
    BaseType_t r = xTaskCreatePinnedToCore(
        asyncFetchTaskFn, "mcpFetch", 16384, nullptr,
        1, &asyncFetchHandle, 0);   // pin to core 0 (Arduino runs on core 1)
    if (r != pdPASS) {
        Serial.println("[Data] Failed to create fetch task");
        asyncFetchRunning = false;
        fetchInProgress   = false;
        return false;
    }
    return true;
}

// Swap staged data into live state.  Called from main loop on Core 1.
void commitAsyncFetch() {
    if (!asyncFetchDone) return;
    asyncFetchDone = false;

    if (asyncFetchOk) {
        memcpy(tasks,  stageTasks,  sizeof(tasks));
        taskCount = stageTaskCount;
        memcpy(events, stageEvents, sizeof(events));
        eventCount = stageEventCount;
        timerInfo  = stageTimer;
        planSummary = stagePlan;

        if (timerInfo.active) {
            time(&rtcTimerFetchedAt);
            rtcTimerElapsedAtFetch = timerInfo.elapsedSec;
        } else {
            rtcTimerFetchedAt = 0;
            rtcTimerElapsedAtFetch = 0;
        }
        dataLoaded = true;
        time(&rtcLastDataFetch);
        saveStateToRTC();
        Serial.println("[Data] Live state updated");
    } else {
        Serial.println("[Data] Fetch failed, keeping previous data");
    }
    fetchInProgress = false;
    lastRefresh     = millis();
    needsRedraw     = true;
}

// Convenience wrapper retained for compatibility — kicks off async fetch.
void fetchAllData() {
    startAsyncFetch();
}

// ─── Display ────────────────────────────────────────────────────────────────
void redrawScreen() {
    updateTimeStrings();
    // Keep timer elapsed in sync with wall clock (survives deep sleep cycles)
    if (timerInfo.active) timerInfo.elapsedSec = timerCurrentElapsed();
    int batt = M5.Power.getBatteryLevel();

    if (showingIntro) {
        UI::drawIntroScreen(canvas);
        pushDisplay();
        needsRedraw = false;
        return;
    }

    switch (currentScreen) {
        case SCREEN_DASHBOARD:
            UI::drawDashboard(canvas, currentTime, batt,
                              tasks, taskCount, events, eventCount,
                              planSummary, timerInfo, currentDate,
                              appSettings.use24h);
            break;
        case SCREEN_TASKS:
            UI::drawTasksScreen(canvas, tasks, taskCount,
                                 taskScrollOff, taskSelectedIdx);
            break;
        case SCREEN_EVENTS:
            UI::drawEventsScreen(canvas, events, eventCount,
                                  currentDate, appSettings.use24h, eventScrollOff);
            break;
        case SCREEN_TIMER:
            UI::drawTimerScreen(canvas, timerInfo,
                                 tasks, taskCount, timerSelIdx, appSettings.use24h,
                                 timerScrollOff);
            break;
        case SCREEN_STATS:
            UI::drawStatsScreen(canvas, tasks, taskCount, timerInfo, batt);
            break;
        case SCREEN_SETTINGS:
            UI::drawSettingsScreen(canvas, appSettings, settSelIdx, batt, settingsEditMode);
            break;
        default: break;
    }

    // Overlay confirmation dialog if active
    if (confirmState != CONFIRM_NONE) {
        const char* dlgTitle = (confirmState == CONFIRM_UNCOMPLETE_TASK)
                             ? "Uncomplete task?" : "Complete task?";
        UI::drawConfirmDialog(canvas, dlgTitle,
                              tasks[taskSelectedIdx].title);
    }

    pushDisplay();
    needsRedraw = false;
}

// ─── Scroll helper: advance task selection with wrap ────────────────────────
void scrollTaskSelection() {
    int maxVis = (SCREEN_H - 30 - 14) / 18;
    if (taskCount > 0) {
        taskSelectedIdx = (taskSelectedIdx + 1) % taskCount;
        if (taskSelectedIdx < taskScrollOff) {
            taskScrollOff = taskSelectedIdx;
        } else if (taskSelectedIdx - taskScrollOff >= (uint8_t)maxVis) {
            taskScrollOff = taskSelectedIdx - maxVis + 1;
        }
    }
}

void scrollTaskSelectionUp() {
    int maxVis = (SCREEN_H - 30 - 14) / 18;
    if (taskCount > 0) {
        taskSelectedIdx = (taskSelectedIdx + taskCount - 1) % taskCount;
        if (taskSelectedIdx < taskScrollOff) {
            taskScrollOff = taskSelectedIdx;
        } else if (taskSelectedIdx - taskScrollOff >= (uint8_t)maxVis) {
            taskScrollOff = taskSelectedIdx - maxVis + 1;
        }
    }
}

void ensureTimerSelVisible() {
    uint8_t visPos = 0;
    for (int i = 0; i < timerSelIdx; i++) {
        if (!tasks[i].completed) visPos++;
    }
    int maxVis = (UI::BODY_BOT - UI::BODY_TOP - 21) / UI::ROW_H;
    if (maxVis < 1) maxVis = 1;
    if (visPos < timerScrollOff) timerScrollOff = visPos;
    else if ((int)(visPos - timerScrollOff) >= maxVis)
        timerScrollOff = visPos - maxVis + 1;
}

void scrollTimerSelection() {
    if (taskCount > 0) {
        uint8_t incomp = 0;
        for (int i = 0; i < taskCount; i++) if (!tasks[i].completed) incomp++;
        if (incomp > 0) {
            int tries = 0;
            do {
                timerSelIdx = (timerSelIdx + 1) % taskCount;
                tries++;
            } while (tasks[timerSelIdx].completed && tries < taskCount);
            ensureTimerSelVisible();
        }
    }
}

void scrollTimerSelectionUp() {
    if (taskCount > 0) {
        uint8_t incomp = 0;
        for (int i = 0; i < taskCount; i++) if (!tasks[i].completed) incomp++;
        if (incomp > 0) {
            int tries = 0;
            do {
                timerSelIdx = (timerSelIdx + taskCount - 1) % taskCount;
                tries++;
            } while (tasks[timerSelIdx].completed && tries < taskCount);
            ensureTimerSelVisible();
        }
    }
}

// ─── Button Handling ────────────────────────────────────────────────────────
// CoreInk:  Dial UP/DOWN=scroll/navigate, Dial PRESS=select, EXT=page, EXT hold=refresh
// StickC:   UP/DOWN=scroll or page, FRONT=select, FRONT hold=refresh

// ── Event scroll ────────────────────────────────────────────────────────────
void scrollEventDown() {
    if (eventCount == 0) return;
    int eventH = IS_EINK ? 30 : 22;
    int maxVis = (UI::BODY_BOT - UI::BODY_TOP - 2) / eventH;
    if (eventCount <= (uint8_t)maxVis) return;
    if (eventScrollOff + maxVis < eventCount) eventScrollOff++;
    else eventScrollOff = 0;  // wrap
}

void scrollEventUp() {
    if (eventCount == 0) return;
    int eventH = IS_EINK ? 30 : 22;
    int maxVis = (UI::BODY_BOT - UI::BODY_TOP - 2) / eventH;
    if (eventCount <= (uint8_t)maxVis) return;
    if (eventScrollOff > 0) eventScrollOff--;
    else eventScrollOff = eventCount - maxVis;  // wrap
}

// Helper: is current screen a "list" screen where UP/DOWN scroll items?
bool isListScreen() {
    if (currentScreen == SCREEN_TASKS) return true;
    if (currentScreen == SCREEN_EVENTS) return true;
    if (currentScreen == SCREEN_TIMER && !timerInfo.active) return true;
    if (currentScreen == SCREEN_SETTINGS) return true;
    return false;
}

void handleButtons() {
    M5.update();
#ifdef BOARD_M5STICK_CPLUS2
    HAL::pollG35();   // Edge-detect GPIO35 (upper-left / power button)
#endif

    // Debounce: skip the wake-up button press
    if (wakeDebounce) {
        if (millis() - wakeTime < WAKE_DEBOUNCE_MS) return;
        wakeDebounce = false;
    }

    // Display-off wake (StickC: any button turns display back on, consumes press)
#if !IS_EINK
    if (displayOff) {
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || HAL::g35JustPressed()) {
            HAL::backlightOn();
            displayOff = false;
            lastActivity = millis();
            needsRedraw = true;
        }
        return;
    }
#endif

    // During an in-progress fetch, allow only navigation (page / up / down) and
    // suppress destructive actions (timer start/stop, task complete, settings).
    // We do this further below at each action site rather than early-returning,
    // so the UI stays fully responsive while data loads on core 0.

#ifdef BOARD_M5STICK_CPLUS2
    // BtnEXT (upper-left, GPIO35) doubles as hardware power button.
    // A 6-second hold powers the device off. Show a countdown overlay once
    // the hold exceeds 1 s so the user can release if it was accidental.
    {
        static uint32_t powerHoldStart = 0;
        static int8_t   lastCountdown  = -1;

        if (HAL::g35IsHeld()) {
            if (powerHoldStart == 0) powerHoldStart = millis();
            uint32_t heldMs = millis() - powerHoldStart;
            if (heldMs >= 1000) {
                int8_t secsLeft = 6 - (int8_t)(heldMs / 1000);
                if (secsLeft < 0) secsLeft = 0;
                if (secsLeft != lastCountdown) {
                    lastCountdown = secsLeft;
                    int16_t bx = SCREEN_W / 2 - 70;
                    int16_t by = SCREEN_H / 2 - 18;
                    canvas.fillRoundRect(bx, by, 140, 36, 6, TFT_RED);
                    canvas.setTextColor(TFT_WHITE, TFT_RED);
                    canvas.setFont(&fonts::FreeSansBold9pt7b);
                    char buf[28];
                    snprintf(buf, sizeof(buf), "Powering off: %d", secsLeft);
                    int16_t tw = canvas.textWidth(buf);
                    canvas.drawString(buf, SCREEN_W / 2 - tw / 2, by + 10);
                    pushDisplay();
                }
                lastActivity = millis(); // prevent auto-sleep during countdown
                return;                  // suppress all other button handling while held
            }
        } else {
            if (lastCountdown >= 0) needsRedraw = true; // restore screen on early release
            powerHoldStart = 0;
            lastCountdown  = -1;
        }
    }
#endif

    bool activity = false;

    if (showingIntro) {
        if (HAL::confirmPressed() || HAL::pagePressed()) {
            Settings::markIntroSeen(appSettings);
            showingIntro = false;
            needsRedraw = true;
            activity = true;
        }
        if (activity) lastActivity = millis();
        return;
    }

    // ── Confirmation dialog mode ──
    if (confirmState != CONFIRM_NONE) {
        if (HAL::confirmPressed()) {
            activity = true;
            if (confirmState == CONFIRM_COMPLETE_TASK && taskSelectedIdx < taskCount) {
                TaskItem& t = tasks[taskSelectedIdx];
                Serial.printf("[Action] Completing: %s\n", t.title);
                if (mcp.completeTask(t.id, todayStr)) {
                    t.completed = true;
                    saveStateToRTC();
                    UI::drawCompletionAnim(canvas, t.title);
                    pushDisplay();
                    delay(1500);
                }
            } else if (confirmState == CONFIRM_UNCOMPLETE_TASK && taskSelectedIdx < taskCount) {
                TaskItem& t = tasks[taskSelectedIdx];
                Serial.printf("[Action] Uncompleting: %s\n", t.title);
                if (mcp.uncompleteTask(t.id)) {
                    t.completed = false;
                    saveStateToRTC();
                }
            }
            confirmState = CONFIRM_NONE;
            needsRedraw = true;
        }
        if (HAL::upPressed() || HAL::downPressed() || HAL::pagePressed()) {
            // Cancel
            confirmState = CONFIRM_NONE;
            needsRedraw = true;
            activity = true;
        }
        if (activity) lastActivity = millis();
        return;
    }

    // ── PAGE button (check before force refresh to avoid false triggers) ──
    static uint32_t lastPagePressMs = 0;
    if (HAL::pagePressed()) {
        currentScreen = (Screen)((currentScreen + 1) % SCREEN_COUNT);
        settingsEditMode = false;
        needsRedraw = true;
        activity = true;
        lastPagePressMs = millis();
        Serial.printf("[Nav] Page -> Screen: %d\n", currentScreen);
    }

    // ── Force refresh (silent) — cooldown prevents false triggers from rapid paging ──
    if (HAL::forceRefreshHeld() && !activity && !fetchInProgress
        && (millis() - lastPagePressMs > 1000)) {
        Serial.println("[Action] Force refresh");
        if (mcpReady) startAsyncFetch();
        activity = true;
    }

    // ── UP button ──
    if (HAL::upPressed()) {
        activity = true;
        if (isListScreen()) {
            // Scroll within current list
            switch (currentScreen) {
                case SCREEN_TASKS:
                    scrollTaskSelectionUp();
                    break;
                case SCREEN_EVENTS:
                    scrollEventUp();
                    break;
                case SCREEN_TIMER:
                    scrollTimerSelectionUp();
                    break;
                case SCREEN_SETTINGS:
                    if (settingsEditMode) Settings::cyclePrev(appSettings, (SettingsItem)settSelIdx);
                    else settSelIdx = (settSelIdx + SETT_COUNT - 1) % SETT_COUNT;
                    break;
                default: break;
            }
        } else {
            // Navigate to previous screen
            currentScreen = (Screen)((currentScreen + SCREEN_COUNT - 1) % SCREEN_COUNT);
            settingsEditMode = false;
            Serial.printf("[Nav] Screen: %d\n", currentScreen);
        }
        needsRedraw = true;
    }

    // ── DOWN button ──
    if (HAL::downPressed()) {
        activity = true;
        if (isListScreen()) {
            // Scroll within current list
            switch (currentScreen) {
                case SCREEN_TASKS:
                    scrollTaskSelection();
                    break;
                case SCREEN_EVENTS:
                    scrollEventDown();
                    break;
                case SCREEN_TIMER:
                    scrollTimerSelection();
                    break;
                case SCREEN_SETTINGS:
                    if (settingsEditMode) Settings::cycleNext(appSettings, (SettingsItem)settSelIdx);
                    else settSelIdx = (settSelIdx + 1) % SETT_COUNT;
                    break;
                default: break;
            }
        } else {
            // Navigate to next screen
            currentScreen = (Screen)((currentScreen + 1) % SCREEN_COUNT);
            settingsEditMode = false;
            Serial.printf("[Nav] Screen: %d\n", currentScreen);
        }
        needsRedraw = true;
    }

    // ── CONFIRM: Context-specific action ──
    // Suppress destructive actions while a fetch is in progress (mcp client is
    // not re-entrant); navigation still works.
    if (HAL::confirmPressed() && !fetchInProgress) {
        activity = true;

        switch (currentScreen) {
            case SCREEN_TASKS: {
                if (taskSelectedIdx < taskCount) {
                    confirmState = tasks[taskSelectedIdx].completed
                                 ? CONFIRM_UNCOMPLETE_TASK
                                 : CONFIRM_COMPLETE_TASK;
                    needsRedraw = true;
                }
                break;
            }
            case SCREEN_TIMER: {
                if (timerInfo.active) {
                    Serial.printf("[Action] Stopping timer: %s\n", timerInfo.taskTitle);
                    if (mcp.stopTimer(timerInfo.taskId)) {
                        timerInfo.active = false;
                        timerInfo.taskTitle[0] = '\0';
                        timerInfo.elapsedSec = 0;
                        rtcTimerFetchedAt = 0;
                        rtcTimerElapsedAtFetch = 0;
                        saveStateToRTC();
                    }
                } else {
                    if (timerSelIdx < taskCount && !tasks[timerSelIdx].completed) {
                        Serial.printf("[Action] Starting timer: %s\n", tasks[timerSelIdx].title);
                        if (mcp.startTimer(tasks[timerSelIdx].id)) {
                            timerInfo.active = true;
                            strlcpy(timerInfo.taskTitle, tasks[timerSelIdx].title, sizeof(timerInfo.taskTitle));
                            strlcpy(timerInfo.taskId, tasks[timerSelIdx].id, sizeof(timerInfo.taskId));
                            timerInfo.elapsedSec = 0;
                            time(&rtcTimerFetchedAt);
                            rtcTimerElapsedAtFetch = 0;
                            saveStateToRTC();
                        }
                    }
                }
                needsRedraw = true;
                break;
            }
            case SCREEN_SETTINGS: {
                settingsEditMode = !settingsEditMode;
                needsRedraw = true;
                break;
            }
            default:
                break;
        }
    }

    if (activity) lastActivity = millis();
}

// ─── Deep Sleep ─────────────────────────────────────────────────────────────
//
// Design notes for the standby screen:
//   • Desk-readable.  Clock and (if running) timer are the focal points and
//     are sized large enough to read across a desk.
//   • E-ink partial refresh.  Wakes use epd_fastest (DU waveform) so the
//     panel updates like normal UI navigation.
//   • Two layouts, depending on whether a timer is active.  When a timer is
//     running it owns the centre of the screen; the clock and date shrink to
//     a small status bar at the top.
//   • The footer carries actionable status, not decorative text: how recent
//     the data is, battery, and a hint that the dial wakes the device.

void enterDeepSleep() {
    Serial.println("[Sleep] Entering deep sleep...");
    if (timerInfo.active) timerInfo.elapsedSec = timerCurrentElapsed();
    saveStateToRTC();

#if IS_EINK
    rtcWakeCounter++;
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
#endif
    canvas.fillSprite(CLR_BG);
    updateTimeStrings();

    int batt = M5.Power.getBatteryLevel();
    if (batt < 0) batt = 0; if (batt > 100) batt = 100;
    updateBatteryTracking(batt);
    char etaStr[12]; formatBatteryEta(etaStr, sizeof(etaStr), batt);

    // Format minutes-since-last-fetch for the status footer
    char ageStr[16] = "—";
    if (rtcLastDataFetch > 0) {
        time_t nowTs; time(&nowTs);
        if (nowTs > rtcLastDataFetch) {
            uint32_t mins = (uint32_t)((nowTs - rtcLastDataFetch) / 60);
            if (mins < 1) snprintf(ageStr, sizeof(ageStr), "now");
            else if (mins < 60) snprintf(ageStr, sizeof(ageStr), "%lum ago", (unsigned long)mins);
            else snprintf(ageStr, sizeof(ageStr), "%luh ago", (unsigned long)(mins / 60));
        }
    }

    // Count incomplete tasks
    uint8_t doneCnt = 0;
    for (uint8_t i = 0; i < taskCount; i++) if (tasks[i].completed) doneCnt++;

    if (appSettings.standbyScreen == STANDBY_CALENDAR) {
        // ── Sunsama-style day timeline standby (both boards) ──────────────────
        UI::drawCalendarStandby(canvas, events, eventCount, tasks, taskCount,
                                currentDate, currentTime, appSettings.use24h,
                                batt, ageStr, timerInfo.active, timerInfo.elapsedSec);
    } else {

#if IS_EINK
    // ── CoreInk 200x200 standby screen ────────────────────────────────────────
    canvas.setTextColor(CLR_TEXT);

    if (timerInfo.active) {
        // ── TIMER-ACTIVE LAYOUT ───────────────────────────────────────────────
        // Top status bar: small clock left, date right
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.drawString(currentTime, 6, 6);
        int dw = canvas.textWidth(currentDate);
        canvas.drawString(currentDate, SCREEN_W - dw - 6, 6);
        UI::drawDottedLine(canvas, 28);

        // Label
        canvas.setFont(&fonts::Font2);
        const char* lbl = "TIMER RUNNING";
        int lw = canvas.textWidth(lbl);
        canvas.drawString(lbl, (SCREEN_W - lw) / 2, 38);

        // HUGE elapsed time
        char el[16];
        UI::formatElapsedMin(el, sizeof(el), timerInfo.elapsedSec);
        canvas.setFont(&fonts::FreeSansBold24pt7b);
        int ew = canvas.textWidth(el);
        canvas.drawString(el, (SCREEN_W - ew) / 2, 60);

        // Working on …
        canvas.setFont(&fonts::Font0);
        canvas.drawString("Working on:", 6, 118);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        char tt[40]; UI::truncPx(canvas, tt, timerInfo.taskTitle, SCREEN_W - 12);
        canvas.drawString(tt, 6, 130);
    } else {
        // ── IDLE LAYOUT (clock dominates) ─────────────────────────────────────
        // HUGE clock, centred high
        canvas.setFont(&fonts::FreeSansBold24pt7b);
        int tw = canvas.textWidth(currentTime);
        canvas.drawString(currentTime, (SCREEN_W - tw) / 2, 18);

        // Date
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        int dw = canvas.textWidth(currentDate);
        canvas.drawString(currentDate, (SCREEN_W - dw) / 2, 78);

        UI::drawDottedLine(canvas, 102);

        // Tasks summary
        char summary[24];
        snprintf(summary, sizeof(summary), "Tasks  %d / %d done",
                 (int)doneCnt, (int)taskCount);
        canvas.setFont(&fonts::Font2);
        int sw = canvas.textWidth(summary);
        canvas.drawString(summary, (SCREEN_W - sw) / 2, 110);

        // Progress bar
        int pbX = 14, pbY = 128, pbW = SCREEN_W - 28, pbH = 8;
        canvas.drawRect(pbX, pbY, pbW, pbH, CLR_TEXT);
        if (taskCount > 0) {
            int fill = (pbW - 2) * doneCnt / taskCount;
            canvas.fillRect(pbX + 1, pbY + 1, fill, pbH - 2, CLR_TEXT);
        }

        // Next task
        bool foundNext = false;
        for (int i = 0; i < taskCount; i++) {
            if (!tasks[i].completed) {
                canvas.setFont(&fonts::Font0);
                canvas.drawString("NEXT", 6, 146);
                canvas.setFont(&fonts::FreeSansBold9pt7b);
                char tt[40]; UI::truncPx(canvas, tt, tasks[i].title, SCREEN_W - 12);
                canvas.drawString(tt, 6, 158);
                foundNext = true;
                break;
            }
        }
        if (!foundNext) {
            canvas.setFont(&fonts::FreeSansBold9pt7b);
            const char* msg = "All done for today";
            int aw = canvas.textWidth(msg);
            canvas.drawString(msg, (SCREEN_W - aw) / 2, 158);
        }
    }

    // ── Footer status strip (always present) ──────────────────────────────────
    UI::drawDottedLine(canvas, 184);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(CLR_TEXT);
    char foot[32];
    snprintf(foot, sizeof(foot), "sync %s", ageStr);
    canvas.drawString(foot, 4, 190);
    char battStr[20];
    if (etaStr[0]) snprintf(battStr, sizeof(battStr), "%d%% %s", batt, etaStr);
    else           snprintf(battStr, sizeof(battStr), "%d%%", batt);
    int bw = canvas.textWidth(battStr);
    canvas.drawString(battStr, SCREEN_W - bw - 4, 190);

#else
    // ── StickC 240x135: always-on sleep display ───────────────────────────────
    // Dark header strip with sleeping status
    canvas.fillRect(0, 0, SCREEN_W, 16, CLR_HEADER_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(CLR_HEADER_TEXT);
    int shw = canvas.textWidth("Sunsamagotchi is sleeping  zZZ");
    canvas.drawString("Sunsamagotchi is sleeping  zZZ", (SCREEN_W - shw) / 2, 4);

    canvas.setTextColor(CLR_TEXT);
    int midX = SCREEN_W / 2;

    // Left: large clock + date
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    int tw = canvas.textWidth(currentTime);
    canvas.drawString(currentTime, (midX - tw) / 2, 20);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(CLR_TEXT_DIM);
    int dpw = canvas.textWidth(currentDate);
    canvas.drawString(currentDate, (midX - dpw) / 2, 56);
    canvas.setTextColor(CLR_TEXT);

    // Timer indicator
    if (timerInfo.active) {
        canvas.setTextColor(CLR_ACCENT);
        UI::drawIcon8(canvas, 6, 76, UI::ICON_TIMER, CLR_ACCENT);
        canvas.setFont(&fonts::Font0);
        char el[12];
        UI::formatElapsedMin(el, sizeof(el), timerInfo.elapsedSec);
        canvas.drawString(el, 18, 78);
        canvas.setTextColor(CLR_TEXT);
    }

    // Vertical separator
    canvas.drawFastVLine(midX - 2, 18, SCREEN_H - 36, CLR_DIVIDER);

    // Right: next task
    int rX = midX + 4;
    int rW = SCREEN_W - rX - 4;
    bool foundNext = false;
    for (int i = 0; i < taskCount; i++) {
        if (!tasks[i].completed) {
            canvas.setFont(&fonts::Font0);
            canvas.setTextColor(CLR_ACCENT);
            canvas.drawString("NEXT", rX, 20);
            canvas.setTextColor(CLR_TEXT);
            canvas.setFont(&fonts::Font2);
            char tt[36]; UI::truncPx(canvas, tt, tasks[i].title, rW);
            canvas.drawString(tt, rX, 34);
            if (tasks[i].projStart[0]) {
                char t24s[12], t24e[12];
                UI::formatApiTime(t24s, sizeof(t24s), tasks[i].projStart, appSettings.use24h);
                UI::formatApiTime(t24e, sizeof(t24e), tasks[i].projEnd, appSettings.use24h);
                canvas.setFont(&fonts::Font0);
                canvas.setTextColor(CLR_TEXT_DIM);
                char sched[28];
                snprintf(sched, sizeof(sched), "%s - %s", t24s, t24e);
                canvas.drawString(sched, rX, 52);
                canvas.setTextColor(CLR_TEXT);
            }
            if (tasks[i].timeEst[0]) {
                canvas.setFont(&fonts::Font0);
                canvas.setTextColor(CLR_ACCENT2);
                canvas.drawString(tasks[i].timeEst, rX, 66);
                canvas.setTextColor(CLR_TEXT);
            }
            foundNext = true;
            break;
        }
    }
    if (!foundNext) {
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(CLR_ACCENT);
        int aw = canvas.textWidth("All done!");
        canvas.drawString("All done!", rX + (rW - aw) / 2, 42);
        canvas.setTextColor(CLR_TEXT);
    }

    // Footer
    canvas.fillRect(0, SCREEN_H - 16, SCREEN_W, 16, CLR_HEADER_BG);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(CLR_TEXT_DIM);
    canvas.drawString("Press FRONT to wake", 4, SCREEN_H - 12);
    char refBuf[32];
    if (etaStr[0]) snprintf(refBuf, sizeof(refBuf), "%d%% %s", batt, etaStr);
    else           snprintf(refBuf, sizeof(refBuf), "%d%% (refresh %dm)",
                            batt, appSettings.refreshMinutes);
    int rbw = canvas.textWidth(refBuf);
    canvas.drawString(refBuf, SCREEN_W - rbw - 4, SCREEN_H - 12);
#endif

    }  // end standby-layout selection

    pushDisplay();

    // Wake every 60 s to update the displayed clock (partial refresh).
    // WiFi/MCP fetches only happen when refreshMinutes have elapsed since the
    // last successful fetch — see the timer-wake branch in setup().
    HAL::setupWakeSources(60);
    HAL::enterSleep();
}

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
    HAL::initDevice();

    // Load persistent settings from NVS
    Settings::load(appSettings);
    showingIntro = !appSettings.introSeen;

    esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
    // EXT0 = single-pin wake (StickC front btn), EXT1 = multi-pin wake (CoreInk dial)
    bool isButtonWake = (wakeReason == ESP_SLEEP_WAKEUP_EXT0)
                     || (wakeReason == ESP_SLEEP_WAKEUP_EXT1);
    bool isTimerWake  = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);

    Serial.printf("\n══ Sunsamagotchi v1 ══ wake=%d heap=%u\n",
                  (int)wakeReason, ESP.getFreeHeap());

    // Display init
    canvas.createSprite(SCREEN_W, SCREEN_H);
    canvas.setTextWrap(false);

    // ── Soft wake from deep sleep (button press) ──
    if (isButtonWake && restoreStateFromRTC()) {
        Serial.println("[Wake] Button wake — soft resume");
        wakeDebounce = true;
        wakeTime = millis();

        HAL::setFastMode();
        updateTimeFromHardwareRTC();  // instant — reads BM8563 hardware RTC
        needsRedraw = true;
        redrawScreen();

        // Start WiFi non-blocking — MCP + data fetch happens in loop()
        Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(50);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        pendingWiFiConnect = true;
        wifiConnectStart = millis();

        lastActivity = millis();
        lastRefresh = millis();
        return;
    }

    // ── Timer wake — update clock display; data refresh only when interval elapsed ──
    // Goal: keep the displayed clock current to within ~1 min while spending the
    // absolute minimum time awake.  Steps:
    //   1. Restore RTC-retained state.
    //   2. Read the BM8563 hardware RTC and refresh date/time strings.
    //   3. Re-render the sleep screen with a fast e-ink partial refresh.
    //   4. Only if refreshMinutes have elapsed: spin up WiFi + MCP and refetch.
    //   5. Sleep again — wake source already set inside enterDeepSleep().
    if (isTimerWake && restoreStateFromRTC()) {
        updateTimeFromHardwareRTC();

        time_t nowTs;
        time(&nowTs);
        uint32_t refreshSec = (uint32_t)effectiveRefreshMinutes() * 60UL;
        bool needsDataRefresh =
            (rtcLastDataFetch == 0) ||
            (nowTs > rtcLastDataFetch && (uint32_t)(nowTs - rtcLastDataFetch) >= refreshSec);

        Serial.printf("[Wake] Timer wake — clock=%s refresh_due=%d (last=%lld now=%lld)\n",
                      currentTime, (int)needsDataRefresh,
                      (long long)rtcLastDataFetch, (long long)nowTs);

        if (!needsDataRefresh) {
            // Fast path: just redraw the sleep screen with the new clock and
            // re-enter deep sleep.  No radio, no MCP, no NTP — ~300 ms awake.
            enterDeepSleep();
            return;
        }

        // Slow path: connect WiFi, refresh data, redraw, sleep again.
        Serial.println("[Wake] Data refresh due — connecting WiFi");
        if (connectWiFi()) {
            syncTime();
            syncHardwareRTC();
            updateTimeStrings();
            if (mcp.begin()) {
                mcpReady = true;
                bool ok = true;
                ok &= mcp.fetchTasks(todayStr, tasks, taskCount, MAX_TASKS);
                ok &= mcp.fetchEvents(todayStr, events, eventCount, MAX_EVENTS);
                ok &= mcp.fetchTimer(timerInfo);
                ok &= mcp.fetchPlanSummary(todayStr, planSummary);
                if (timerInfo.active) {
                    time(&rtcTimerFetchedAt);
                    rtcTimerElapsedAtFetch = timerInfo.elapsedSec;
                } else {
                    rtcTimerFetchedAt = 0;
                    rtcTimerElapsedAtFetch = 0;
                }
                dataLoaded = ok;
                if (ok) {
                    time(&rtcLastDataFetch);
                    saveStateToRTC();
                }
                if (!ok) Serial.println("[Data] Some fetches failed");
            }
        }
        enterDeepSleep();
        return;
    }

    // ── Cold boot — full init ──
    Serial.println("[Boot] Cold boot — full init");
    HAL::setQualityMode();

    // Boot screen
    canvas.fillSprite(CLR_BG);
#if IS_COLOR
    canvas.fillRect(0, 0, SCREEN_W, 22, CLR_HEADER_BG);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(CLR_HEADER_TEXT);
    int htw = canvas.textWidth("SUNSAMAGOTCHI");
    canvas.drawString("SUNSAMAGOTCHI", (SCREEN_W - htw) / 2, 3);
    UI::drawSunsamagotchiChar(canvas, SCREEN_W / 2, SCREEN_H / 2 - 5, 30);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(CLR_TEXT);
    int ctw = canvas.textWidth("Connecting...");
    canvas.drawString("Connecting...", (SCREEN_W - ctw) / 2, SCREEN_H - 22);
#else
    canvas.drawRect(4, 4, SCREEN_W - 8, SCREEN_H - 8, CLR_TEXT);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(CLR_TEXT);
    int htw = canvas.textWidth("Sunsamagotchi");
    canvas.drawString("Sunsamagotchi", (SCREEN_W - htw) / 2, SCREEN_H / 4 - 8);
    UI::drawSunsamagotchiChar(canvas, SCREEN_W / 2, SCREEN_H / 2 + 10, 30);
    canvas.setFont(&fonts::Font2);
    int ctw = canvas.textWidth("Connecting...");
    canvas.drawString("Connecting...", (SCREEN_W - ctw) / 2, SCREEN_H * 3 / 4 + 10);
#endif
    pushDisplay();

    if (!connectWiFi()) {
        canvas.fillRect(20, SCREEN_H * 2 / 3 - 4, SCREEN_W - 40, 40, CLR_BG);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextColor(CLR_TEXT);
        int fw = canvas.textWidth("WiFi FAILED");
        canvas.drawString("WiFi FAILED", (SCREEN_W - fw) / 2, SCREEN_H * 2 / 3);
        canvas.setFont(&fonts::Font2);
        int cfw = canvas.textWidth("Check config.h");
        canvas.drawString("Check config.h", (SCREEN_W - cfw) / 2, SCREEN_H * 2 / 3 + 20);
        pushDisplay();
        delay(5000);
        enterDeepSleep();
        return;
    }

    syncTime();
    syncHardwareRTC();

    HAL::setFastMode();

    canvas.fillSprite(CLR_BG);
    UI::drawHeader(canvas, "SUNSAMAGOTCHI");
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(CLR_TEXT);
    int mcpw = canvas.textWidth("Waking up...");
    canvas.drawString("Waking up...", (SCREEN_W - mcpw) / 2, SCREEN_H / 2 - 8);
    pushDisplay();

    if (!mcp.begin()) {
        canvas.fillSprite(CLR_BG);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextColor(CLR_TEXT);
        int mfw = canvas.textWidth("MCP Failed");
        canvas.drawString("MCP Failed", (SCREEN_W - mfw) / 2, SCREEN_H / 2 - 8);
        pushDisplay();
        delay(5000);
        enterDeepSleep();
        return;
    }
    mcpReady = true;

    fetchAllData();
    lastActivity = millis();
    lastRefresh = millis();
}

// ─── Loop ───────────────────────────────────────────────────────────────────
void loop() {
    handleButtons();

    uint32_t now = millis();

    // ── Deferred WiFi connect (non-blocking wake) ──
    // When WiFi associates, kick off mcp.begin() + the data fetch on Core 0 so
    // the UI on Core 1 stays fully responsive (no more 20-second freeze).
    if (pendingWiFiConnect) {
        if (WiFi.status() == WL_CONNECTED) {
            pendingWiFiConnect = false;
            Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            // mcp.begin() is a blocking TLS handshake; run it on the fetch task
            // by piggy-backing on startAsyncFetch (which checks mcpReady first).
            // We launch a tiny init task here that does begin() then triggers a fetch.
            xTaskCreatePinnedToCore(
                [](void*) {
                    if (!mcpReady) {
                        if (mcp.begin()) mcpReady = true;
                    }
                    if (mcpReady) {
                        // Inline the fetch body here so we don't race with
                        // startAsyncFetch() guards.
                        bool ok = true;
                        stageTaskCount = 0;
                        stageEventCount = 0;
                        memset(&stageTimer, 0, sizeof(stageTimer));
                        memset(&stagePlan,  0, sizeof(stagePlan));
                        ok &= mcp.fetchTasks(todayStr, stageTasks, stageTaskCount, MAX_TASKS);
                        ok &= mcp.fetchEvents(todayStr, stageEvents, stageEventCount, MAX_EVENTS);
                        ok &= mcp.fetchTimer(stageTimer);
                        ok &= mcp.fetchPlanSummary(todayStr, stagePlan);
                        asyncFetchOk      = ok;
                        asyncFetchDone    = true;
                    }
                    asyncFetchRunning = false;
                    asyncFetchHandle  = nullptr;
                    vTaskDelete(nullptr);
                },
                "mcpInit", 16384, nullptr, 1, &asyncFetchHandle, 0);
            asyncFetchRunning = true;
            fetchInProgress   = true;
            lastRefresh = millis();
        } else if (now - wifiConnectStart > 30000) {
            pendingWiFiConnect = false;
            Serial.println("[WiFi] Connection timeout");
        }
    }

    // ── Commit async fetch results into live state ──
    commitAsyncFetch();
    // commitAsyncFetch() may have just set lastRefresh = millis() (a value
    // newer than the `now` captured at the top of loop()).  Refresh `now` so
    // the auto-refresh check below doesn't see a uint32_t underflow.
    now = millis();

    // ── Time & timer display update ──
    static uint32_t lastTimeUpdate = 0;
#if IS_EINK
    // E-ink: minute-level updates only (avoid unnecessary slow refreshes)
    if (now - lastTimeUpdate > 60000) {
        if (currentScreen == SCREEN_DASHBOARD || currentScreen == SCREEN_TIMER
            || currentScreen == SCREEN_STATS) {
            needsRedraw = true;
        }
        lastTimeUpdate = now;
    }
#else
    // TFT: second-level timer updates when timer screen is active
    {
        uint32_t interval = (currentScreen == SCREEN_TIMER && timerInfo.active) ? 1000 : 60000;
        if (now - lastTimeUpdate > interval) {
            if (currentScreen == SCREEN_DASHBOARD || currentScreen == SCREEN_TIMER
                || currentScreen == SCREEN_STATS) {
                needsRedraw = true;
            }
            lastTimeUpdate = now;
        }
    }
#endif

    // Background data refresh (interval from settings) — no loading screen.
    // Uses the timer-aware effective interval so a running timer doesn't burn
    // battery on per-3-min API calls.
    uint32_t refreshInterval = (uint32_t)effectiveRefreshMinutes() * 60000UL;
    if (mcpReady && !fetchInProgress
        && now >= lastRefresh
        && (now - lastRefresh) >= refreshInterval) {
        Serial.println("[Auto] Background refresh");
        fetchAllData();
    }

    if (needsRedraw) redrawScreen();

    // ── StickC: display off after inactivity (backlight off, stays awake) ──
#if !IS_EINK
    if (!displayOff && (now - lastActivity > DISPLAY_OFF_MS)) {
        HAL::backlightOff();
        displayOff = true;
        Serial.println("[Display] Backlight off");
    }
#endif

    // Auto sleep (timeout from settings).  Don't sleep if a fetch is in flight —
    // it would kill the TLS connection and leave us with stale data.  Extend
    // the activity window slightly to wait for it to complete.
    if (now - lastActivity > appSettings.activeTimeoutMs) {
        if (fetchInProgress) {
            // Keep waking the user-activity logic briefly while the fetch runs.
            lastActivity = now - appSettings.activeTimeoutMs + 2000;
        } else {
            enterDeepSleep();
        }
    }

    delay(50);
}
