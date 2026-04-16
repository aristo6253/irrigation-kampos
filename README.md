# ESP32 Irrigation Control System

An intelligent, battery-operated irrigation controller for ESP32 microcontrollers. Supports multi-zone valve control, flow monitoring, solar charging management, deep-sleep power optimization, and remote configuration via Firebase Realtime Database.

---

## Features

- **Multi-zone control** — up to 8 solenoid valves via 3-bit multiplexer
- **Two operation modes**
  - `Mode 0` — pressure-based: irrigates when water pressure is detected, optionally switches to timed irrigation
  - `Mode 1` — time-interval: irrigates at configured intervals during daytime hours
- **Flow monitoring** — Hall-effect sensor with pulse-counting ISR
- **Battery & solar management** — voltage monitoring, optional midday charging session
- **Deep sleep** — wakes on timer or external RTC trigger; maximizes battery life
- **Firebase integration** — real-time status updates and remote settings sync
- **RTC backup** — DS3231 external RTC keeps time across deep-sleep cycles
- **Watchdog protection** — hardware watchdog timer feed on dedicated GPIO
- **Offline fallback** — operates without network using last-known settings

---

## Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32 |
| External RTC | DS3231 (I2C — SDA: 21, SCL: 22) |
| Flow sensor | Hall-effect, GPIO 34 (FALLING interrupt) |
| Solenoid valves | Up to 8, via 3-bit mux (A: 25, B: 26, C: 27, INH: 2) |
| Main ball valve | GPIO 13 (reverse logic) |
| Voltage monitor | GPIO 35 (ADC) |
| Battery divider | GPIO 18 |
| USB / 5V control | GPIO 19 (reverse logic), GPIO 32 (PMOS) |
| Watchdog feed | GPIO 4 |
| External wake | GPIO 33 (RTC-capable) |

---

## Software Dependencies

| Library | Purpose |
|---|---|
| `Firebase_ESP_Client` | Firebase Realtime Database |
| `ArduinoJson` | JSON parsing/serialization |
| `RTClib` | DS3231 RTC communication |
| `Wire` | I2C bus |
| `WiFi` | ESP32 WiFi stack |
| `Preferences` | Non-volatile storage (NVS) |
| `esp_sleep` / `driver/rtc_io` | Deep sleep & power management |

---

## Configuration

### Credentials

Sensitive values are stored in a `.env` file (not committed):

```
FIREBASE_API_KEY = "your_firebase_api_key"
```

WiFi credentials and the Firebase database URL are currently set as constants in `system.c`. Update these before flashing:

```c
#define DATABASE_URL "your-db.firebasedatabase.app"
const char* ssid     = "your_wifi_ssid";
const char* password = "your_wifi_password";
```

### Remotely Configurable Parameters (via Firebase)

| Parameter | Description |
|---|---|
| `VOLUME_THRESHOLD` | Target irrigation volume per cycle (L) |
| `morningResumeTime` | Hour to resume irrigation (0–23) |
| `eveningPauseTime` | Hour to pause irrigation (0–23) |
| `mode` | Operation mode (0 = pressure, 1 = time-interval) |
| `includeCharging` | Enable midday solar charging session |
| `NUM_OUTPUTS` | Number of active solenoid valves (max 8) |
| `waterIntervalDays` | Days between irrigation cycles |

Settings are fetched from Firebase on each wake cycle when the `updateSettings` flag is set, and persisted to RTC memory across deep-sleep cycles.

---

## Architecture

```
Wake (timer / ext. trigger / cold boot)
        │
        ▼
Sync time (NTP → DS3231)
        │
        ▼
Fetch settings from Firebase
        │
        ▼
Evaluate watering condition
 (pressure detected? interval elapsed? daytime?)
        │
        ▼
Open valves → monitor flow → update Firebase
        │
        ▼
Enter deep sleep (calculated duration)
```

State is preserved across deep-sleep cycles using ESP32 RTC memory (`RTCData` struct).

---

## Key Timing Constants

| Constant | Value | Description |
|---|---|---|
| `valveDelay` | 6000 ms | Central ball valve open/close time |
| `updateIntrvl` | 60000 ms | Firebase update frequency during watering |
| `connDelay` | 40 s | Router wake-up delay before WiFi connect |

---

## Project Structure

```
Irrigation System/
├── system.c    # Main firmware (~1100 lines)
├── .env        # Firebase API key (not committed)
└── README.md
```

---

## Security Notes

- WiFi credentials and the Firebase API key are currently hardcoded. Move them to environment variables or a secrets manager before sharing or deploying.
- The `.env` file should be added to `.gitignore`.
