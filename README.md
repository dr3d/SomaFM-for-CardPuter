# SOMA FM Radio Player

Internet radio player for the **M5Stack Cardputer ADV** that streams all [SOMA FM](https://somafm.com) channels.

![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)
![Framework](https://img.shields.io/badge/framework-Arduino-green)
![License](https://img.shields.io/badge/license-MIT-yellow)

## Features

- Browse all SOMA FM stations with genre-colored list
- MP3 streaming via direct I2S to ES8311 DAC
- Station logos fetched and scaled from SOMA FM
- Now-playing track info with auto-refresh
- EQ visualizer animation
- Volume control with on-screen bar
- Quick station switching without stopping playback

## Hardware

- **M5Stack Cardputer ADV** (ESP32-S3, ES8311 codec, 240x135 IPS)
- No PSRAM required

## Controls

### Station Browser
| Key | Action |
|-----|--------|
| `w` / `;` | Scroll up |
| `s` / `.` | Scroll down |
| `q` | Page up |
| `e` | Page down |
| `,` / `/` | Volume down / up |
| `Enter` | Play station |

### Now Playing
| Key | Action |
|-----|--------|
| `G0` / `BS` | Back to browser |
| `x` | Stop playback & back |
| `;` / `.` | Previous / next station |
| `,` / `/` | Volume down / up |

## Setup

1. Copy `include/config.h` and set your WiFi credentials:
   ```cpp
   #define WIFI_SSID "your_ssid"
   #define WIFI_PASS "your_password"
   ```

2. Build and upload with PlatformIO:
   ```
   pio run -e cardputer -t upload
   ```

## Dependencies

Managed automatically by PlatformIO:

- M5Cardputer
- M5Unified
- M5GFX
- ESP8266Audio
- ArduinoJson

## Architecture

- **Core 0**: Audio decoder task (MP3 decode + I2S DMA writes)
- **Core 1**: UI rendering + input handling + network fetches
- Direct I2S output on port 1 bypasses M5.Speaker for gapless audio
- ES8311 codec initialized via I2C after M5.Speaker releases the port
