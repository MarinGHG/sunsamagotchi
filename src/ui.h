// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Marin Benke
#pragma once

#include <M5Unified.h>
#include <WiFi.h>
#include "config.h"
#include "hal.h"
#include "net_cfg.h"
#include "settings.h"

// PlatformIO injects the real value via scripts/version.py (git tag/describe,
// or the exact release tag in CI). This is only a fallback for IDEs/build
// setups that skip extra_scripts.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev-local"
#endif

// ─── Screen IDs ─────────────────────────────────────────────────────────────
enum Screen : uint8_t {
    SCREEN_DASHBOARD = 0,
    SCREEN_TASKS,
    SCREEN_EVENTS,
    SCREEN_TIMER,
    SCREEN_STATS,
    SCREEN_SETTINGS,
    SCREEN_COUNT
};

// ─── Confirmation dialog state ──────────────────────────────────────────────
enum ConfirmState : uint8_t {
    CONFIRM_NONE = 0,
    CONFIRM_COMPLETE_TASK,
    CONFIRM_UNCOMPLETE_TASK,
    CONFIRM_OTA_UPDATE,
};

// Options cycled with UP/DOWN inside the OTA update prompt.
enum OtaPromptOption : uint8_t {
    OTA_OPT_INSTALL = 0,
    OTA_OPT_REMIND,
    OTA_OPT_SKIP,
    OTA_OPT_COUNT,
};

// ─── Task / Event data ──────────────────────────────────────────────────────
struct TaskItem {
    char title[48];
    char channel[20];
    char timeEst[16];
    char projStart[12];
    char projEnd[12];
    bool completed;
    bool overcommitted;
    char id[32];
};

struct EventItem {
    char title[48];
    char startTime[12];
    uint16_t durationMin;
    bool isAllDay;
};

struct TimerInfo {
    bool active;
    char taskTitle[48];
    char taskId[32];
    char startedAt[24];
    uint32_t elapsedSec;
};

struct PlanSummary {
    char totalRemaining[24];
    char totalWork[24];
    char totalPersonal[24];
    char shutdown[8];
    char overcommitted[24];
};

// ─── Drawing helpers ────────────────────────────────────────────────────────
namespace UI {

// ── Layout constants ────────────────────────────────────────────────────────
// Adaptive sizing based on screen dimensions
static const int HDR_H    = IS_EINK ? 20 : 18;          // Header bar height
static const int NAV_H    = 10;                           // Nav dots area at very bottom
static const int HINT_H   = IS_EINK ? 12 : 10;           // Footer hint text area
static const int FOOT_H   = NAV_H + HINT_H;              // Total footer area
static const int PAD      = IS_EINK ? 4 : 3;            // Edge padding
static const int ROW_H    = IS_EINK ? 18 : 16;          // List row height
static const int BODY_TOP = HDR_H + 2;                   // Content start Y
static const int BODY_BOT = SCREEN_H - FOOT_H;           // Content end Y
static const int MAX_VIS  = (BODY_BOT - BODY_TOP) / ROW_H; // Max visible rows
// Max chars that fit in one row with Font2 (~7px/char on e-ink, ~6px on TFT)
static const int CHAR_W   = IS_EINK ? 7 : 7;
static const int MAX_TITLE_CHARS = (SCREEN_W - 40) / CHAR_W;

// ── Pixel-art style icons (8x8) ────────────────────────────────────────────
static const uint8_t ICON_TASK[] = {
    0b01111110, 0b10000001, 0b10100101, 0b10000001,
    0b10100101, 0b10011001, 0b10000001, 0b01111110,
};
static const uint8_t ICON_CLOCK[] = {
    0b00111100, 0b01000010, 0b10011001, 0b10010001,
    0b10010001, 0b10000001, 0b01000010, 0b00111100,
};
static const uint8_t ICON_TIMER[] = {
    0b01111110, 0b00100100, 0b00011000, 0b00111100,
    0b01100110, 0b01000010, 0b01100110, 0b00111100,
};
static const uint8_t ICON_CAL[] = {
    0b01111110, 0b01000010, 0b01111110, 0b01000010,
    0b01010010, 0b01000010, 0b01000010, 0b01111110,
};
static const uint8_t ICON_CHECK[] = {
    0b00000000, 0b00000010, 0b00000100, 0b10001000,
    0b01010000, 0b00100000, 0b00000000, 0b00000000,
};
static const uint8_t ICON_BULLET[] = {
    0b00000000, 0b00000000, 0b00111100, 0b01111110,
    0b01111110, 0b00111100, 0b00000000, 0b00000000,
};
static const uint8_t ICON_GEAR[] = {
    0b00011000, 0b01111110, 0b01100110, 0b11100111,
    0b11100111, 0b01100110, 0b01111110, 0b00011000,
};
static const uint8_t ICON_CHART[] = {
    0b00000010, 0b00000010, 0b00001010, 0b00001010,
    0b00101010, 0b00101010, 0b10101010, 0b11111110,
};

inline void drawIcon8(M5Canvas& c, int x, int y, const uint8_t* icon, uint16_t color = CLR_ICON) {
    for (int row = 0; row < 8; row++) {
        uint8_t bits = icon[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                c.drawPixel(x + col, y + row, color);
            }
        }
    }
}

// ── Parse "H:MM AM/PM" from API to minutes-of-day (-1 if unparsable) ────────
inline int apiTimeToMinutes(const char* apiTime) {
    if (!apiTime || !apiTime[0]) return -1;
    int h = 0, m = 0;
    char ampm[4] = "";
    int n = sscanf(apiTime, "%d:%d %2s", &h, &m, ampm);
    if (n < 2) return -1;
    if (ampm[0] == 'P' && h != 12) h += 12;
    if (ampm[0] == 'A' && h == 12) h = 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

// True if a calendar event has already ended given the current local time.
// Sunsama auto-completes events whose end is in the past; we mirror that
// purely client-side because the events resource doesn't expose a completed
// flag (only title/startTime/duration/isAllDay).
inline bool eventIsPast(const EventItem& e) {
    if (e.isAllDay) return false;
    struct tm now_tm;
    if (!getLocalTime(&now_tm, 0)) return false;
    int nowMins = now_tm.tm_hour * 60 + now_tm.tm_min;
    int evStart = apiTimeToMinutes(e.startTime);
    if (evStart < 0) return false;
    int evEnd = evStart + (int)e.durationMin;
    return evEnd <= nowMins;
}

// ── Convert "H:MM AM/PM" from API to 24h based on runtime setting ───────────
inline void formatApiTime(char* out, size_t outSz, const char* apiTime, bool use24h = true) {
    if (!apiTime || !apiTime[0]) { out[0] = '\0'; return; }
    if (use24h) {
        int h = 0, m = 0;
        char ampm[4] = "";
        if (sscanf(apiTime, "%d:%d %2s", &h, &m, ampm) >= 2) {
            if (ampm[0] == 'P' && h != 12) h += 12;
            if (ampm[0] == 'A' && h == 12) h = 0;
            snprintf(out, outSz, "%d:%02d", h, m);
        } else {
            strlcpy(out, apiTime, outSz);
        }
    } else {
        strlcpy(out, apiTime, outSz);
    }
}

// ── Decorative elements ─────────────────────────────────────────────────────

inline void drawHeader(M5Canvas& c, const char* title, const uint8_t* icon = nullptr,
                        bool sunsamaFailed = false, bool otaAvailable = false) {
    c.fillRect(0, 0, SCREEN_W, HDR_H, CLR_HEADER_BG);
    c.setTextColor(CLR_HEADER_TEXT);
    c.setFont(&fonts::Font2);
    int textY = (HDR_H - 16) / 2;  // vertically center Font2 (~16px tall)
    if (icon) {
        drawIcon8(c, 5, (HDR_H - 8) / 2, icon, CLR_HEADER_TEXT);
        c.drawString(title, 16, textY);
    } else {
        c.drawString(title, 5, textY);
    }
    // WiFi is up but the last Sunsama API call failed — distinct from a WiFi outage.
    if (sunsamaFailed) {
        int bx = SCREEN_W - 44, by = (HDR_H - 10) / 2;
        c.fillRect(bx, by, 16, 10, CLR_HEADER_BG);
        c.drawRect(bx, by, 16, 10, CLR_HEADER_TEXT);
        c.drawString("!", bx + 5, by - 1);
    }
    // A newer firmware build was found on the selected OTA channel and is
    // waiting for the user to Install/Remind/Skip — see CONFIRM_OTA_UPDATE.
    if (otaAvailable) {
        int bx = SCREEN_W - 64, by = (HDR_H - 10) / 2;
        c.fillRect(bx, by, 16, 10, CLR_HEADER_BG);
        c.drawRect(bx, by, 16, 10, CLR_HEADER_TEXT);
        c.drawString("^", bx + 5, by - 1);
    }
}

inline void drawDottedLine(M5Canvas& c, int y, int x0 = -1, int x1 = -1) {
    if (x0 < 0) x0 = PAD;
    if (x1 < 0) x1 = SCREEN_W - PAD;
    for (int x = x0; x < x1; x += 3) c.drawPixel(x, y, CLR_DIVIDER);
}

inline void drawNavDots(M5Canvas& c, uint8_t cur) {
    int dotY = SCREEN_H - NAV_H / 2;  // Center dots in bottom nav area
    int totalW = SCREEN_COUNT * 10;
    int sx = (SCREEN_W - totalW) / 2;
    for (uint8_t i = 0; i < SCREEN_COUNT; i++) {
        int cx = sx + i * 10 + 3;
        if (i == cur) c.fillCircle(cx, dotY, 2, CLR_TEXT);
        else          c.drawCircle(cx, dotY, 2, CLR_TEXT_DIM);
    }
}

inline void drawBattery(M5Canvas& c, int pct) {
    int x = SCREEN_W - 26, y = (HDR_H - 10) / 2;
    c.drawRect(x, y, 18, 10, CLR_HEADER_TEXT);
    c.fillRect(x + 18, y + 3, 2, 4, CLR_HEADER_TEXT);
    int fw = (int)(14.0f * pct / 100.0f);
    if (fw > 0) c.fillRect(x + 2, y + 2, fw, 6, CLR_HEADER_TEXT);
}

// ── Truncation helper ───────────────────────────────────────────────────────
inline void trunc(char* dst, const char* src, int maxChars) {
    int len = strlen(src);
    if (len <= maxChars) {
        strcpy(dst, src);
    } else {
        strncpy(dst, src, maxChars - 2);
        dst[maxChars - 2] = '\0';
        strcat(dst, "..");
    }
}

// ── Pixel-width-based truncation ────────────────────────────────────────────
// The implementation takes the destination size explicitly; the templated
// wrapper below lets callers pass a stack array directly and infer the size
// at compile time, so we get bounds-checked writes even when the source
// string fits the pixel budget but not the destination buffer.  (This was a
// real bug — long task titles fit horizontally on screen but overflowed the
// 24-/30-byte tt[] buffers, tripping the stack canary and rebooting.)
inline void truncPxImpl(M5Canvas& c, char* dst, size_t dstSz,
                         const char* src, int maxPx) {
    if (!dst || dstSz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    if (c.textWidth(src) <= maxPx) {
        strlcpy(dst, src, dstSz);
        return;
    }
    char tmp[64];
    int len = (int)strlen(src);
    if (len > (int)sizeof(tmp) - 3) len = (int)sizeof(tmp) - 3;
    for (int n = len; n > 2; n--) {
        strncpy(tmp, src, n);
        tmp[n] = '\0';
        strcat(tmp, "..");
        if (c.textWidth(tmp) <= maxPx) {
            strlcpy(dst, tmp, dstSz);
            return;
        }
    }
    strlcpy(dst, "..", dstSz);
}

template<size_t N>
inline void truncPx(M5Canvas& c, char (&dst)[N], const char* src, int maxPx) {
    truncPxImpl(c, dst, N, src, maxPx);
}

// ── Format elapsed timer as minutes only ────────────────────────────────────
inline void formatElapsedMin(char* out, size_t sz, uint32_t elapsedSec) {
    uint32_t totalMin = elapsedSec / 60;
    if (totalMin >= 60) {
        snprintf(out, sz, "%luh %lum", (unsigned long)(totalMin / 60), (unsigned long)(totalMin % 60));
    } else {
        snprintf(out, sz, "%lum", (unsigned long)totalMin);
    }
}

// ── Format elapsed timer with seconds (TFT second-level display) ────────────
inline void formatElapsedSec(char* out, size_t sz, uint32_t elapsedSec) {
    uint32_t h = elapsedSec / 3600;
    uint32_t m = (elapsedSec % 3600) / 60;
    uint32_t s = elapsedSec % 60;
    if (h > 0) snprintf(out, sz, "%luh%02lum%02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else snprintf(out, sz, "%lum%02lus", (unsigned long)m, (unsigned long)s);
}

// ── Footer hint — uniform for both boards ───────────────────────────────────
inline void drawFooterHint(M5Canvas& c, const char* hint) {
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    int hw = c.textWidth(hint);
    c.drawString(hint, (SCREEN_W - hw) / 2, SCREEN_H - FOOT_H + 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Sunsamagotchi character drawing helper ───────────────────────────────────
// Draws a sun+cloud tamagotchi-style character centred at (cx, cy).
// sunR = radius of the sun circle.
// ═══════════════════════════════════════════════════════════════════════════

inline void drawSunsamagotchiChar(M5Canvas& c, int cx, int cy, int sunR) {
    // ── Sun body ──────────────────────────────────────────────────────────────
#if IS_COLOR
    c.fillCircle(cx, cy, sunR, CLR_SUN);
    c.drawCircle(cx, cy, sunR,     0xFB00);
    c.drawCircle(cx, cy, sunR - 1, 0xFB00);
#else
    c.fillCircle(cx, cy, sunR, CLR_SUN);   // CLR_SUN = mid-gray on e-ink
    c.drawCircle(cx, cy, sunR, TFT_BLACK);
#endif

    // ── Rays (8 directions) ───────────────────────────────────────────────────
    for (int i = 0; i < 8; i++) {
        float rad = i * 45.0f * 0.017453f;
        int x1 = cx + (int)((sunR + 3) * sinf(rad));
        int y1 = cy - (int)((sunR + 3) * cosf(rad));
        int x2 = cx + (int)((sunR + 7) * sinf(rad));
        int y2 = cy - (int)((sunR + 7) * cosf(rad));
#if IS_COLOR
        c.drawLine(x1, y1, x2, y2, 0xFEA0);
        c.drawLine(x1, y1 + 1, x2, y2 + 1, 0xFEA0);
#else
        c.drawLine(x1, y1, x2, y2, TFT_BLACK);
#endif
    }

    // ── Sunglasses ────────────────────────────────────────────────────────────
    int eyeX = sunR * 3 / 8;    // horizontal offset per lens from center
    int eyeY = sunR / 5;         // vertical offset above center
    int lW   = sunR / 4;         // half-width of each lens
    int lH2  = (sunR / 9 < 2) ? 2 : sunR / 9;  // half-height of lens
    // Left lens
    c.fillRoundRect(cx - eyeX - lW, cy - eyeY - lH2, lW * 2, lH2 * 2 + 1, 2, TFT_BLACK);
    // Right lens
    c.fillRoundRect(cx + eyeX - lW, cy - eyeY - lH2, lW * 2, lH2 * 2 + 1, 2, TFT_BLACK);
    // Bridge between inner edges
    {
        int bx1 = cx - eyeX + lW;
        int bx2 = cx + eyeX - lW;
        if (bx2 > bx1) c.fillRect(bx1, cy - eyeY - 1, bx2 - bx1, 2, TFT_BLACK);
    }
    // Arms from outer edges toward sun rim
    c.fillRect(cx - eyeX - lW - sunR / 4, cy - eyeY - 1, sunR / 4, 2, TFT_BLACK);
    c.fillRect(cx + eyeX + lW,            cy - eyeY - 1, sunR / 4, 2, TFT_BLACK);
    // Lens shine
    c.drawPixel(cx - eyeX - lW / 3, cy - eyeY - lH2 + 1, TFT_WHITE);
    c.drawPixel(cx + eyeX - lW / 3, cy - eyeY - lH2 + 1, TFT_WHITE);

    // ── Smile ─────────────────────────────────────────────────────────────────
    int smileY = cy + sunR / 5 + 1;
    int sw     = sunR / 3;
    c.drawLine(cx - sw,     smileY,     cx - sw / 2, smileY + 3, TFT_BLACK);
    c.drawLine(cx - sw / 2, smileY + 3, cx,          smileY + 5, TFT_BLACK);
    c.drawLine(cx,          smileY + 5, cx + sw / 2, smileY + 3, TFT_BLACK);
    c.drawLine(cx + sw / 2, smileY + 3, cx + sw,     smileY,     TFT_BLACK);

    // ── Blush marks (TFT only) ────────────────────────────────────────────────
#if IS_COLOR
    c.fillCircle(cx - sunR * 3 / 5, cy + 2, 4, 0xFC11);   // peachy-orange
    c.fillCircle(cx + sunR * 3 / 5, cy + 2, 4, 0xFC11);
#endif

    // ── Cloud — drawn over sun bottom-right so it overlaps like the logo ──────
    int cdx = cx + sunR * 11 / 16;
    int cdy = cy + sunR * 11 / 16;
    uint16_t cldClr = CLR_CLOUD;
    c.fillCircle(cdx,      cdy,      9, cldClr);
    c.fillCircle(cdx + 10, cdy + 4,  8, cldClr);
    c.fillCircle(cdx - 8,  cdy + 4,  7, cldClr);
    c.fillRect(cdx - 14, cdy + 5, 34, 8, cldClr);
#if !IS_COLOR
    // E-ink: outline the cloud so it's visible against the gray sun
    c.drawCircle(cdx,      cdy,      9, TFT_BLACK);
    c.drawCircle(cdx + 10, cdy + 4,  8, TFT_BLACK);
    c.drawCircle(cdx - 8,  cdy + 4,  7, TFT_BLACK);
    c.drawRect(cdx - 14, cdy + 5, 34, 8, TFT_BLACK);
#endif
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Intro / Welcome screen ──────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawIntroScreen(M5Canvas& c) {
    c.fillSprite(CLR_BG);

#if IS_COLOR
    // Warm orange header
    c.fillRect(0, 0, SCREEN_W, HDR_H, CLR_HEADER_BG);
    c.setFont(&fonts::Font2);
    c.setTextColor(CLR_HEADER_TEXT);
    int htw = c.textWidth("SUNSAMAGOTCHI");
    c.drawString("SUNSAMAGOTCHI", (SCREEN_W - htw) / 2, (HDR_H - 16) / 2);
#else
    drawHeader(c, "SUNSAMAGOTCHI");
#endif

#if IS_EINK
    // CoreInk 200x200: character centred, description + hints below.
    // Everything below the dotted line must clear the footer band
    // (SCREEN_H - FOOT_H = 178) so the "Press SELECT" footer hint never
    // overlaps the control list — that overlap was the "jammed" intro bug.
    drawSunsamagotchiChar(c, 100, 68, 28);

    c.setTextColor(CLR_TEXT);
    c.setFont(&fonts::FreeSansBold9pt7b);
    int nw = c.textWidth("Sunsamagotchi");
    c.drawString("Sunsamagotchi", (SCREEN_W - nw) / 2, 104);
    c.setFont(&fonts::Font2);
    int sw = c.textWidth("Your pocket Sunsama friend!");
    c.drawString("Your pocket Sunsama friend!", (SCREEN_W - sw) / 2, 122);

    drawDottedLine(c, 138);

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT);
    c.drawString("Dial UP/DOWN  scroll lists", 8, 144);
    c.drawString("Dial PRESS    select / confirm", 8, 155);
    c.drawString("Top button    next page", 8, 166);

#else
    // StickC 240x135: character left, text right
    drawSunsamagotchiChar(c, 52, HDR_H + 50, 28);

    int rX = 110;
    c.setTextColor(CLR_TEXT);
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.drawString("Sunsamagotchi", rX, HDR_H + 4);

    c.setFont(&fonts::Font2);
    c.setTextColor(CLR_TEXT_DIM);
    c.drawString("Your pocket Sunsama", rX, HDR_H + 24);
    c.drawString("companion!", rX, HDR_H + 40);

    drawDottedLine(c, HDR_H + 57, rX, SCREEN_W - PAD);

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT);
    c.drawString("LEFT  : next page", rX, HDR_H + 61);
    c.drawString("RIGHT : scroll down", rX, HDR_H + 71);
    c.drawString("FRONT : select / confirm", rX, HDR_H + 81);
    c.drawString("HOLD  : force refresh", rX, HDR_H + 91);
#endif

    drawFooterHint(c, "Press SELECT to continue");
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Dashboard screen ────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawDashboard(M5Canvas& c, const char* timeStr, int batt,
                          TaskItem* tasks, uint8_t taskCount,
                          EventItem* events, uint8_t eventCount,
                          PlanSummary& plan, TimerInfo& timer,
                          const char* dateStr, bool use24h,
                          bool sunsamaFailed = false, bool otaAvailable = false)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "SUNSAMAGOTCHI", nullptr, sunsamaFailed, otaAvailable);
    drawBattery(c, batt);

#if IS_EINK
    // ── CoreInk 200x200: Vertical layout — clock, plan, timer, tasks ──
    c.setTextColor(CLR_TEXT);
    c.setFont(&fonts::FreeSansBold18pt7b);
    int tw = c.textWidth(timeStr);
    c.drawString(timeStr, (SCREEN_W - tw) / 2, 24);

    c.setFont(&fonts::FreeSansBold9pt7b);
    int dw = c.textWidth(dateStr);
    c.drawString(dateStr, (SCREEN_W - dw) / 2, 56);
    drawDottedLine(c, 72);

    // Plan summary row
    c.setFont(&fonts::Font2);
    c.setTextColor(CLR_TEXT);
    c.drawString("Left:", PAD, 76);
    c.drawString(plan.totalRemaining, 40, 76);
    c.drawString("End:", 110, 76);
    c.drawString(plan.shutdown, 140, 76);

    if (plan.overcommitted[0] != '0' && plan.overcommitted[0] != '\0') {
        c.setTextColor(CLR_WARN);
        c.drawString("Over:", PAD, 92);
        c.drawString(plan.overcommitted, 40, 92);
        c.setTextColor(CLR_TEXT);
    }
    drawDottedLine(c, 108);

    // Active timer
    int yOff = 112;
    if (timer.active) {
        drawIcon8(c, PAD, yOff + 1, ICON_TIMER);
        c.setFont(&fonts::FreeSansBold9pt7b);
        char el[12];
        formatElapsedMin(el, sizeof(el), timer.elapsedSec);
        c.drawString(el, 16, yOff - 2);
        c.setFont(&fonts::Font2);
        char tt[30]; trunc(tt, timer.taskTitle, 26);
        c.drawString(tt, 16, yOff + 14);
        yOff += 30;
        drawDottedLine(c, yOff);
        yOff += 4;
    }

    // Next tasks preview
    c.setFont(&fonts::Font2);
    int maxP = timer.active ? 2 : 3;
    int shown = 0;
    for (int i = 0; i < taskCount && shown < maxP; i++) {
        if (tasks[i].completed) continue;
        c.setTextColor(CLR_TEXT);
        if (tasks[i].overcommitted) {
            drawIcon8(c, PAD, yOff + 2, ICON_BULLET, CLR_WARN);
        } else {
            drawIcon8(c, PAD, yOff + 2, ICON_BULLET, CLR_ICON);
        }

        int titleMaxPx = SCREEN_W - 20;
        if (tasks[i].projStart[0]) {
            char t24[12];
            formatApiTime(t24, sizeof(t24), tasks[i].projStart, use24h);
            int timeW = c.textWidth(t24) + 8;
            titleMaxPx = SCREEN_W - 18 - timeW;
            char tt[30]; truncPx(c, tt, tasks[i].title, titleMaxPx);
            c.drawString(tt, 16, yOff);
            int sw = c.textWidth(t24);
            c.drawString(t24, SCREEN_W - sw - PAD, yOff);
        } else {
            char tt[30]; truncPx(c, tt, tasks[i].title, titleMaxPx);
            c.drawString(tt, 16, yOff);
        }
        yOff += 16;
        shown++;
    }

#else
    // ── StickC 240x135: Horizontal two-column layout ──
    c.setTextColor(CLR_TEXT);

    // LEFT COLUMN: Clock + date + plan
    int midX = SCREEN_W / 2 - 2;

    c.setFont(&fonts::FreeSansBold18pt7b);
    int tw = c.textWidth(timeStr);
    c.drawString(timeStr, (midX - tw) / 2, BODY_TOP);

    c.setFont(&fonts::Font2);
    int dw = c.textWidth(dateStr);
    c.drawString(dateStr, (midX - dw) / 2, BODY_TOP + 32);

    // Plan summary below date
    int planY = BODY_TOP + 50;
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT);
    char planLine[40];
    snprintf(planLine, sizeof(planLine), "Left: %s", plan.totalRemaining);
    c.drawString(planLine, PAD, planY);
    snprintf(planLine, sizeof(planLine), "End:  %s", plan.shutdown);
    c.drawString(planLine, PAD, planY + 12);
    if (plan.overcommitted[0] != '0' && plan.overcommitted[0] != '\0') {
        c.setTextColor(CLR_WARN);
        snprintf(planLine, sizeof(planLine), "Over: %s", plan.overcommitted);
        c.drawString(planLine, PAD, planY + 24);
        c.setTextColor(CLR_TEXT);
    }

    // Vertical separator
    c.drawFastVLine(midX, BODY_TOP, BODY_BOT - BODY_TOP - 2, CLR_DIVIDER);

    // RIGHT COLUMN: Timer + next tasks
    int rX = midX + 6;
    int rW = SCREEN_W - rX - PAD;
    int ryOff = BODY_TOP;

    if (timer.active) {
        drawIcon8(c, rX, ryOff + 1, ICON_TIMER);
        c.setFont(&fonts::FreeSansBold9pt7b);
        char el[12];
        formatElapsedMin(el, sizeof(el), timer.elapsedSec);
        c.drawString(el, rX + 12, ryOff - 1);
        c.setFont(&fonts::Font0);
        char tt[24]; truncPx(c, tt, timer.taskTitle, rW - 4);
        c.drawString(tt, rX, ryOff + 14);
        ryOff += 26;
        drawDottedLine(c, ryOff, rX, SCREEN_W - PAD);
        ryOff += 3;
    }

    // Next tasks
    c.setFont(&fonts::Font2);
    int maxP = timer.active ? 3 : 5;
    int shown = 0;
    for (int i = 0; i < taskCount && shown < maxP && ryOff < BODY_BOT - ROW_H; i++) {
        if (tasks[i].completed) continue;
        c.setTextColor(CLR_TEXT);
        uint16_t bulletClr = tasks[i].overcommitted ? CLR_WARN : CLR_ICON;
        drawIcon8(c, rX, ryOff + 3, ICON_BULLET, bulletClr);
        char tt[30]; truncPx(c, tt, tasks[i].title, rW - 14);
        c.drawString(tt, rX + 12, ryOff);
        ryOff += ROW_H;
        shown++;
    }
#endif

    drawNavDots(c, SCREEN_DASHBOARD);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Tasks list screen ───────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawTasksScreen(M5Canvas& c, TaskItem* tasks, uint8_t taskCount,
                             uint8_t scrollOff, uint8_t selIdx)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "TASKS", ICON_TASK);

    uint8_t doneCount = 0;
    for (int i = 0; i < taskCount; i++) if (tasks[i].completed) doneCount++;

    // Count in header
    char cs[12]; snprintf(cs, sizeof(cs), "%d/%d", doneCount, taskCount);
    c.setFont(&fonts::Font2);
    c.setTextColor(CLR_HEADER_TEXT);
    int cw = c.textWidth(cs);
    c.drawString(cs, SCREEN_W - cw - 30, (HDR_H - 16) / 2);

    // Progress bar
    int barY = HDR_H + 1;
    c.drawRect(PAD, barY, SCREEN_W - PAD * 2, 4, CLR_TEXT);
    if (taskCount > 0) {
        int fw = (int)((float)(SCREEN_W - PAD * 2 - 2) * doneCount / taskCount);
        c.fillRect(PAD + 1, barY + 1, fw, 2, CLR_PROGRESS);
    }

    // List
    int yOff = barY + 6;
    int maxVis = (BODY_BOT - yOff) / ROW_H;

    for (int vi = 0; vi < maxVis && (scrollOff + vi) < taskCount; vi++) {
        int idx = scrollOff + vi;
        TaskItem& t = tasks[idx];
        int iy = yOff + vi * ROW_H;

        bool sel = (idx == selIdx);
        if (sel) {
            c.fillRoundRect(2, iy, SCREEN_W - 4, ROW_H - 1, 3, CLR_SELECTED_BG);
            c.setTextColor(CLR_SELECTED_TEXT);
        } else {
            c.setTextColor(t.completed ? CLR_TEXT_DIM : CLR_TEXT);
        }

        uint16_t ic = sel ? CLR_SELECTED_TEXT : (t.completed ? CLR_TEXT_DIM : CLR_ICON);
        drawIcon8(c, PAD + 2, iy + (ROW_H - 8) / 2, t.completed ? ICON_CHECK : ICON_BULLET, ic);

        c.setFont(&fonts::Font2);
        // Measure time estimate width first to know how much space title gets
        int timeEstW = 0;
        if (t.timeEst[0]) {
            c.setFont(&fonts::Font0);
            timeEstW = c.textWidth(t.timeEst) + 6;
            c.setFont(&fonts::Font2);
        }
        int titleMaxPx = SCREEN_W - PAD * 2 - 14 - timeEstW;
        char tt[40]; truncPx(c, tt, t.title, titleMaxPx);
        c.drawString(tt, PAD + 14, iy + (ROW_H - 16) / 2);

        // Time est on right
        if (t.timeEst[0]) {
            c.setFont(&fonts::Font0);
            int ew = c.textWidth(t.timeEst);
            c.drawString(t.timeEst, SCREEN_W - ew - PAD - 1, iy + (ROW_H - 8) / 2);
        }

        c.setTextColor(CLR_TEXT);
    }

    // Scroll indicators
    c.setFont(&fonts::Font0);
    if (scrollOff > 0) {
        c.fillTriangle(SCREEN_W - 10, yOff - 1, SCREEN_W - 14, yOff + 3, SCREEN_W - 6, yOff + 3, CLR_TEXT);
    }
    if (scrollOff + maxVis < taskCount) {
        int by = BODY_BOT - 5;
        c.fillTriangle(SCREEN_W - 10, by + 4, SCREEN_W - 14, by, SCREEN_W - 6, by, CLR_TEXT);
    }

    drawFooterHint(c, "UP/DOWN: move  SELECT: complete");
    drawNavDots(c, SCREEN_TASKS);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Events screen ───────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawEventsScreen(M5Canvas& c, EventItem* events, uint8_t eventCount,
                              const char* dateStr, bool use24h,
                              uint8_t scrollOff = 0)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "EVENTS", ICON_CAL);

    // Date in header
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_HEADER_TEXT);
    int dw = c.textWidth(dateStr);
    c.drawString(dateStr, SCREEN_W - dw - 28, (HDR_H - 8) / 2);

    c.setTextColor(CLR_TEXT);

    if (eventCount == 0) {
        c.setFont(&fonts::FreeSansBold9pt7b);
        int nw = c.textWidth("No events");
        c.drawString("No events", (SCREEN_W - nw) / 2, BODY_TOP + (BODY_BOT - BODY_TOP) / 2 - 16);
        c.setFont(&fonts::Font2);
        int ew = c.textWidth("Enjoy your day!");
        c.drawString("Enjoy your day!", (SCREEN_W - ew) / 2, BODY_TOP + (BODY_BOT - BODY_TOP) / 2 + 4);
    } else {
#if IS_EINK
        // CoreInk 200x200: vertical event cards.
        //
        // Layout per card (eventH = 30 px):
        //   col 0..7  → leading marker (accent bar = upcoming, check = past)
        //   col 12..  → row 1: time text + inline duration tag
        //              row 2: title text
        //
        // Duration is drawn INLINE on the time row (not floating top-right)
        // so it never collides with the up-arrow scroll indicator.
        // Past events are marked with a check icon instead of a strike-through.
        int yOff = BODY_TOP + 2;
        int eventH = 30;
        int evMaxVis = (BODY_BOT - yOff) / eventH;
        const int textX = PAD + 12;  // leaves room for accent bar OR check icon
        for (int i = scrollOff; i < eventCount && (i - scrollOff) < evMaxVis; i++) {
            EventItem& e = events[i];
            bool past = eventIsPast(e);

            // Leading marker
            if (past) {
                drawIcon8(c, PAD, yOff + (eventH - 4 - 8) / 2, ICON_CHECK, CLR_TEXT);
                c.setTextColor(CLR_TEXT_DIM);
            } else {
                c.fillRect(PAD, yOff, 3, eventH - 4, CLR_ACCENT);
            }

            // Row 1: time + inline duration tag
            c.setFont(&fonts::FreeSansBold9pt7b);
            char timeStr2[12];
            if (e.isAllDay) strlcpy(timeStr2, "All Day", sizeof(timeStr2));
            else formatApiTime(timeStr2, sizeof(timeStr2), e.startTime, use24h);
            c.drawString(timeStr2, textX, yOff);
            int timeW = c.textWidth(timeStr2);

            if (!e.isAllDay && e.durationMin > 0) {
                char dur[10];
                if (e.durationMin >= 60)
                    snprintf(dur, sizeof(dur), "%dh%02d", e.durationMin / 60, e.durationMin % 60);
                else
                    snprintf(dur, sizeof(dur), "%dm", e.durationMin);
                c.setFont(&fonts::Font0);
                int dw2 = c.textWidth(dur);
                // Generous padding: 5 px horizontal, ~2 px vertical, height 14
                int boxX = textX + timeW + 8;
                int boxY = yOff + 1;
                int boxW = dw2 + 10;
                int boxH = 14;
                c.drawRect(boxX, boxY, boxW, boxH, CLR_TEXT);
                c.drawString(dur, boxX + 5, boxY + 3);
            }

            // Row 2: title
            c.setFont(&fonts::Font2);
            char tt[40]; truncPx(c, tt, e.title, SCREEN_W - textX - PAD);
            c.drawString(tt, textX, yOff + 14);

            // Restore text color if dimmed for past
            if (past) c.setTextColor(CLR_TEXT);

            yOff += eventH;
            if (i < eventCount - 1) drawDottedLine(c, yOff - 2, textX, SCREEN_W - PAD);
        }
        // Scroll indicators (top-right & bottom-right; clear of inline duration)
        if (scrollOff > 0) {
            c.fillTriangle(SCREEN_W - 10, BODY_TOP + 3, SCREEN_W - 14, BODY_TOP + 7, SCREEN_W - 6, BODY_TOP + 7, CLR_TEXT);
        }
        if (scrollOff + evMaxVis < eventCount) {
            int by = BODY_BOT - 5;
            c.fillTriangle(SCREEN_W - 10, by + 4, SCREEN_W - 14, by, SCREEN_W - 6, by, CLR_TEXT);
        }
#else
        // StickC 240x135: compact one-row event cards.
        // Layout: [marker][time] [duration tag] [title fills rest]
        // Past events use a check icon in place of the accent bar (no strike).
        int yOff = BODY_TOP + 2;
        int eventH = 22;
        int evMaxVis = (BODY_BOT - yOff) / eventH;
        const int textX = PAD + 11;  // room for 8 px check icon or 2 px bar
        for (int i = scrollOff; i < eventCount && (i - scrollOff) < evMaxVis; i++) {
            EventItem& e = events[i];
            bool past = eventIsPast(e);

            // Leading marker
            if (past) {
                drawIcon8(c, PAD, yOff + (eventH - 8) / 2, ICON_CHECK, CLR_TEXT_DIM);
                c.setTextColor(CLR_TEXT_DIM);
            } else {
                c.fillRect(PAD, yOff + 2, 2, eventH - 6, CLR_ACCENT);
            }

            // Time
            c.setFont(&fonts::Font2);
            char timeStr2[16];
            if (e.isAllDay) strlcpy(timeStr2, "All Day", sizeof(timeStr2));
            else formatApiTime(timeStr2, sizeof(timeStr2), e.startTime, use24h);
            c.drawString(timeStr2, textX, yOff + 1);
            int timeW = c.textWidth(timeStr2);

            // Inline duration tag right after time (no longer floats top-right,
            // so it can never overlap the scroll-up arrow).
            int afterTagX = textX + timeW + 4;
            if (!e.isAllDay && e.durationMin > 0) {
                char dur[10];
                if (e.durationMin >= 60)
                    snprintf(dur, sizeof(dur), "%dh%02d", e.durationMin / 60, e.durationMin % 60);
                else
                    snprintf(dur, sizeof(dur), "%dm", e.durationMin);
                c.setFont(&fonts::Font0);
                int dw2 = c.textWidth(dur);
                int boxX = afterTagX;
                int boxY = yOff + 3;
                int boxW = dw2 + 8;       // 4 px h-padding inside
                int boxH = 13;            // a bit taller for breathing room
                c.drawRect(boxX, boxY, boxW, boxH, past ? CLR_TEXT_DIM : CLR_TEXT);
                c.drawString(dur, boxX + 4, boxY + 3);
                afterTagX = boxX + boxW + 5;
                c.setFont(&fonts::Font2);
            }

            // Title fills the remaining space
            int titleMaxPx = SCREEN_W - afterTagX - PAD;
            char tt[40]; truncPx(c, tt, e.title, titleMaxPx);
            c.drawString(tt, afterTagX, yOff + 1);

            if (past) c.setTextColor(CLR_TEXT);

            yOff += eventH;
            if (i < eventCount - 1) drawDottedLine(c, yOff - 2, textX, SCREEN_W - PAD);
        }
        // Scroll indicators (top-right & bottom-right; clear of inline duration)
        if (scrollOff > 0) {
            c.fillTriangle(SCREEN_W - 10, BODY_TOP + 3, SCREEN_W - 14, BODY_TOP + 7, SCREEN_W - 6, BODY_TOP + 7, CLR_TEXT);
        }
        if (scrollOff + evMaxVis < eventCount) {
            int by = BODY_BOT - 5;
            c.fillTriangle(SCREEN_W - 10, by + 4, SCREEN_W - 14, by, SCREEN_W - 6, by, CLR_TEXT);
        }
#endif
    }

    drawFooterHint(c, "UP/DOWN: scroll");
    drawNavDots(c, SCREEN_EVENTS);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Calendar standby screen ─────────────────────────────────────────────────
//
// A Sunsama-style day timeline used as an alternative deep-sleep (standby)
// layout.  Top to bottom is the flow of time; the current moment sits on a
// bold line in the vertical centre, with its clock time pinned in the left
// gutter.  Only a ±3 h window (6 h total) is shown, so the day appears to
// scroll upward as time passes.  Hour markers label the gutter; timeboxed
// tasks and calendar events are drawn as blocks positioned and sized by their
// start time and duration, with overlapping blocks split into side-by-side
// columns (lanes), exactly like a real calendar.
// ═══════════════════════════════════════════════════════════════════════════

inline void drawCalendarStandby(M5Canvas& c,
                                EventItem* events, uint8_t eventCount,
                                TaskItem* tasks, uint8_t taskCount,
                                const char* dateStr, const char* timeStr,
                                bool use24h, int batt, const char* ageStr,
                                bool timerActive, uint32_t timerElapsedSec)
{
    c.fillSprite(CLR_BG);

    // ── Current time as minutes-of-day (centre of the window) ─────────────────
    struct tm now_tm;
    int nowMins = 12 * 60;
    if (getLocalTime(&now_tm, 0)) nowMins = now_tm.tm_hour * 60 + now_tm.tm_min;

    // ── Top strip: date (left), running timer + sync age + battery (right) ────
    const int topH = IS_EINK ? 15 : 13;
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT);
    c.drawString(dateStr, PAD, (topH - 8) / 2);

    char rstr[28];
    char timerStr[12] = "";
    if (timerActive) {
        char el[10];
        formatElapsedMin(el, sizeof(el), timerElapsedSec);
        snprintf(timerStr, sizeof(timerStr), "%s ", el);
    }
    if (ageStr && ageStr[0])
        snprintf(rstr, sizeof(rstr), "%s%s  %d%%", timerStr, ageStr, batt);
    else
        snprintf(rstr, sizeof(rstr), "%s%d%%", timerStr, batt);
    c.setTextColor(CLR_TEXT_DIM);
    int rw = c.textWidth(rstr);
    c.drawString(rstr, SCREEN_W - rw - PAD, (topH - 8) / 2);
    c.setTextColor(CLR_TEXT);
    drawDottedLine(c, topH);

    // ── Timeline geometry ─────────────────────────────────────────────────────
    const int WINDOW   = 360;                 // ±3 h → 6 h total visible
    const int gutterW  = IS_EINK ? 40 : 38;   // room for the "HH:MM" now-chip
    const int colX     = gutterW;             // left edge of the block columns
    const int colW     = SCREEN_W - colX - PAD;
    const int top      = topH + 2;
    const int bot      = SCREEN_H - 1;
    const int availH   = bot - top;
    const float pxPerMin = (float)availH / (float)WINDOW;
    const int yCenter  = top + availH / 2;
    const int winStart = nowMins - WINDOW / 2;
    const int winEnd   = nowMins + WINDOW / 2;

    auto yOf = [&](int t) -> int {
        int y = yCenter + (int)lroundf((t - nowMins) * pxPerMin);
        if (y < top) y = top;
        if (y > bot) y = bot;
        return y;
    };

    // ── Hour gridlines + gutter labels ────────────────────────────────────────
    c.setFont(&fonts::Font0);
    for (int h = (winStart - 60) / 60; h <= (winEnd + 60) / 60; h++) {
        int hm = h * 60;
        if (hm < winStart || hm > winEnd) continue;
        int hy = yOf(hm);
        drawDottedLine(c, hy, colX, SCREEN_W - PAD);
        int hh = ((h % 24) + 24) % 24;
        char hlbl[8];
        if (use24h) {
            snprintf(hlbl, sizeof(hlbl), "%d:00", hh);
        } else {
            int h12 = hh % 12; if (h12 == 0) h12 = 12;
            snprintf(hlbl, sizeof(hlbl), "%d%s", h12, hh < 12 ? "a" : "p");
        }
        c.setTextColor(CLR_TEXT_DIM);
        int lw = c.textWidth(hlbl);
        c.drawString(hlbl, colX - lw - 3, hy - 3);
        c.setTextColor(CLR_TEXT);
    }

    // ── Gather visible blocks (events + timeboxed tasks) ──────────────────────
    struct Blk { int s; int e; const char* title; bool isTask; bool done;
                 int lane; int lanes; };
    static Blk blk[MAX_EVENTS + MAX_TASKS];
    int nb = 0;

    auto addBlock = [&](int s, int e, const char* title, bool isTask, bool done) {
        if (s < 0 || e <= s) return;
        // Fold across midnight so late-night windows still catch early/late blocks.
        if (e <= winStart && s + 1440 <= winEnd) { s += 1440; e += 1440; }
        else if (s >= winEnd && e - 1440 >= winStart) { s -= 1440; e -= 1440; }
        if (e <= winStart || s >= winEnd) return;          // outside window
        if (nb >= MAX_EVENTS + MAX_TASKS) return;
        blk[nb++] = { s, e, title, isTask, done, 0, 1 };
    };

    for (int i = 0; i < eventCount; i++) {
        EventItem& ev = events[i];
        if (ev.isAllDay) continue;
        int s = apiTimeToMinutes(ev.startTime);
        if (s < 0) continue;
        int e = s + (ev.durationMin > 0 ? ev.durationMin : 30);
        addBlock(s, e, ev.title, false, false);
    }
    for (int i = 0; i < taskCount; i++) {
        TaskItem& tk = tasks[i];
        if (!tk.projStart[0] || !tk.projEnd[0]) continue;
        int s = apiTimeToMinutes(tk.projStart);
        int e = apiTimeToMinutes(tk.projEnd);
        if (s < 0 || e < 0) continue;
        addBlock(s, e, tk.title, true, tk.completed);
    }

    // ── Lane assignment: split overlapping blocks into side-by-side columns ───
    // Sort by start (insertion sort — nb is tiny).
    for (int i = 1; i < nb; i++) {
        Blk key = blk[i];
        int j = i - 1;
        while (j >= 0 && blk[j].s > key.s) { blk[j + 1] = blk[j]; j--; }
        blk[j + 1] = key;
    }
    for (int i = 0; i < nb; ) {
        int clusterEnd = blk[i].e;
        int j = i + 1;
        while (j < nb && blk[j].s < clusterEnd) {
            if (blk[j].e > clusterEnd) clusterEnd = blk[j].e;
            j++;
        }
        int laneEnd[8]; int nLanes = 0;
        for (int k = i; k < j; k++) {
            int placed = -1;
            for (int l = 0; l < nLanes; l++) {
                if (blk[k].s >= laneEnd[l]) { placed = l; break; }
            }
            if (placed < 0) { placed = nLanes; if (nLanes < 8) nLanes++; }
            laneEnd[placed] = blk[k].e;
            blk[k].lane = placed;
        }
        for (int k = i; k < j; k++) blk[k].lanes = (nLanes < 1 ? 1 : nLanes);
        i = j;
    }

    // ── Draw blocks ───────────────────────────────────────────────────────────
    for (int i = 0; i < nb; i++) {
        Blk& b = blk[i];
        int y0 = yOf(b.s);
        int y1 = yOf(b.e);
        int h = y1 - y0;
        if (h < 9) h = 9;                       // keep room for one text line
        int laneW = colW / b.lanes;
        int bx = colX + b.lane * laneW;
        int bw = laneW - 1;
        if (bw < 12) bw = 12;

        uint16_t barColor = b.isTask ? CLR_ACCENT2 : CLR_ACCENT;
        // Body
        c.fillRect(bx + 3, y0, bw - 3, h, IS_COLOR ? CLR_CARD_BG : CLR_BG);
        c.drawRect(bx, y0, bw, h, b.done ? CLR_DIVIDER : barColor);
        // Type marker on the left edge: events = solid bar, tasks = striped bar
        // (the stripe distinguishes them on monochrome e-ink too).
        if (b.isTask) {
            for (int yy = y0 + 1; yy < y0 + h - 1; yy += 2)
                c.drawFastHLine(bx, yy, 3, b.done ? CLR_DIVIDER : barColor);
        } else {
            c.fillRect(bx, y0, 3, h, barColor);
        }

        // Text — time line then title when tall enough, else title only.
        uint16_t txtColor = b.done ? CLR_TEXT_DIM : CLR_TEXT;
        int tx = bx + 6;
        int tw = bw - 8;
        if (tw < 10) { continue; }
        // Build start-time label from minutes-of-day.
        char ts[12];
        {
            int sm = ((b.s % 1440) + 1440) % 1440;
            int sh = sm / 60, smin = sm % 60;
            if (use24h) snprintf(ts, sizeof(ts), "%d:%02d", sh, smin);
            else {
                int h12 = sh % 12; if (h12 == 0) h12 = 12;
                snprintf(ts, sizeof(ts), "%d:%02d%s", h12, smin, sh < 12 ? "a" : "p");
            }
        }
        c.setTextColor(txtColor);
        if (h >= 24) {
            c.setFont(&fonts::Font0);
            char tline[12]; truncPx(c, tline, ts, tw);
            c.drawString(tline, tx, y0 + 2);
            c.setFont(&fonts::Font2);
            char tt[48]; truncPx(c, tt, b.title, tw);
            c.drawString(tt, tx, y0 + 10);
        } else {
            c.setFont(&fonts::Font0);
            char tt[48]; truncPx(c, tt, b.title, tw);
            c.drawString(tt, tx, y0 + (h - 8) / 2);
        }
        c.setTextColor(CLR_TEXT);
    }

    if (nb == 0) {
        c.setFont(&fonts::Font2);
        c.setTextColor(CLR_TEXT_DIM);
        const char* msg = "No events or tasks";
        int mw = c.textWidth(msg);
        c.drawString(msg, colX + (colW - mw) / 2, top + 6);
        c.setTextColor(CLR_TEXT);
    }

    // ── "Now" line (drawn last so it sits on top of blocks) ───────────────────
    c.drawFastHLine(colX, yCenter, SCREEN_W - colX - PAD, CLR_ACCENT);
    if (IS_EINK) c.drawFastHLine(colX, yCenter + 1, SCREEN_W - colX - PAD, CLR_ACCENT);
    c.fillCircle(colX, yCenter, 2, CLR_ACCENT);

    // Current time pinned in the gutter on an accent chip for emphasis.  The
    // chip may overhang the first column slightly — it sits on top, like the
    // current-time pill in Sunsama.
    c.setFont(&fonts::Font2);
    int ctw = c.textWidth(timeStr);
    int chipH = 16;
    int chipY = yCenter - chipH / 2;
    int chipW = ctw + 6;
    c.fillRect(0, chipY, chipW, chipH, CLR_ACCENT);
    c.setTextColor(IS_COLOR ? TFT_WHITE : CLR_BG);
    c.drawString(timeStr, 3, chipY);
    c.setTextColor(CLR_TEXT);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Timer screen ────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawTimerScreen(M5Canvas& c, TimerInfo& timer,
                             TaskItem* tasks, uint8_t taskCount,
                             uint8_t selIdx, bool use24h = true,
                             uint8_t pickerScrollOff = 0)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "TIMER", ICON_TIMER);

    if (timer.active) {
        // Find projected time for the active task
        char projStr[28] = "";
        for (int i = 0; i < taskCount; i++) {
            if (strcmp(tasks[i].id, timer.taskId) == 0) {
                if (tasks[i].projStart[0]) {
                    char t24s[12], t24e[12];
                    formatApiTime(t24s, sizeof(t24s), tasks[i].projStart, use24h);
                    formatApiTime(t24e, sizeof(t24e), tasks[i].projEnd, use24h);
                    snprintf(projStr, sizeof(projStr), "%s - %s", t24s, t24e);
                }
                break;
            }
        }

#if IS_EINK
        // CoreInk: big timer circle
        int cx = SCREEN_W / 2, cy = 84, r = 44;
        c.drawCircle(cx, cy, r, CLR_TEXT);
        c.drawCircle(cx, cy, r - 1, CLR_TEXT);
        for (int i = 0; i < 12; i++) {
            float a = i * 30.0f * DEG_TO_RAD;
            int x1 = cx + (int)((r - 4) * sin(a)), y1 = cy - (int)((r - 4) * cos(a));
            int x2 = cx + (int)((r - 8) * sin(a)), y2 = cy - (int)((r - 8) * cos(a));
            c.drawLine(x1, y1, x2, y2, CLR_TEXT);
        }

        char el[16];
        formatElapsedMin(el, sizeof(el), timer.elapsedSec);
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextColor(CLR_TEXT);
        int tw = c.textWidth(el);
        c.drawString(el, cx - tw / 2, cy - 12);

        c.setFont(&fonts::Font2);
        char tt[28]; truncPx(c, tt, timer.taskTitle, SCREEN_W - 20);
        int ntw = c.textWidth(tt);
        c.drawString(tt, (SCREEN_W - ntw) / 2, 136);

        if (projStr[0]) {
            c.setFont(&fonts::Font0);
            c.setTextColor(CLR_TEXT_DIM);
            int pw = c.textWidth(projStr);
            c.drawString(projStr, (SCREEN_W - pw) / 2, 152);
        }

        drawFooterHint(c, "SELECT: stop timer");
#else
        // StickC landscape: horizontal timer display (second-level)
        char el[16];
        formatElapsedSec(el, sizeof(el), timer.elapsedSec);

        // Large centered elapsed time
        c.setFont(&fonts::FreeSansBold18pt7b);
        c.setTextColor(CLR_TEXT);
        int tw = c.textWidth(el);
        c.drawString(el, (SCREEN_W - tw) / 2, BODY_TOP + 6);

        // Task name below
        c.setFont(&fonts::Font2);
        char tt[36]; truncPx(c, tt, timer.taskTitle, SCREEN_W - 20);
        int ntw = c.textWidth(tt);
        c.drawString(tt, (SCREEN_W - ntw) / 2, BODY_TOP + 38);

        // Projected time slot
        if (projStr[0]) {
            c.setFont(&fonts::Font0);
            c.setTextColor(CLR_TEXT_DIM);
            int pw = c.textWidth(projStr);
            c.drawString(projStr, (SCREEN_W - pw) / 2, BODY_TOP + 56);
            c.setTextColor(CLR_TEXT);
        }

        // Small timer ring on the side
        int ringX = SCREEN_W - 40, ringY = BODY_TOP + 20;
        c.drawCircle(ringX, ringY, 16, CLR_ACCENT);
        drawIcon8(c, ringX - 4, ringY - 4, ICON_TIMER, CLR_ACCENT);

        drawFooterHint(c, "SELECT: stop timer");
#endif
    } else {
        // Task picker — same layout on both boards using adaptive constants
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextColor(CLR_TEXT);
        c.drawString("Start timer:", PAD + 2, BODY_TOP);
        drawDottedLine(c, BODY_TOP + 16);

        int yOff = BODY_TOP + 19;
        int maxVis = (BODY_BOT - yOff - 2) / ROW_H;

        uint8_t skipped = 0, shown = 0;
        uint8_t totalIncomp = 0;
        for (int i = 0; i < taskCount; i++) if (!tasks[i].completed) totalIncomp++;

        for (int i = 0; i < taskCount; i++) {
            if (tasks[i].completed) continue;
            if (skipped < pickerScrollOff) { skipped++; continue; }
            if (shown >= (uint8_t)maxVis) break;
            int iy = yOff + shown * ROW_H;
            bool sel = (i == selIdx);

            if (sel) {
                c.fillRoundRect(2, iy, SCREEN_W - 4, ROW_H - 1, 3, CLR_SELECTED_BG);
                c.setTextColor(CLR_SELECTED_TEXT);
            } else {
                c.setTextColor(CLR_TEXT);
            }

            uint16_t ic = sel ? CLR_SELECTED_TEXT : CLR_ICON;
            drawIcon8(c, PAD + 2, iy + (ROW_H - 8) / 2, ICON_CLOCK, ic);
            c.setFont(&fonts::Font2);
            char tt[36]; truncPx(c, tt, tasks[i].title, SCREEN_W - PAD * 2 - 20);
            c.drawString(tt, PAD + 14, iy + (ROW_H - 16) / 2);

            c.setTextColor(CLR_TEXT);
            shown++;
        }

        // Scroll indicators
        if (pickerScrollOff > 0) {
            c.fillTriangle(SCREEN_W - 10, yOff - 1, SCREEN_W - 14, yOff + 3, SCREEN_W - 6, yOff + 3, CLR_TEXT);
        }
        if (pickerScrollOff + maxVis < totalIncomp) {
            int by = BODY_BOT - 5;
            c.fillTriangle(SCREEN_W - 10, by + 4, SCREEN_W - 14, by, SCREEN_W - 6, by, CLR_TEXT);
        }

        if (shown == 0) {
            c.setFont(&fonts::FreeSansBold9pt7b);
            int aw = c.textWidth("All done!");
            c.drawString("All done!", (SCREEN_W - aw) / 2, BODY_TOP + 40);
        }

        drawFooterHint(c, "UP/DOWN: move  SELECT: start");
    }

    drawNavDots(c, SCREEN_TIMER);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Stats screen ────────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawStatsScreen(M5Canvas& c, TaskItem* tasks, uint8_t taskCount,
                             TimerInfo& timer, int batt)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "STATS", ICON_CHART);
    drawBattery(c, batt);

    uint8_t doneCount = 0;
    for (int i = 0; i < taskCount; i++) if (tasks[i].completed) doneCount++;

    // WiFi signal (shared by both layouts)
    int32_t rssi = WiFi.RSSI();
    const char* sigStr = "N/A";
    char rssiBuf[12];
    if (WiFi.status() == WL_CONNECTED) {
        if (rssi > -50) sigStr = "Excellent";
        else if (rssi > -60) sigStr = "Good";
        else if (rssi > -70) sigStr = "Fair";
        else { snprintf(rssiBuf, sizeof(rssiBuf), "%lddBm", (long)rssi); sigStr = rssiBuf; }
    }

    // Label-value row helper positions
    int labelX = PAD + 4;
    int valueR = SCREEN_W - PAD - 4;  // right-align values to this X

    c.setTextColor(CLR_TEXT);

#if IS_EINK
    // ── CoreInk 200x200: clean card-style stat rows ──
    int yOff = BODY_TOP + 4;

    // ── Large task completion fraction ──
    c.setFont(&fonts::FreeSansBold18pt7b);
    char bigFrac[12];
    snprintf(bigFrac, sizeof(bigFrac), "%d / %d", doneCount, taskCount);
    int bfw = c.textWidth(bigFrac);
    c.drawString(bigFrac, (SCREEN_W - bfw) / 2, yOff);

    c.setFont(&fonts::Font2);
    int lbl1w = c.textWidth("tasks completed");
    c.setTextColor(CLR_TEXT_DIM);
    c.drawString("tasks completed", (SCREEN_W - lbl1w) / 2, yOff + 30);
    c.setTextColor(CLR_TEXT);
    yOff += 46;

    // Progress bar
    int barW = SCREEN_W - labelX * 2;
    c.drawRect(labelX, yOff, barW, 8, CLR_TEXT);
    if (taskCount > 0) {
        int fw = (int)((float)(barW - 2) * doneCount / taskCount);
        c.fillRect(labelX + 1, yOff + 1, fw, 6, CLR_PROGRESS);
    }
    yOff += 16;
    drawDottedLine(c, yOff);
    yOff += 6;

    // Stat rows: label left, value right-aligned
    c.setFont(&fonts::Font2);
    char val[16];
    int statRowH = 20;

    // Timer
    c.drawString("Timer", labelX, yOff);
    char timerVal[16];
    if (timer.active) formatElapsedMin(timerVal, sizeof(timerVal), timer.elapsedSec);
    else snprintf(timerVal, sizeof(timerVal), "Stopped");
    int tvw = c.textWidth(timerVal);
    c.drawString(timerVal, valueR - tvw, yOff);
    yOff += statRowH;

    // Battery
    c.drawString("Battery", labelX, yOff);
    snprintf(val, sizeof(val), "%d%%", batt);
    int bvw = c.textWidth(val);
    c.drawString(val, valueR - bvw, yOff);
    yOff += statRowH;

    // WiFi
    c.drawString("WiFi", labelX, yOff);
    int svw = c.textWidth(sigStr);
    c.drawString(sigStr, valueR - svw, yOff);
    yOff += statRowH;

    // Board
    drawDottedLine(c, yOff);
    yOff += 4;
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    c.drawString("Sunsamagotchi | M5Stack CoreInk", labelX, yOff);

#else
    // ── StickC 240x135: two-panel layout ──
    int midX = SCREEN_W / 2 - 2;
    int yOff = BODY_TOP + 2;

    // LEFT: Big completion number + progress bar
    c.setFont(&fonts::FreeSansBold18pt7b);
    char bigFrac[8];
    snprintf(bigFrac, sizeof(bigFrac), "%d/%d", doneCount, taskCount);
    int bfw = c.textWidth(bigFrac);
    c.drawString(bigFrac, (midX - bfw) / 2, yOff);

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    int lbl1w = c.textWidth("tasks done");
    c.drawString("tasks done", (midX - lbl1w) / 2, yOff + 30);
    c.setTextColor(CLR_TEXT);

    // Progress bar
    int barX = PAD + 4, barW = midX - barX * 2;
    int barY = yOff + 44;
    c.drawRect(barX, barY, barW, 6, CLR_TEXT);
    if (taskCount > 0) {
        int fw = (int)((float)(barW - 2) * doneCount / taskCount);
        c.fillRect(barX + 1, barY + 1, fw, 4, CLR_PROGRESS);
    }

    // Vertical divider
    c.drawFastVLine(midX, BODY_TOP, BODY_BOT - BODY_TOP - 2, CLR_DIVIDER);

    // RIGHT: stat rows
    int rX = midX + 8;
    int rValR = SCREEN_W - PAD - 4;
    int ry = BODY_TOP + 2;
    int statRowH = 18;
    char val[16];

    c.setFont(&fonts::Font2);

    // Timer
    c.drawString("Timer", rX, ry);
    char timerVal[16];
    if (timer.active) formatElapsedMin(timerVal, sizeof(timerVal), timer.elapsedSec);
    else snprintf(timerVal, sizeof(timerVal), "Off");
    int tvw = c.textWidth(timerVal);
    c.drawString(timerVal, rValR - tvw, ry);
    ry += statRowH;
    drawDottedLine(c, ry - 2, rX, SCREEN_W - PAD);

    // Battery
    c.drawString("Battery", rX, ry);
    snprintf(val, sizeof(val), "%d%%", batt);
    int bvw = c.textWidth(val);
    c.drawString(val, rValR - bvw, ry);
    ry += statRowH;
    drawDottedLine(c, ry - 2, rX, SCREEN_W - PAD);

    // WiFi
    c.drawString("WiFi", rX, ry);
    int svw = c.textWidth(sigStr);
    c.drawString(sigStr, rValR - svw, ry);
    ry += statRowH;
    drawDottedLine(c, ry - 2, rX, SCREEN_W - PAD);

    // Board
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    c.drawString("Sunsamagotchi | StickC Plus2", rX, ry);
#endif

    drawNavDots(c, SCREEN_STATS);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Settings screen ─────────────────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawSettingsScreen(M5Canvas& c, const AppSettings& settings,
                                uint8_t selIdx, int batt, bool editMode,
                                uint8_t scrollOff = 0)
{
    c.fillSprite(CLR_BG);
    drawHeader(c, "SETTINGS", ICON_GEAR);

    int yOff = BODY_TOP + 2;
    // Fixed, comfortable row height — the list scrolls (like Tasks/Events)
    // instead of rows shrinking indefinitely as SETT_COUNT grows.
    int infoH = IS_EINK ? 30 : 14;
    int rowH  = IS_EINK ? 22 : 20;
    int maxVis = (BODY_BOT - yOff - infoH) / rowH;
    if (maxVis < 1) maxVis = 1;

    for (uint8_t vi = 0; vi < maxVis && (scrollOff + vi) < SETT_COUNT; vi++) {
        uint8_t i = scrollOff + vi;
        int iy = yOff + vi * rowH;
        bool sel = (i == selIdx);

        if (sel) {
            c.fillRoundRect(2, iy, SCREEN_W - 4, rowH - 1, 3, CLR_SELECTED_BG);
            c.setTextColor(CLR_SELECTED_TEXT);
        } else {
            c.setTextColor(CLR_TEXT);
        }

        c.setFont(&fonts::Font2);
        c.drawString(Settings::itemLabel((SettingsItem)i), PAD + 4, iy + (rowH - 16) / 2);

        char val[16];
        Settings::formatValue(val, sizeof(val), settings, (SettingsItem)i);
        int vw = c.textWidth(val);

        // If selected + edit mode, show arrows around value
        if (sel && editMode) {
            char editVal[20];
            snprintf(editVal, sizeof(editVal), "< %s >", val);
            vw = c.textWidth(editVal);
            c.drawString(editVal, SCREEN_W - vw - PAD - 2, iy + (rowH - 16) / 2);
        } else {
            c.drawString(val, SCREEN_W - vw - PAD - 2, iy + (rowH - 16) / 2);
        }

        c.setTextColor(CLR_TEXT);
    }

    // Scroll indicators — same triangle style as Tasks/Events
    if (scrollOff > 0) {
        c.fillTriangle(SCREEN_W - 10, yOff - 1, SCREEN_W - 14, yOff + 3, SCREEN_W - 6, yOff + 3, CLR_TEXT);
    }
    if (scrollOff + maxVis < SETT_COUNT) {
        int by = yOff + maxVis * rowH - 5;
        c.fillTriangle(SCREEN_W - 10, by + 4, SCREEN_W - 14, by, SCREEN_W - 6, by, CLR_TEXT);
    }

    // Device info below settings
    int infoY = yOff + maxVis * rowH + 4;
    drawDottedLine(c, infoY);
    infoY += 4;
    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    char info[48];
    char fwShort[20];
    trunc(fwShort, FIRMWARE_VERSION, sizeof(fwShort));
#if IS_EINK
    snprintf(info, sizeof(info), "WiFi: %s  Batt: %d%%", NetCfg::ssid(), batt);
    c.drawString(info, PAD + 2, infoY);
    snprintf(info, sizeof(info), "CoreInk | fw %s", fwShort);
    c.drawString(info, PAD + 2, infoY + 12);
#else
    // StickC: limited vertical space — single compact line, version wins over
    // WiFi status here since that's already visible via the header icon.
    snprintf(info, sizeof(info), "Batt: %d%%  fw %s", batt, fwShort);
    c.drawString(info, PAD + 2, infoY);
#endif

    if (editMode)
        drawFooterHint(c, "UP/DOWN: change  SELECT: done");
    else
        drawFooterHint(c, "UP/DOWN: move  SELECT: edit");

    drawNavDots(c, SCREEN_SETTINGS);
}

// ═══════════════════════════════════════════════════════════════════════════
// ── Confirmation dialog overlay ─────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════

inline void drawConfirmDialog(M5Canvas& c, const char* title, const char* message) {
    // Adaptive dialog size
    int bw = SCREEN_W - 20;
    int bh = IS_EINK ? 90 : 70;
    int bx = 10;
    int by = (SCREEN_H - bh) / 2;

    c.fillRect(bx, by, bw, bh, CLR_BG);
    c.drawRect(bx, by, bw, bh, CLR_TEXT);
    c.drawRect(bx + 1, by + 1, bw - 2, bh - 2, CLR_TEXT);

    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(CLR_TEXT);
    int tw = c.textWidth(title);
    c.drawString(title, (SCREEN_W - tw) / 2, by + 8);

    c.setFont(&fonts::Font2);
    char tt[36]; truncPx(c, tt, message, bw - 16);
    int mw = c.textWidth(tt);
    c.drawString(tt, (SCREEN_W - mw) / 2, by + 28);

    drawDottedLine(c, by + bh - 28, bx + 6, bx + bw - 6);

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT);
#if IS_EINK
    const char* confHint = "PRESS=Yes  Any other=Cancel";
#else
    const char* confHint = "SELECT=Yes  UP/DOWN=Cancel";
#endif
    int hintW = c.textWidth(confHint);
    c.drawString(confHint, (SCREEN_W - hintW) / 2, by + bh - 18);
}

// ── OTA update prompt — 3 cycleable options (Install / Remind / Skip) ───────
inline void drawOtaPrompt(M5Canvas& c, const char* tag, uint8_t sel) {
    int bw = SCREEN_W - 20;
    int bh = IS_EINK ? 110 : 92;
    int bx = 10;
    int by = (SCREEN_H - bh) / 2;

    c.fillRect(bx, by, bw, bh, CLR_BG);
    c.drawRect(bx, by, bw, bh, CLR_TEXT);
    c.drawRect(bx + 1, by + 1, bw - 2, bh - 2, CLR_TEXT);

    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(CLR_TEXT);
    const char* title = "Update available";
    int tw = c.textWidth(title);
    c.drawString(title, (SCREEN_W - tw) / 2, by + 6);

    c.setFont(&fonts::Font0);
    char sub[40]; snprintf(sub, sizeof(sub), "fw %s", tag);
    char subTrunc[36]; truncPx(c, subTrunc, sub, bw - 16);
    int sw = c.textWidth(subTrunc);
    c.drawString(subTrunc, (SCREEN_W - sw) / 2, by + 26);

    static const char* OPT_LABEL[OTA_OPT_COUNT] = {
        "Install now", "Remind me tomorrow", "Skip this version",
    };
    int rowH = 18;
    int rowY = by + 38;
    for (uint8_t i = 0; i < OTA_OPT_COUNT; i++) {
        int iy = rowY + i * rowH;
        bool sel_ = (i == sel);
        if (sel_) {
            c.fillRoundRect(bx + 6, iy, bw - 12, rowH - 2, 3, CLR_SELECTED_BG);
            c.setTextColor(CLR_SELECTED_TEXT);
        } else {
            c.setTextColor(CLR_TEXT);
        }
        c.drawString(OPT_LABEL[i], bx + 12, iy + 2);
    }

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
#if IS_EINK
    const char* hint = "UP/DOWN=choose  PRESS=confirm";
#else
    const char* hint = "UP/DOWN=choose  SELECT=confirm";
#endif
    int hw = c.textWidth(hint);
    c.drawString(hint, (SCREEN_W - hw) / 2, by + bh - 12);
}

// ── OTA status screen — used for every blocking OTA phase (checking,
// downloading/flashing, success, failure) so text width/padding is handled
// in exactly one place instead of ad-hoc canvas calls scattered per call
// site. pct < 0 means "no determinate progress" (e.g. the manifest check,
// which is a single quick request with nothing to report a percentage on):
// draws a plain indeterminate "working" bar instead of a percentage fill.
inline void drawOtaStatus(M5Canvas& c, const char* title, const char* subtitle = nullptr, int pct = -1) {
    c.fillSprite(CLR_BG);

    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(CLR_TEXT);
    char t[28]; truncPx(c, t, title, SCREEN_W - 2 * PAD - 8);
    int tw = c.textWidth(t);
    int titleY = SCREEN_H / 2 - 30;
    c.drawString(t, (SCREEN_W - tw) / 2, titleY);

    if (subtitle) {
        c.setFont(&fonts::Font2);
        c.setTextColor(CLR_TEXT_DIM);
        char s[36]; truncPx(c, s, subtitle, SCREEN_W - 2 * PAD - 8);
        int sw = c.textWidth(s);
        c.drawString(s, (SCREEN_W - sw) / 2, titleY + 20);
        c.setTextColor(CLR_TEXT);
    }

    int barW = SCREEN_W - 40, barH = 14;
    int barX = 20, barY = SCREEN_H / 2 + 6;
    c.drawRect(barX, barY, barW, barH, CLR_TEXT);

    if (pct >= 0) {
        int fillW = (barW - 4) * pct / 100;
        if (fillW > 0) c.fillRect(barX + 2, barY + 2, fillW, barH - 4, CLR_TEXT);
        c.setFont(&fonts::Font2);
        char pctStr[8]; snprintf(pctStr, sizeof(pctStr), "%d%%", pct);
        int pw = c.textWidth(pctStr);
        c.drawString(pctStr, (SCREEN_W - pw) / 2, barY + barH + 8);
    } else {
        // Indeterminate: three evenly-spaced dashes signal "working" without
        // claiming a fake percentage for a request that has no progress to report.
        int segW = (barW - 8) / 3;
        for (int i = 0; i < 3; i++) {
            c.fillRect(barX + 3 + i * segW, barY + 3, segW - 4, barH - 6, CLR_TEXT_DIM);
        }
    }

    c.setFont(&fonts::Font0);
    c.setTextColor(CLR_TEXT_DIM);
    const char* warn = (pct >= 0) ? "Do not power off" : "";
    if (warn[0]) {
        int ww = c.textWidth(warn);
        c.drawString(warn, (SCREEN_W - ww) / 2, SCREEN_H - 16);
    }
}

// ── Completion animation ────────────────────────────────────────────────────

inline void drawCompletionAnim(M5Canvas& c, const char* taskTitle) {
    c.fillSprite(CLR_BG);

    int cx = SCREEN_W / 2, cy = IS_EINK ? 70 : 45;
    int r = IS_EINK ? 28 : 20;
    c.drawCircle(cx, cy, r, CLR_TEXT);
    c.drawCircle(cx, cy, r - 1, CLR_TEXT);

    // Checkmark
    int s = r / 3;
    c.drawLine(cx - s - 2, cy, cx - 2, cy + s, CLR_TEXT);
    c.drawLine(cx - s - 1, cy, cx - 1, cy + s, CLR_TEXT);
    c.drawLine(cx - 2, cy + s, cx + s + 4, cy - s + 2, CLR_TEXT);
    c.drawLine(cx - 1, cy + s, cx + s + 5, cy - s + 2, CLR_TEXT);

    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(CLR_TEXT);
    int tw = c.textWidth("Done!");
    c.drawString("Done!", (SCREEN_W - tw) / 2, cy + r + 8);

    c.setFont(&fonts::Font2);
    char tt[36]; truncPx(c, tt, taskTitle, SCREEN_W - 20);
    int mw = c.textWidth(tt);
    c.drawString(tt, (SCREEN_W - mw) / 2, cy + r + 28);
}

} // namespace UI
