#pragma once

#include <Preferences.h>

// ─── Persistent Settings (NVS-backed) ───────────────────────────────────────

// Standby (deep-sleep) screen layouts
enum StandbyScreen : uint8_t {
    STANDBY_DEFAULT  = 0,  // Clock-focused summary (original)
    STANDBY_CALENDAR = 1,  // Sunsama-style day timeline (±3h)
    STANDBY_OPT_COUNT
};

// OTA update channel — which manifest.json the device checks against.
enum OtaChannel : uint8_t {
    OTA_OFF    = 0,  // never check
    OTA_BETA   = 1,  // preview.sunsamagotchi.pages.dev — matches firmware `beta` tier
    OTA_STABLE = 2,  // sunsamagotchi.marinbenke.dev — matches firmware `master` tier
    OTA_CHANNEL_COUNT
};

struct AppSettings {
    uint8_t  sleepMinutes;     // Deep sleep interval (1,5,10,15)
    uint8_t  refreshMinutes;   // Background refresh interval (1,3,5,10)
    bool     use24h;           // 24h time format
    uint32_t activeTimeoutMs;  // Auto-sleep timeout in ms (30000,60000,120000)
    uint8_t  standbyScreen;    // Standby layout (STANDBY_DEFAULT / STANDBY_CALENDAR)
    bool     introSeen;        // First-run intro/tutorial dismissed
    uint8_t  otaChannel;       // OtaChannel — OTA_OFF disables checks entirely
    char     otaSkipTag[24];   // Version tag the user chose to never be notified about again
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
    SETT_STANDBY,
    SETT_ACTIVE_TIMEOUT,
    SETT_OTA_CHANNEL,
    SETT_OTA_CHECK_NOW,  // action row — SELECT triggers an immediate manifest check
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
    s.standbyScreen   = _prefs.getUChar("standby", STANDBY_DEFAULT);
    s.introSeen       = _prefs.getBool("introSeen", false);
    s.otaChannel      = _prefs.getUChar("otaChan", OTA_OFF);
    _prefs.getString("otaSkip", s.otaSkipTag, sizeof(s.otaSkipTag));
    _prefs.end();
}

inline void save(const AppSettings& s) {
    _prefs.begin("pager", false);  // read-write
    _prefs.putUChar("sleepMin", s.sleepMinutes);
    _prefs.putUChar("refreshMin", s.refreshMinutes);
    _prefs.putBool("use24h", s.use24h);
    _prefs.putULong("activeTO", s.activeTimeoutMs);
    _prefs.putUChar("standby", s.standbyScreen);
    _prefs.putBool("introSeen", s.introSeen);
    _prefs.putUChar("otaChan", s.otaChannel);
    _prefs.putString("otaSkip", s.otaSkipTag);
    _prefs.end();
}

// Persist just the skipped-version tag without rewriting every other key —
// called from the OTA "never notify for this version" action.
inline void saveOtaSkipTag(AppSettings& s, const char* tag) {
    strlcpy(s.otaSkipTag, tag, sizeof(s.otaSkipTag));
    _prefs.begin("pager", false);
    _prefs.putString("otaSkip", s.otaSkipTag);
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
        case SETT_STANDBY:
            s.standbyScreen = (s.standbyScreen + 1) % STANDBY_OPT_COUNT;
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
        case SETT_OTA_CHANNEL:
            s.otaChannel = (s.otaChannel + 1) % OTA_CHANNEL_COUNT;
            break;
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
        case SETT_STANDBY:
            s.standbyScreen = (s.standbyScreen + STANDBY_OPT_COUNT - 1) % STANDBY_OPT_COUNT;
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
        case SETT_OTA_CHANNEL:
            s.otaChannel = (s.otaChannel + OTA_CHANNEL_COUNT - 1) % OTA_CHANNEL_COUNT;
            break;
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
        case SETT_STANDBY:
            snprintf(out, sz, "%s", s.standbyScreen == STANDBY_CALENDAR ? "Calendar" : "Clock");
            break;
        case SETT_ACTIVE_TIMEOUT:
            snprintf(out, sz, "%lus", (unsigned long)(s.activeTimeoutMs / 1000));
            break;
        case SETT_OTA_CHANNEL:
            snprintf(out, sz, "%s", s.otaChannel == OTA_STABLE ? "Stable"
                                    : s.otaChannel == OTA_BETA  ? "Beta" : "Off");
            break;
        case SETT_OTA_CHECK_NOW:
            snprintf(out, sz, "%s", "Press SELECT");
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
        case SETT_STANDBY:       return "Standby screen";
        case SETT_ACTIVE_TIMEOUT: return "Active timeout";
        case SETT_OTA_CHANNEL:   return "OTA updates";
        case SETT_OTA_CHECK_NOW: return "Check for update";
        default: return "";
    }
}

} // namespace Settings
