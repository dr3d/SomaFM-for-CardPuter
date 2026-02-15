#pragma once

// ──────────────────────────────────────────────────────────
// WiFi Configuration  (EDIT THESE!)
// ──────────────────────────────────────────────────────────
#define WIFI_SSID       "your_ssid"
#define WIFI_PASS       "your_password"

// ──────────────────────────────────────────────────────────
// Audio Stream Settings
// ──────────────────────────────────────────────────────────
// Format: "mp3" (most compatible) or "aac"
// Bitrate: 128 for mp3, 64 for aac (lower = less bandwidth)
#define STREAM_FORMAT   "mp3"
#define STREAM_BITRATE  128

// ──────────────────────────────────────────────────────────
// Player Settings
// ──────────────────────────────────────────────────────────
#define DEFAULT_VOLUME  100     // 0-255
#define MAX_STATIONS    50
#define AUDIO_BUF_SIZE  4096    // HTTP stream buffer (bytes)
