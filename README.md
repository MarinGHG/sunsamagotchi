# Sunsamagotchi

A tiny hardware companion for your desk that shows your current Sunsama tasks, lets you check them off, and starts a timer — all synced live with your Sunsama account.

Runs on an **M5Stack CoreInk** (200×200 e-paper) or **M5StickC Plus2** (135×240 TFT). Built with PlatformIO and Arduino.

The idea came from a simple frustration: in a world full of digital noise, sometimes you just want one small, focused thing telling you what to do next. No browser tab. No notification badge. Just a clock and a list.

Under the hood, it's a custom MCP client talking to Sunsama's MCP server over Streamable HTTP — technically a "misuse" of MCP as a REST-like API, but a respectful one. Every action on the device syncs back through the cloud to the desktop and mobile app.

---

## Hardware

| Board | Display | Colors | Notes |
|-------|---------|--------|-------|
| M5Stack CoreInk | 200×200 e-ink | Black/white | Deep sleep + persistent display |
| M5StickC Plus2 | 135×240 TFT | 16-bit color | Deep sleep + screen off |

## Screens

| Screen | What it shows |
|--------|---------------|
| Dashboard | Clock, date, plan summary, active timer, next tasks |
| Tasks | Full task list with progress bar, mark complete |
| Events | Calendar events with time and duration |
| Timer | Start/stop task timer, synced with web/mobile |
| Stats | Tasks completed, elapsed time, battery, WiFi |
| Settings | Sleep interval, refresh interval, 12/24h, active timeout |

Settings persist across reboots via NVS.

## Button Controls

### CoreInk (3 buttons)

| Button | Action | Long Press |
|--------|--------|------------|
| Dial Press (BtnA) | Next screen | Force refresh |
| Side Top (BtnB) | Scroll / cycle items | — |
| Side Bottom (BtnC) | Complete task / start-stop timer / toggle setting | — |

### StickC Plus2 (2 buttons)

| Button | Action | Long Press |
|--------|--------|------------|
| Front (BtnA) | Scroll / cycle items | Complete / start / toggle |
| Side (BtnB) | Next screen | Force refresh |

## Setup

### 1. Configure credentials

```bash
cp src/config.h.example src/config.h
```

Edit `src/config.h`:

```c
#define WIFI_SSID   "your-network"
#define WIFI_PASS   "your-password"
#define AUTH_TOKEN  "your-sunsama-api-token"
```

Get your Sunsama API token from: **Settings → Integrations → API Token**

Adjust `TZ_OFFSET_SEC` for your timezone (e.g. `3600` for UTC+1, `0` for UTC).

### 2. Build and upload

```bash
# CoreInk
pio run -e m5stack-coreink -t upload

# StickC Plus2
pio run -e m5stick-cplus2 -t upload
```

### 3. Monitor serial output

```bash
pio device monitor
```

## Architecture

```
┌──────────────────┐    HTTPS POST       ┌──────────────────┐
│  CoreInk / StickC│ ── JSON-RPC 2.0 ──> │ api.sunsama.com  │
│  ESP32           │ <─ application/json  │     /mcp         │
│  HAL abstraction │    or SSE stream     │                  │
└──────────────────┘                      └──────────────────┘
```

MCP operations used:
- `initialize` — handshake
- `resources/read` — tasks, events, timer state, plan summary
- `tools/call` — `start_task_timer`, `stop_task_timer`, `mark_task_as_completed`

## File Structure

```
src/
├── main.cpp       — setup, loop, button handling, deep sleep
├── config.h       — WiFi, auth token, timezone, defaults (not in git)
├── config.h.example
├── hal.h          — hardware abstraction (display, buttons, sleep, colors)
├── settings.h     — NVS-backed persistent settings
├── mcp_client.h   — MCP Streamable HTTP client (JSON-RPC over HTTPS)
└── ui.h           — all drawing routines, 6 screens
```

## License

AGPLv3 — Copyright 2026 Marin Benke. See [LICENSE](LICENSE).
