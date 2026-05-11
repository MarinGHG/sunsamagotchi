#pragma once

#include <Preferences.h>

// ─── Persistent Settings (NVS-backed) ───────────────────────────────────────

struct AppSettings {
    uint8_t  sleepMinutes;     // Deep sleep interval (1,5,10,15)
    uint8_t  refreshMinutes;   // Background refresh interval (1,3,5,10)
    bool     use24h;           // 24h time format
    uint32_t activeTimeoutMs;  // Auto-sleep timeout in ms (30000,60000,120000)
    bool     introSeen;        // First-run intro/tutorial dismissed
};

// Allowed values for cycling
static const uint8_t  SLEEP_OPTIONS[]   = { 1, 5, 10, 15 };
static const uint8_t  REFRESH_OPTIONS[] = { 1, 3, 5, 10 };
static const uint32_t TIMEOUT_OPTIONS[] = { 30000, 60000, 120000, 180000, 300000 };
static const uint8_t  SLEEP_OPT_COUNT   = 4;
static const uint8_t  REFRESH_OPT_COUNT = 4;
static const uint8_t  TIMEOUT_OPT_COUNT = 5;

// Settings item IDs for the settings screen
enum SettingsItem : uint8_t {
    SETT_SLEEP_MIN = 0,
    SETT_REFRESH_MIN,
    SETT_TIME_FMT,
    SETT_ACTIVE_TIMEOUT,
    SETT_COUNT
};

namespace Settings {

static Preferences _prefs;

inline void load(AppSettings& s) {
    _prefs.begin("pager", true);  // read-only
    s.sleepMinutes    = _prefs.getUChar("sleepMin", 5);
    s.refreshMinutes  = _prefs.getUChar("refreshMin", 3);
    s.use24h          = _prefs.getBool("use24h", true);
    s.activeTimeoutMs = _prefs.getULong("activeTO", 60000);
    s.introSeen       = _prefs.getBool("introSeen", false);
    _prefs.end();
}

inline void save(const AppSettings& s) {
    _prefs.begin("pager", false);  // read-write
    _prefs.putUChar("sleepMin", s.sleepMinutes);
    _prefs.putUChar("refreshMin", s.refreshMinutes);
    _prefs.putBool("use24h", s.use24h);
    _prefs.putULong("activeTO", s.activeTimeoutMs);
    _prefs.putBool("introSeen", s.introSeen);
    _prefs.end();
}

// Cycle a setting to next value
inline void cycleNext(AppSettings& s, SettingsItem item) {
    switch (item) {
        case SETT_SLEEP_MIN: {
            for (uint8_t i = 0; i < SLEEP_OPT_COUNT; i++) {
                if (SLEEP_OPTIONS[i] == s.sleepMinutes) {
                    s.sleepMinutes = SLEEP_OPTIONS[(i + 1) % SLEEP_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        case SETT_REFRESH_MIN: {
            for (uint8_t i = 0; i < REFRESH_OPT_COUNT; i++) {
                if (REFRESH_OPTIONS[i] == s.refreshMinutes) {
                    s.refreshMinutes = REFRESH_OPTIONS[(i + 1) % REFRESH_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        case SETT_TIME_FMT:
            s.use24h = !s.use24h;
            break;
        case SETT_ACTIVE_TIMEOUT: {
            for (uint8_t i = 0; i < TIMEOUT_OPT_COUNT; i++) {
                if (TIMEOUT_OPTIONS[i] == s.activeTimeoutMs) {
                    s.activeTimeoutMs = TIMEOUT_OPTIONS[(i + 1) % TIMEOUT_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        default: break;
    }
    save(s);
}

inline void cyclePrev(AppSettings& s, SettingsItem item) {
    switch (item) {
        case SETT_SLEEP_MIN: {
            for (uint8_t i = 0; i < SLEEP_OPT_COUNT; i++) {
                if (SLEEP_OPTIONS[i] == s.sleepMinutes) {
                    s.sleepMinutes = SLEEP_OPTIONS[(i + SLEEP_OPT_COUNT - 1) % SLEEP_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        case SETT_REFRESH_MIN: {
            for (uint8_t i = 0; i < REFRESH_OPT_COUNT; i++) {
                if (REFRESH_OPTIONS[i] == s.refreshMinutes) {
                    s.refreshMinutes = REFRESH_OPTIONS[(i + REFRESH_OPT_COUNT - 1) % REFRESH_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        case SETT_TIME_FMT:
            s.use24h = !s.use24h;
            break;
        case SETT_ACTIVE_TIMEOUT: {
            for (uint8_t i = 0; i < TIMEOUT_OPT_COUNT; i++) {
                if (TIMEOUT_OPTIONS[i] == s.activeTimeoutMs) {
                    s.activeTimeoutMs = TIMEOUT_OPTIONS[(i + TIMEOUT_OPT_COUNT - 1) % TIMEOUT_OPT_COUNT];
                    break;
                }
            }
            break;
        }
        default: break;
    }
    save(s);
}

inline void markIntroSeen(AppSettings& s) {
    s.introSeen = true;
    save(s);
}

// Format a setting value as display string
inline void formatValue(char* out, size_t sz, const AppSettings& s, SettingsItem item) {
    switch (item) {
        case SETT_SLEEP_MIN:
            snprintf(out, sz, "%d min", s.sleepMinutes);
            break;
        case SETT_REFRESH_MIN:
            snprintf(out, sz, "%d min", s.refreshMinutes);
            break;
        case SETT_TIME_FMT:
            snprintf(out, sz, "%s", s.use24h ? "24h" : "12h");
            break;
        case SETT_ACTIVE_TIMEOUT:
            snprintf(out, sz, "%lus", (unsigned long)(s.activeTimeoutMs / 1000));
            break;
        default:
            out[0] = '\0';
            break;
    }
}

inline const char* itemLabel(SettingsItem item) {
    switch (item) {
        case SETT_SLEEP_MIN:     return "Sleep interval";
        case SETT_REFRESH_MIN:   return "Refresh interval";
        case SETT_TIME_FMT:      return "Time format";
        case SETT_ACTIVE_TIMEOUT: return "Active timeout";
        default: return "";
    }
}

} // namespace Settings
