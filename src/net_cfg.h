#pragma once

#include <Preferences.h>

// ─── Network / Device Config (NVS-backed, namespace "cfg") ──────────────────
//
// Written by the web flasher as a separate NVS partition at 0x9000.
// Falls back to config.h compile-time values if the NVS key is absent
// (useful for local dev builds without going through the web flasher).

#include "config.h"  // compile-time fallback values

struct NetCfgData {
    char    ssid[64];
    char    pass[64];
    char    token[512];
    int32_t tzSec;
    bool    valid;  // false when NVS is blank (device not yet configured)
};

namespace NetCfg {

static Preferences _prefs;
static NetCfgData  _data;
static bool        _loaded = false;

inline const NetCfgData& data() {
    if (_loaded) return _data;

    _prefs.begin("cfg", true);  // read-only
    _prefs.getString("ssid",  _data.ssid,  sizeof(_data.ssid));
    _prefs.getString("pass",  _data.pass,  sizeof(_data.pass));
    _prefs.getString("token", _data.token, sizeof(_data.token));
    _data.tzSec = _prefs.getInt("tz", INT32_MIN);
    _prefs.end();

    // Fall back to compile-time config.h values when NVS is blank.
    if (_data.ssid[0] == '\0') {
        strncpy(_data.ssid, WIFI_SSID, sizeof(_data.ssid) - 1);
    }
    if (_data.pass[0] == '\0') {
        strncpy(_data.pass, WIFI_PASS, sizeof(_data.pass) - 1);
    }
    if (_data.token[0] == '\0') {
        strncpy(_data.token, AUTH_TOKEN, sizeof(_data.token) - 1);
    }
    if (_data.tzSec == INT32_MIN) {
        _data.tzSec = TZ_OFFSET_SEC;
    }

    // "valid" means we have at least an SSID (either from NVS or config.h).
    _data.valid = (_data.ssid[0] != '\0');
    _loaded = true;
    return _data;
}

inline const char*  ssid()  { return data().ssid;  }
inline const char*  pass()  { return data().pass;   }
inline const char*  token() { return data().token;  }
inline int32_t      tzSec() { return data().tzSec;  }
inline bool         valid() { return data().valid;  }

} // namespace NetCfg
