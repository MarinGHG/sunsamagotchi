// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Marin Benke
#pragma once

#include <M5Unified.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_wifi.h>

// ─── Board Detection ────────────────────────────────────────────────────────
#if !defined(BOARD_COREINK) && !defined(BOARD_M5STICK_CPLUS2)
  #define BOARD_COREINK  // Default fallback
#endif

// ─── Screen Dimensions ─────────────────────────────────────────────────────
#ifdef BOARD_COREINK
  #define SCREEN_W        200
  #define SCREEN_H        200
  #define IS_EINK         true
  #define IS_COLOR        false
  #define HAS_THREE_BTNS  true
#endif

#ifdef BOARD_M5STICK_CPLUS2
  // StickC Plus2: 135x240 TFT in LANDSCAPE (rotation 3) → 240x135
  #define SCREEN_W        240
  #define SCREEN_H        135
  #define IS_EINK         false
  #define IS_COLOR        true
  #define HAS_THREE_BTNS  true
#endif

// ─── Color Palette ──────────────────────────────────────────────────────────
#if IS_COLOR
  // TFT — Sunsamagotchi warm dark theme with amber/orange brand accent
  #define CLR_BG           0x1082   // Dark charcoal
  #define CLR_CARD_BG      0x2104   // Slightly lighter card bg
  #define CLR_TEXT         TFT_WHITE
  #define CLR_TEXT_DIM     0x8C71   // Warm gray text
  #define CLR_ACCENT       0xFB60   // Sunsama brand orange
  #define CLR_ACCENT2      0x5FDF   // Teal secondary accent
  #define CLR_HEADER_BG    0x3180   // Dark warm amber header
  #define CLR_HEADER_TEXT  TFT_WHITE
  #define CLR_SELECTED_BG  0xFB60   // Orange selection
  #define CLR_SELECTED_TEXT TFT_BLACK
  #define CLR_PROGRESS     0xFB60   // Orange progress bar
  #define CLR_DIVIDER      0x528A   // Warm dark divider
  #define CLR_ICON         0xFEA0   // Warm yellow-orange icons
  #define CLR_ICON_DIM     0x8C71   // Dim icon
  #define CLR_WARN         0xF800   // Red for overcommit
  #define CLR_SUN          0xFD40   // Sun amber-orange (character drawing)
  #define CLR_CLOUD        0xCE18   // Cloud blue-gray (character drawing)
#else
  // E-ink — pure black/white only
  #define CLR_BG           TFT_WHITE
  #define CLR_CARD_BG      TFT_WHITE
  #define CLR_TEXT         TFT_BLACK
  #define CLR_TEXT_DIM     TFT_BLACK
  #define CLR_ACCENT       TFT_BLACK
  #define CLR_ACCENT2      TFT_BLACK
  #define CLR_HEADER_BG    TFT_BLACK
  #define CLR_HEADER_TEXT  TFT_WHITE
  #define CLR_SELECTED_BG  TFT_BLACK
  #define CLR_SELECTED_TEXT TFT_WHITE
  #define CLR_PROGRESS     TFT_BLACK
  #define CLR_DIVIDER      TFT_BLACK
  #define CLR_ICON         TFT_BLACK
  #define CLR_ICON_DIM     TFT_BLACK
  #define CLR_WARN         TFT_BLACK
  #define CLR_SUN          0x8410   // Mid gray for sun on e-ink
  #define CLR_CLOUD        TFT_WHITE
#endif

// ─── Button Mapping ─────────────────────────────────────────────────────────
//
// CoreInk:
//   Dial UP    = BtnA (G37)  → UP / scroll up
//   Dial PRESS = BtnB (G38)  → CONFIRM / SELECT  (also wake source)
//   Dial DOWN  = BtnC (G39)  → DOWN / scroll down
//   Top button = BtnEXT      → PAGE (next screen) / long-hold REFRESH
//
// StickC Plus2 (landscape):
//   Upper-left (GPIO35)  = direct GPIO poll  → PAGE (next screen) / power-off hold
//   Front      (GPIO37)  = BtnA              → CONFIRM / SELECT
//   Lower-right(GPIO39)  = BtnB              → DOWN / scroll down
//   Front long-hold                          → REFRESH

namespace HAL {

// ── GPIO35 direct button (StickC Plus2 upper-left / power button) ───────────
// M5Unified does not reliably expose BtnEXT on the StickC Plus2, so we poll
// GPIO35 directly with falling-edge detection.
#ifdef BOARD_M5STICK_CPLUS2
static bool _g35High = true;   // HIGH = not pressed (internal pull-up)
static bool _g35Edge = false;  // true for exactly one poll cycle after press

inline void initG35() {
    pinMode(35, INPUT_PULLUP);
    _g35High = (bool)digitalRead(35);
}

inline void pollG35() {
    bool cur = (bool)digitalRead(35);
    _g35Edge = (!cur && _g35High);   // falling edge → press
    _g35High = cur;
}

inline bool g35IsHeld()       { return !digitalRead(35); }
inline bool g35JustPressed()  { return _g35Edge; }
#endif

// ── Button state queries ────────────────────────────────────────────────────

inline bool upPressed() {
#ifdef BOARD_COREINK
    return M5.BtnA.wasPressed();    // Dial UP (G37)
#else
    return false;                   // StickC: no dedicated UP; upper-left = PAGE
#endif
}

inline bool downPressed() {
#ifdef BOARD_COREINK
    return M5.BtnC.wasPressed();    // Dial DOWN (G39)
#else
    return M5.BtnB.wasPressed();    // Lower-right (GPIO39)
#endif
}

inline bool confirmPressed() {
#ifdef BOARD_COREINK
    return M5.BtnB.wasPressed();    // Dial PRESS (G38)
#else
    return M5.BtnA.wasPressed();    // Front button (GPIO37)
#endif
}

// Cycles to next screen. CoreInk: top button. StickC: upper-left GPIO35.
inline bool pagePressed() {
#ifdef BOARD_COREINK
    return M5.BtnEXT.wasPressed();
#else
    return g35JustPressed();
#endif
}

inline bool forceRefreshHeld() {
#ifdef BOARD_COREINK
    return M5.BtnEXT.wasHold();
#else
    return M5.BtnA.wasHold();       // Long-press front button
#endif
}

// ── Display helpers ─────────────────────────────────────────────────────────
inline void pushCanvas(M5Canvas& canvas) {
    canvas.pushSprite(0, 0);
#if IS_EINK
    M5.Display.waitDisplay();
#endif
}

inline void backlightOff() {
#ifdef BOARD_M5STICK_CPLUS2
    M5.Display.setBrightness(0);
#endif
}

inline void backlightOn() {
#ifdef BOARD_M5STICK_CPLUS2
    M5.Display.setBrightness(64);
#endif
}

inline void setFastMode() {
#if IS_EINK
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);
#endif
}

inline void setQualityMode() {
#if IS_EINK
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
#endif
}

// ── Sleep ───────────────────────────────────────────────────────────────────
// While in deep sleep we wake every minute to update the displayed clock with
// a partial refresh.  Data is only refetched from Sunsama on every Nth wake
// (controlled by AppSettings.refreshMinutes in main.cpp).
static const uint32_t WAKE_INTERVAL_SEC = 60;

inline void setupWakeSources(uint32_t wakeSec = WAKE_INTERVAL_SEC) {
    if (wakeSec < 5) wakeSec = 5;
    esp_sleep_enable_timer_wakeup((uint64_t)wakeSec * 1000000ULL);
#ifdef BOARD_COREINK
    // Dial press = GPIO38. Use ext0 (single-pin, active-low).
    // Ensure pull-up is enabled during deep sleep.
    rtc_gpio_pullup_en(GPIO_NUM_38);
    rtc_gpio_pulldown_dis(GPIO_NUM_38);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
#endif
#ifdef BOARD_M5STICK_CPLUS2
    // Front button = GPIO37. Ensure pull-up held during sleep.
    rtc_gpio_pullup_en(GPIO_NUM_37);
    rtc_gpio_pulldown_dis(GPIO_NUM_37);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);
#endif
}

inline void enterSleep() {
    // Cleanly shut down WiFi before deep sleep.  This matters most after a
    // slow-path refresh wake, where WiFi was active up to ~50 ms ago: if
    // esp_deep_sleep_start runs while the WiFi MAC/PHY is still tearing down,
    // the deep-sleep wake sources can be left in a corrupted state and the
    // timer wake never fires — the device appears to "stick" on the standby
    // screen and only wakes via the dial.  esp_wifi_stop() is the synchronous
    // shutdown call; the Arduino WiFi.mode(OFF) wrapper is async and returns
    // before the radio is fully down.
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true, false);
        esp_wifi_stop();
        WiFi.mode(WIFI_OFF);
        delay(100);
    }
#if IS_EINK
    // Wait for the in-flight refresh to complete, then issue POF (Power Off)
    // via setPowerSave(true).  We deliberately skip the full DSLP command:
    //   - DSLP forces a panel re-init (and thus a full-clearing flash) on the
    //     next wake, which the user sees as a black flicker every minute.
    //   - POF alone leaves the panel controller off but the bistable e-ink
    //     pixels stay latched (they hold for hours/days without driving),
    //     and on wake we can do a proper partial refresh — no flash.
    M5.Display.waitDisplay();
    M5.Display.powerSave(true);
#else
    M5.Display.sleep();
#endif
#ifdef BOARD_COREINK
    // CoreInk power latch: GPIO12 (POWER_HOLD) must stay HIGH on battery,
    // otherwise the latch releases the battery and the device fully powers
    // off during deep sleep (only G27 — which mechanically re-arms the
    // latch — will then bring it back, with a cold boot).
    // gpio_hold_en freezes the current output level (HIGH from M5.begin)
    // through deep sleep; gpio_deep_sleep_hold_en arms the hold globally.
    pinMode(12, OUTPUT);
    digitalWrite(12, HIGH);
    gpio_hold_en(GPIO_NUM_12);
    gpio_deep_sleep_hold_en();
#endif
    Serial.println("[Sleep] Sleeping now...");
    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

// ── Init ─────────────────────────────────────────────────────────────────────
inline void initDevice() {
    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    cfg.serial_baudrate = 115200;
#ifdef BOARD_COREINK
    // Re-assert power latch IMMEDIATELY on wake, before anything else can
    // glitch GPIO12 low and drop battery power.  Then release the deep-sleep
    // hold so the pin is writable again.
    pinMode(12, OUTPUT);
    digitalWrite(12, HIGH);
    gpio_hold_dis(GPIO_NUM_12);
    gpio_deep_sleep_hold_dis();
#endif
    M5.begin(cfg);
#ifdef BOARD_COREINK
    digitalWrite(12, HIGH);       // belt-and-suspenders after M5.begin
    M5.Display.setRotation(0);    // Portrait 200x200
#else
    M5.Display.setRotation(3);    // Landscape 240x135
    initG35();                    // Direct GPIO35 button init
#endif
}

} // namespace HAL
