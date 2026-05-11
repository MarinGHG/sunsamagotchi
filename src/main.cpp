// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2024 Marin Benke
// Sunsamagotchi — a Sunsama companion for M5Stack devices
#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>
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
    configTime(TZ_OFFSET_SEC, 0, NTP_SERVER);
    struct tm timeinfo;
    int tries = 0;
    while (!getLocalTime(&timeinfo) && tries < 10) {
        delay(500);
        tries++;
    }
    Serial.println(tries < 10 ? "[NTP] Time synced" : "[NTP] Sync failed");
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
    M5.Rtc.setDateTime(dt);
    Serial.println("[RTC] Hardware RTC synced from NTP");
}

// Read time directly from hardware RTC (BM8563) — instant, no WiFi needed.
// M5Unified syncs the BM8563 from gmtime() (UTC) after NTP.
// We adjust for TZ_OFFSET_SEC and also set the system clock so time() works.
void updateTimeFromHardwareRTC() {
    auto dt = M5.Rtc.getDateTime();
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

// ─── Data Fetch — silent, no UI overlay ─────────────────────────────────────
void fetchAllData() {
    Serial.println("[Data] Fetching all data...");
    fetchInProgress = true;

    updateTimeStrings();

    bool ok = true;
    ok &= mcp.fetchTasks(todayStr, tasks, taskCount, MAX_TASKS);
    ok &= mcp.fetchEvents(todayStr, events, eventCount, MAX_EVENTS);
    ok &= mcp.fetchTimer(timerInfo);
    ok &= mcp.fetchPlanSummary(todayStr, planSummary);

    // Anchor timer to now so elapsed tracks wall-clock through sleep cycles
    if (timerInfo.active) {
        time(&rtcTimerFetchedAt);
        rtcTimerElapsedAtFetch = timerInfo.elapsedSec;
    } else {
        rtcTimerFetchedAt = 0;
        rtcTimerElapsedAtFetch = 0;
    }

    dataLoaded = ok;
    if (!ok) Serial.println("[Data] Some fetches failed");

    saveStateToRTC();
    needsRedraw = true;
    lastRefresh = millis();
    fetchInProgress = false;
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

    // Don't process buttons during fetch
    if (fetchInProgress) return;

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
    if (HAL::forceRefreshHeld() && !activity && (millis() - lastPagePressMs > 1000)) {
        Serial.println("[Action] Force refresh");
        if (mcpReady) fetchAllData();
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
    if (HAL::confirmPressed()) {
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
void enterDeepSleep() {
    Serial.println("[Sleep] Entering deep sleep...");
    // Sync timer elapsed before saving so sleep screen shows correct value
    if (timerInfo.active) timerInfo.elapsedSec = timerCurrentElapsed();
    saveStateToRTC();

    HAL::setQualityMode();
    canvas.fillSprite(CLR_BG);
    updateTimeStrings();

#if IS_EINK
    // ── CoreInk 200x200: always-on sleep display ──────────────────────────────
    // All text in full CLR_TEXT (no dimming) so e-ink renders crisp black.
    canvas.setTextColor(CLR_TEXT);

    // Large clock
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    int tw = canvas.textWidth(currentTime);
    canvas.drawString(currentTime, (SCREEN_W - tw) / 2, 22);

    // Date
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    int dw = canvas.textWidth(currentDate);
    canvas.drawString(currentDate, (SCREEN_W - dw) / 2, 56);
    UI::drawDottedLine(canvas, 74);

    // Next task
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(CLR_TEXT);
    bool foundNext = false;
    for (int i = 0; i < taskCount; i++) {
        if (!tasks[i].completed) {
            canvas.drawString("Next:", 6, 80);
            char tt[30]; UI::truncPx(canvas, tt, tasks[i].title, SCREEN_W - 16);
            canvas.drawString(tt, 6, 96);
            if (tasks[i].projStart[0]) {
                char t24s[12], t24e[12];
                UI::formatApiTime(t24s, sizeof(t24s), tasks[i].projStart, appSettings.use24h);
                UI::formatApiTime(t24e, sizeof(t24e), tasks[i].projEnd, appSettings.use24h);
                char sched[28];
                snprintf(sched, sizeof(sched), "%s - %s", t24s, t24e);
                canvas.drawString(sched, 6, 112);
            }
            if (tasks[i].timeEst[0]) {
                canvas.setFont(&fonts::Font0);
                canvas.drawString(tasks[i].timeEst, 6, 126);
                canvas.setFont(&fonts::Font2);
            }
            foundNext = true;
            break;
        }
    }
    if (!foundNext) {
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        int aw = canvas.textWidth("All tasks done!");
        canvas.drawString("All tasks done!", (SCREEN_W - aw) / 2, 96);
    }

    // Active timer
    if (timerInfo.active) {
        UI::drawDottedLine(canvas, 142);
        canvas.setFont(&fonts::Font2);
        UI::drawIcon8(canvas, 6, 150, UI::ICON_TIMER);
        char ts[40]; UI::truncPx(canvas, ts, timerInfo.taskTitle, SCREEN_W - 24);
        canvas.drawString(ts, 18, 150);
    }

    // Sunsamagotchi sleeping footer
    UI::drawDottedLine(canvas, 168);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(CLR_TEXT);
    int smw = canvas.textWidth("Sunsamagotchi is sleeping  zZZ");
    canvas.drawString("Sunsamagotchi is sleeping  zZZ", (SCREEN_W - smw) / 2, 172);
    int pmw = canvas.textWidth("Press button to wake");
    canvas.drawString("Press button to wake", (SCREEN_W - pmw) / 2, 183);

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
    char refBuf[24];
    snprintf(refBuf, sizeof(refBuf), "refresh: %dmin", appSettings.refreshMinutes);
    int rbw = canvas.textWidth(refBuf);
    canvas.drawString(refBuf, SCREEN_W - rbw - 4, SCREEN_H - 12);
#endif

    pushDisplay();

    HAL::setupWakeSources(appSettings.sleepMinutes);
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
    if (isTimerWake && restoreStateFromRTC()) {
        Serial.println("[Wake] Timer wake — clock update");

        HAL::setQualityMode();

        // Read hardware RTC immediately — sets currentTime, currentDate, todayStr,
        // and also restores system time so time() comparisons work.
        updateTimeFromHardwareRTC();

        // Only do WiFi + data fetch if refresh interval has elapsed
        time_t nowTs;
        time(&nowTs);
        uint32_t refreshSec = (uint32_t)appSettings.refreshMinutes * 60UL;
        bool needsDataRefresh = (nowTs - rtcLastDataFetch >= (time_t)refreshSec)
                             || rtcLastDataFetch == 0;

        if (needsDataRefresh) {
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
                    time(&rtcLastDataFetch);
                    if (!ok) Serial.println("[Data] Some fetches failed");
                }
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
    if (pendingWiFiConnect) {
        if (WiFi.status() == WL_CONNECTED) {
            pendingWiFiConnect = false;
            Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            // No NTP on button wake — BM8563 hardware RTC is accurate enough
            if (mcp.begin()) {
                mcpReady = true;
                fetchAllData();
            }
            lastRefresh = millis();
        } else if (now - wifiConnectStart > 30000) {
            pendingWiFiConnect = false;
            Serial.println("[WiFi] Connection timeout");
        }
    }

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

    // Background data refresh (interval from settings) — no loading screen
    uint32_t refreshInterval = (uint32_t)appSettings.refreshMinutes * 60000UL;
    if (mcpReady && now - lastRefresh > refreshInterval && !fetchInProgress) {
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

    // Auto sleep (timeout from settings)
    if (now - lastActivity > appSettings.activeTimeoutMs) {
        enterDeepSleep();
    }

    delay(50);
}
