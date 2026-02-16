/**************************************************************
 * SOMA FM Internet Radio Player
 * For M5Stack Cardputer ADV (ESP32-S3 / ES8311 codec)
 *
 * Controls (Browser):
 *   w/;  = Scroll Up       s/.  = Scroll Down
 *   q    = Page Up          e    = Page Down
 *   ,    = Volume Down      /    = Volume Up
 *   Enter = Play station
 *
 * Controls (Now Playing):
 *   BS   = Back to browser  x    = Stop & back
 *   ,    = Volume Down      /    = Volume Up
 *   .    = Next station     ;    = Previous station
 **************************************************************/

#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>
#include <algorithm>
#include <Preferences.h>
#include <LittleFS.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════
//  COLOR PALETTE (RGB565)
// ═══════════════════════════════════════════════════════════
#define C_BG         0x0000
#define C_BG_DARK    0x0841
#define C_HEADER1    0xA000
#define C_HEADER2    0xF800
#define C_ACCENT     0xFD20
#define C_WHITE      0xFFFF
#define C_GRAY       0x7BEF
#define C_DARKGRAY   0x4208
#define C_SELECT     0x0339
#define C_PLAYING    0x07E0
#define C_AMBIENT    0x0479
#define C_ELECTRONIC 0x781F
#define C_ROCK       0xFB00
#define C_JAZZ       0xFE60
#define C_WORLD      0x2589
#define C_LOUNGE     0xF81F
#define C_FOLK       0xC460
#define C_METAL      0xA000
#define C_REGGAE     0x0600
#define C_HIPHOP     0xB5B6
#define C_INDIE      0xE71C
#define C_NEWS       0x867F
#define C_SPECIAL    0xFFE0

// ═══════════════════════════════════════════════════════════
//  LAYOUT CONSTANTS
// ═══════════════════════════════════════════════════════════
#define SCREEN_W      240
#define SCREEN_H      135
#define HEADER_H      22
#define FOOTER_H      16
#define CONTENT_Y     HEADER_H
#define CONTENT_H     (SCREEN_H - HEADER_H - FOOTER_H)
#define LINE_H        15
#define VISIBLE_LINES (CONTENT_H / LINE_H)

// ═══════════════════════════════════════════════════════════
//  DATA STRUCTURES
// ═══════════════════════════════════════════════════════════
struct Station {
    String id;
    String title;
    String desc;
    String genre;
    String imageUrl;
    uint16_t color;
    int listeners;
    bool fav;
};

enum AppState {
    STATE_BOOT,
    STATE_BROWSER,
    STATE_PLAYING,
    STATE_ERROR
};

// Audio commands (single atomic variable eliminates race conditions)
#define ACMD_NONE 0
#define ACMD_STOP 1
#define ACMD_PLAY 2

// ═══════════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════════
Station   stations[MAX_STATIONS];
int       stationCount  = 0;
int       selectedIdx   = 0;
int       scrollOffset  = 0;
int       playingIdx    = -1;
uint8_t   volume        = DEFAULT_VOLUME;
AppState  appState      = STATE_BOOT;
bool      needsRefresh  = false;   // deferred network refresh after cached boot
String    nowTrack      = "";
String    errorMsg      = "";

M5Canvas  canvas(&M5.Display);

// Audio pipeline  (Direct I2S → ES8311 codec)
AudioOutput                  *audioOut    = nullptr;
AudioFileSourceHTTPStream    *audioSrc    = nullptr;
AudioFileSourceBuffer        *audioBuf    = nullptr;
AudioGeneratorMP3            *mp3         = nullptr;

// Audio task
TaskHandle_t  audioTaskH  = nullptr;
volatile bool aRunning    = false;
volatile bool aPaused     = false;
volatile int  aCmd        = ACMD_NONE;
volatile int  aTarget     = -1;

// Timing
unsigned long tLastUI     = 0;
unsigned long tLastNP     = 0;
unsigned long tLastKey    = 0;
const unsigned long DEBOUNCE_MS   = 180;
const unsigned long UI_MS         = 66;
const unsigned long NP_MS         = 30000;

// Audio visualizer
#define VIS_OFF   0
#define VIS_BARS  1
#define VIS_WAVE  2
#define VIS_VU    3
#define VIS_COUNT 4
volatile int visMode = VIS_BARS;

// Audio data shared from Core 0 → Core 1
#define VIS_BINS   16          // number of amplitude bins for bars
#define VIS_WAVE_N 120         // waveform sample count (matches pixel width)
volatile uint16_t visPeak = 0; // overall peak amplitude (0-32767)
volatile uint8_t  visBins[VIS_BINS];   // amplitude per bin (0-255)
volatile int8_t   visWave[VIS_WAVE_N]; // waveform samples (-128..127)
volatile int      visWaveW = 0;        // write index into visWave

// Accumulator for bins (used in ConsumeSample on Core 0)
static uint32_t _binAcc[VIS_BINS];
static int      _binIdx = 0;
static int      _binCnt = 0;
static uint32_t _peakAcc = 0;
static int      _peakCnt = 0;
static int      _waveSub = 0;

// Logo cache
uint8_t *logoData    = nullptr;
size_t   logoDataLen = 0;
int      logoForIdx  = -1;
bool     logoValid   = false;

// Scroll state for car-radio text effect
struct ScrollState {
    String text;
    int    fullWidth;
    unsigned long startMs;
};
ScrollState scrTitle, scrGenre, scrSong;

// ═══════════════════════════════════════════════════════════
//  UTILITY FUNCTIONS
// ═══════════════════════════════════════════════════════════
uint16_t getGenreColor(const String &g) {
    String lc = g;
    lc.toLowerCase();
    if (lc.indexOf("ambient") >= 0 || lc.indexOf("chill") >= 0)  return C_AMBIENT;
    if (lc.indexOf("electro") >= 0)                               return C_ELECTRONIC;
    if (lc.indexOf("rock") >= 0 || lc.indexOf("altern") >= 0)    return C_INDIE;
    if (lc.indexOf("jazz") >= 0)                                  return C_JAZZ;
    if (lc.indexOf("world") >= 0 || lc.indexOf("bossa") >= 0 ||
        lc.indexOf("celtic") >= 0 || lc.indexOf("tiki") >= 0)    return C_WORLD;
    if (lc.indexOf("lounge") >= 0)                                return C_LOUNGE;
    if (lc.indexOf("folk") >= 0 || lc.indexOf("americ") >= 0)    return C_FOLK;
    if (lc.indexOf("metal") >= 0 || lc.indexOf("indust") >= 0)   return C_METAL;
    if (lc.indexOf("reggae") >= 0)                                return C_REGGAE;
    if (lc.indexOf("hip") >= 0)                                   return C_HIPHOP;
    if (lc.indexOf("oldies") >= 0 || lc.indexOf("70") >= 0 ||
        lc.indexOf("80") >= 0)                                    return C_ACCENT;
    if (lc.indexOf("pop") >= 0)                                   return C_INDIE;
    if (lc.indexOf("news") >= 0 || lc.indexOf("live") >= 0 ||
        lc.indexOf("spoken") >= 0)                                return C_NEWS;
    if (lc.indexOf("special") >= 0)                               return C_SPECIAL;
    return C_GRAY;
}

String shortGenre(const String &g) {
    int p = g.indexOf('|');
    String s = (p > 0) ? g.substring(0, p) : g;
    if (s.length() > 7) s = s.substring(0, 7);
    return s;
}

uint16_t blendRGB(uint16_t c1, uint16_t c2, uint8_t t) {
    uint8_t r1 = (c1 >> 11), g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
    uint8_t r2 = (c2 >> 11), g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
    return ((r1 + ((r2 - r1) * t >> 8)) << 11) |
           ((g1 + ((g2 - g1) * t >> 8)) << 5) |
            (b1 + ((b2 - b1) * t >> 8));
}

void drawGradient(M5Canvas &c, int y, int h, uint16_t c1, uint16_t c2) {
    for (int i = 0; i < h; i++)
        c.drawFastHLine(0, y + i, SCREEN_W, blendRGB(c1, c2, i * 255 / h));
}

bool hasKey(const std::vector<char> &word, char ch) {
    return std::find(word.begin(), word.end(), ch) != word.end();
}

// Truncate string to fit within maxPx pixels (using current canvas font)
String fitText(M5Canvas &c, const String &s, int maxPx) {
    if (c.textWidth(s) <= maxPx) return s;
    for (int len = s.length() - 1; len > 0; len--) {
        String t = s.substring(0, len) + "~";
        if (c.textWidth(t) <= maxPx) return t;
    }
    return "~";
}

// Car-radio scrolling text: scrolls if text exceeds maxW, otherwise draws normally.
// Uses TL_DATUM. Font must be set before calling.
void drawScrollText(M5Canvas &c, const String &s, int x, int y,
                    int maxW, ScrollState &ss) {
    int tw = c.textWidth(s);
    if (tw <= maxW) {
        c.drawString(s, x, y);
        ss.text = "";
        return;
    }
    // Reset scroll on text change
    if (s != ss.text) {
        ss.text      = s;
        ss.fullWidth = tw;
        ss.startMs   = millis();
    }
    unsigned long elapsed = millis() - ss.startMs;
    int pause = 2000;     // ms to show start before scrolling
    int speed = 35;       // px/sec
    int gap   = 50;       // px gap before text repeats
    int cycle = ss.fullWidth + gap;

    int offset = 0;
    if (elapsed > (unsigned long)pause) {
        offset = (int)((elapsed - pause) * speed / 1000) % cycle;
    }

    int fh = c.fontHeight();
    c.setClipRect(x, y, maxW, fh);
    c.drawString(s, x - offset, y);
    c.drawString(s, x - offset + cycle, y);
    c.clearClipRect();
}

// ═══════════════════════════════════════════════════════════
//  AUDIO OUTPUT (Direct I2S to ES8311 codec)
// ═══════════════════════════════════════════════════════════
#include <driver/i2s.h>

#define ES8311_ADDR 0x18

void es8311_init_dac() {
    // Exact register values from M5Unified _speaker_enabled_cb_cardputer_adv
    auto wr = [](uint8_t reg, uint8_t val) {
        M5.In_I2C.writeRegister8(ES8311_ADDR, reg, val, 400000);
    };
    wr(0x00, 0x80);  // CSM POWER ON
    wr(0x01, 0xB5);  // MCLK = BCLK
    wr(0x02, 0x18);  // MULT_PRE = 3
    wr(0x0D, 0x01);  // Power up analog circuitry
    wr(0x12, 0x00);  // Power-up DAC
    wr(0x13, 0x10);  // Enable output to HP drive
    wr(0x32, 0xBF);  // DAC volume 0dB
    wr(0x37, 0x08);  // Bypass DAC equalizer
}

class DirectI2SOutput : public AudioOutput {
public:
    DirectI2SOutput(i2s_port_t port, int bck, int ws, int dout)
        : _port(port), _bck(bck), _ws(ws), _dout(dout), _started(false), _bp(0) {
        hertz = 44100;
        gainF2P6 = 64;
    }

    bool begin() override {
        if (_started) return true;
        i2s_config_t cfg = {};
        cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate = hertz > 0 ? hertz : 44100;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
        cfg.dma_buf_count = 8;
        cfg.dma_buf_len = 128;
        cfg.use_apll = false;
        cfg.tx_desc_auto_clear = true;

        if (i2s_driver_install(_port, &cfg, 0, NULL) != ESP_OK) {
            Serial.println("[I2S] driver_install FAILED");
            return false;
        }
        i2s_pin_config_t pins = {};
        pins.mck_io_num   = I2S_PIN_NO_CHANGE;
        pins.bck_io_num   = _bck;
        pins.ws_io_num    = _ws;
        pins.data_out_num = _dout;
        pins.data_in_num  = I2S_PIN_NO_CHANGE;
        i2s_set_pin(_port, &pins);
        _started = true;
        Serial.printf("[I2S] port %d  bck=%d ws=%d dout=%d\n", _port, _bck, _ws, _dout);
        return true;
    }

    bool stop() override {
        _bp = 0;
        if (_started) i2s_zero_dma_buffer(_port);
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (aCmd != ACMD_NONE) return false;

        int16_t mono;
        if (aPaused) {
            mono = 0;
        } else {
            mono = ((int32_t)sample[LEFTCHANNEL] + sample[RIGHTCHANNEL]) / 2;
            mono = (int16_t)(((int32_t)mono * gainF2P6) >> 6);
        }
        _buf[_bp++] = mono;  // L
        _buf[_bp++] = mono;  // R

        // Feed visualizer (cheap — just track amplitude)
        uint16_t absMono = (mono < 0) ? -mono : mono;
        _peakAcc += absMono;
        _peakCnt++;
        _binAcc[_binIdx] += absMono;
        _binCnt++;
        // ~44100/VIS_BINS/30 ≈ 92 samples per bin per frame at 30fps
        if (_binCnt >= 92) {
            visBins[_binIdx] = min(255u, (uint32_t)(_binAcc[_binIdx] / _binCnt) >> 5);
            _binAcc[_binIdx] = 0;
            _binCnt = 0;
            _binIdx = (_binIdx + 1) % VIS_BINS;
        }
        if (_peakCnt >= 735) {  // ~60fps peak update (44100/60)
            visPeak = min(32767u, (uint32_t)(_peakAcc / _peakCnt));
            _peakAcc = 0;
            _peakCnt = 0;
        }
        // Waveform: downsample to VIS_WAVE_N samples per ~30ms window
        // 44100/VIS_WAVE_N/33 ≈ 11 samples between captures
        _waveSub++;
        if (_waveSub >= 11) {
            _waveSub = 0;
            visWave[visWaveW] = (int8_t)(mono >> 8);  // 16-bit → 8-bit
            visWaveW = (visWaveW + 1) % VIS_WAVE_N;
        }

        if (_bp >= BUF_SZ) {
            size_t written = 0;
            i2s_write(_port, _buf, _bp * sizeof(int16_t), &written, pdMS_TO_TICKS(50));
            _bp = 0;
        }
        return true;
    }

    bool SetRate(int hz) override {
        hertz = hz;
        if (_started) i2s_set_sample_rates(_port, hz);
        return true;
    }

    bool SetBitsPerSample(int bits) override { return (bits == 16); }
    bool SetChannels(int ch) override { return true; }

private:
    static const int BUF_SZ = 512;  // 256 stereo sample pairs
    int16_t _buf[BUF_SZ];
    int _bp;
    i2s_port_t _port;
    int _bck, _ws, _dout;
    bool _started;
};

// ═══════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════
bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40; i++) {
        if (WiFi.status() == WL_CONNECTED) return true;
        delay(500);
        canvas.fillSprite(C_BG);
        canvas.setTextDatum(MC_DATUM);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextColor(C_ACCENT);
        canvas.drawString("SOMA FM", SCREEN_W / 2, 40);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(C_WHITE);
        canvas.drawString("Connecting WiFi", SCREEN_W / 2, 70);
        String dots = "";
        for (int d = 0; d <= (i % 3); d++) dots += " .";
        canvas.setTextColor(C_GRAY);
        canvas.drawString(dots, SCREEN_W / 2, 90);
        canvas.setFont(&fonts::Font0);
        canvas.setTextColor(C_DARKGRAY);
        canvas.drawString(WIFI_SSID, SCREEN_W / 2, 115);
        canvas.pushSprite(0, 0);
    }
    return false;
}

// ═══════════════════════════════════════════════════════════
//  SOMA FM API
// ═══════════════════════════════════════════════════════════
bool parseChannelsJson(Stream &input) {
    DynamicJsonDocument filter(256);
    JsonObject cf = filter["channels"].createNestedObject();
    cf["id"]          = true;
    cf["title"]       = true;
    cf["description"] = true;
    cf["genre"]       = true;
    cf["image"]       = true;
    cf["listeners"]   = true;

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(
        doc, input, DeserializationOption::Filter(filter));

    if (err) {
        Serial.printf("[PARSE] JSON error: %s\n", err.c_str());
        return false;
    }

    JsonArray ch = doc["channels"];
    stationCount = 0;
    for (JsonObject o : ch) {
        if (stationCount >= MAX_STATIONS) break;
        Station &s  = stations[stationCount];
        s.id        = o["id"].as<String>();
        s.title     = o["title"].as<String>();
        s.desc      = o["description"].as<String>();
        s.genre     = o["genre"].as<String>();
        s.imageUrl  = o["image"].as<String>();
        s.listeners = o["listeners"].as<String>().toInt();
        s.color     = getGenreColor(s.genre);
        s.fav       = false;
        stationCount++;
    }
    Serial.printf("[PARSE] Loaded %d stations\n", stationCount);
    return stationCount > 0;
}

bool loadCachedChannels() {
    if (!LittleFS.exists("/channels.json")) return false;
    File f = LittleFS.open("/channels.json", "r");
    if (!f) return false;
    Serial.printf("[CACHE] Loading channels.json (%d bytes)\n", f.size());
    bool ok = parseChannelsJson(f);
    f.close();
    return ok;
}

bool fetchChannels(bool showSplash = true) {
    if (showSplash) {
        canvas.fillSprite(C_BG);
        canvas.setTextDatum(MC_DATUM);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextColor(C_ACCENT);
        canvas.drawString("SOMA FM", SCREEN_W / 2, 40);
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(C_WHITE);
        canvas.drawString("Loading stations...", SCREEN_W / 2, 75);
        canvas.pushSprite(0, 0);
    }

    Serial.printf("[FETCH] Free heap: %u\n", ESP.getFreeHeap());

    HTTPClient http;
    WiFiClient plainClient;

    Serial.println("[FETCH] Trying HTTP...");
    http.begin(plainClient, "http://somafm.com/channels.json");
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    int code = http.GET();
    Serial.printf("[FETCH] HTTP code: %d\n", code);

    if (code != 200) {
        http.end();
        // Fallback to HTTPS
        WiFiClientSecure secClient;
        secClient.setInsecure();
        http.begin(secClient, "https://somafm.com/channels.json");
        http.setTimeout(15000);
        code = http.GET();
        Serial.printf("[FETCH] HTTPS code: %d\n", code);
    }

    if (code != 200) {
        http.end();
        errorMsg = "HTTP " + String(code);
        return false;
    }

    Serial.printf("[FETCH] Got 200, heap: %u\n", ESP.getFreeHeap());

    // Read full response into a String so we can both parse and cache it
    String payload = http.getString();
    http.end();

    // Save to flash cache
    File f = LittleFS.open("/channels.json", "w");
    if (f) {
        f.print(payload);
        f.close();
        Serial.printf("[CACHE] Saved channels.json (%d bytes)\n", payload.length());
    }

    // Parse from the saved string
    // ArduinoJson can deserialize from a const char*
    DynamicJsonDocument filter(256);
    JsonObject cf = filter["channels"].createNestedObject();
    cf["id"]          = true;
    cf["title"]       = true;
    cf["description"] = true;
    cf["genre"]       = true;
    cf["image"]       = true;
    cf["listeners"]   = true;

    DynamicJsonDocument doc(32768);
    DeserializationError err = deserializeJson(
        doc, payload, DeserializationOption::Filter(filter));

    if (err) {
        errorMsg = String("JSON: ") + err.c_str();
        return false;
    }

    JsonArray ch = doc["channels"];
    stationCount = 0;
    for (JsonObject o : ch) {
        if (stationCount >= MAX_STATIONS) break;
        Station &s  = stations[stationCount];
        s.id        = o["id"].as<String>();
        s.title     = o["title"].as<String>();
        s.desc      = o["description"].as<String>();
        s.genre     = o["genre"].as<String>();
        s.imageUrl  = o["image"].as<String>();
        s.listeners = o["listeners"].as<String>().toInt();
        s.color     = getGenreColor(s.genre);
        s.fav       = false;
        stationCount++;
    }
    Serial.printf("[FETCH] Loaded %d stations\n", stationCount);
    return stationCount > 0;
}

// ═══════════════════════════════════════════════════════════
//  FAVORITES (persisted to NVS)
// ═══════════════════════════════════════════════════════════
Preferences prefs;

void loadFavorites() {
    prefs.begin("somafm", true);
    String favs = prefs.getString("favs", "");
    prefs.end();
    for (int i = 0; i < stationCount; i++)
        stations[i].fav = (favs.indexOf(stations[i].id) >= 0);
    Serial.printf("[FAV] Loaded: %s\n", favs.c_str());
}

void saveFavorites() {
    String favs = "";
    for (int i = 0; i < stationCount; i++) {
        if (stations[i].fav) {
            if (favs.length() > 0) favs += ",";
            favs += stations[i].id;
        }
    }
    prefs.begin("somafm", false);
    prefs.putString("favs", favs);
    prefs.end();
}

void saveLastStation() {
    if (selectedIdx >= 0 && selectedIdx < stationCount) {
        prefs.begin("somafm", false);
        prefs.putString("last", stations[selectedIdx].id);
        prefs.end();
    }
}

void ensureVisible() {
    int maxOff = max(0, stationCount - (int)VISIBLE_LINES);
    scrollOffset = max(0, min(selectedIdx - (int)VISIBLE_LINES / 2, maxOff));
}

void restoreLastStation() {
    prefs.begin("somafm", true);
    String lastId = prefs.getString("last", "");
    prefs.end();
    if (lastId.length() == 0) return;
    for (int i = 0; i < stationCount; i++) {
        if (stations[i].id == lastId) {
            selectedIdx = i;
            ensureVisible();
            Serial.printf("[LAST] Restored: %s (idx %d)\n", lastId.c_str(), i);
            return;
        }
    }
}

void sortStations() {
    // Remember which stations are selected/playing by ID
    String selId  = (selectedIdx >= 0 && selectedIdx < stationCount)
                    ? stations[selectedIdx].id : "";
    String playId = (playingIdx >= 0 && playingIdx < stationCount)
                    ? stations[playingIdx].id : "";

    // Stable sort: favorites first, original order preserved within groups
    std::stable_sort(stations, stations + stationCount,
        [](const Station &a, const Station &b) { return a.fav && !b.fav; });

    // Restore indices to follow the moved stations
    for (int i = 0; i < stationCount; i++) {
        if (selId.length()  && stations[i].id == selId)  selectedIdx = i;
        if (playId.length() && stations[i].id == playId) playingIdx  = i;
    }
    ensureVisible();
}

void toggleFavorite(int idx) {
    if (idx < 0 || idx >= stationCount) return;
    stations[idx].fav = !stations[idx].fav;
    saveFavorites();
    sortStations();
}

void fetchNowPlaying() {
    if (playingIdx < 0 || playingIdx >= stationCount) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://somafm.com/songs/" + stations[playingIdx].id + ".json";
    http.begin(client, url);
    http.setTimeout(5000);
    if (http.GET() == 200) {
        DynamicJsonDocument doc(4096);
        if (!deserializeJson(doc, http.getStream())) {
            JsonArray songs = doc["songs"];
            if (songs.size() > 0) {
                String a = songs[0]["artist"].as<String>();
                String t = songs[0]["title"].as<String>();
                nowTrack = a + " - " + t;
            }
        }
    }
    http.end();
}

// ═══════════════════════════════════════════════════════════
//  LOGO DOWNLOAD & CACHE
// ═══════════════════════════════════════════════════════════
void freeLogo() {
    if (logoData) { free(logoData); logoData = nullptr; }
    logoDataLen = 0;
    logoForIdx  = -1;
    logoValid   = false;
}

String logoCachePath(int stationIdx) {
    return "/logos/" + stations[stationIdx].id + ".img";
}

bool loadCachedLogo(int stationIdx) {
    String path = logoCachePath(stationIdx);
    if (!LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    int len = f.size();
    if (len <= 0 || len > 25000) { f.close(); return false; }
    logoData = (uint8_t *)malloc(len);
    if (!logoData) { f.close(); return false; }
    size_t read = f.readBytes((char *)logoData, len);
    f.close();
    if ((int)read == len) {
        logoDataLen = len;
        logoForIdx  = stationIdx;
        logoValid   = true;
        Serial.printf("[LOGO] Cache hit: %s (%d bytes)\n", path.c_str(), len);
        return true;
    }
    free(logoData); logoData = nullptr;
    return false;
}

void saveCachedLogo(int stationIdx) {
    String path = logoCachePath(stationIdx);
    File f = LittleFS.open(path, "w");
    if (f) {
        f.write(logoData, logoDataLen);
        f.close();
        Serial.printf("[LOGO] Cached: %s (%d bytes)\n", path.c_str(), logoDataLen);
    }
}

void downloadLogo(int stationIdx) {
    if (stationIdx == logoForIdx && logoValid) return;  // already in RAM
    freeLogo();
    if (stationIdx < 0 || stationIdx >= stationCount) return;

    // Check flash cache first
    if (loadCachedLogo(stationIdx)) return;

    String url = stations[stationIdx].imageUrl;
    if (url.length() == 0) return;

    // Try HTTP version of the image URL (less RAM than HTTPS)
    url.replace("https://", "http://");

    Serial.printf("[LOGO] Downloading: %s\n", url.c_str());

    HTTPClient http;
    WiFiClient client;
    http.begin(client, url);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(5000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[LOGO] HTTP %d\n", code);
        http.end();
        logoForIdx = stationIdx;  // don't retry
        return;
    }

    int len = http.getSize();
    if (len <= 0 || len > 25000) {
        Serial.printf("[LOGO] Bad size: %d\n", len);
        http.end();
        logoForIdx = stationIdx;
        return;
    }

    logoData = (uint8_t *)malloc(len);
    if (!logoData) {
        Serial.println("[LOGO] malloc failed");
        http.end();
        logoForIdx = stationIdx;
        return;
    }

    size_t read = http.getStream().readBytes(logoData, len);
    http.end();

    if ((int)read == len) {
        logoDataLen = len;
        logoForIdx  = stationIdx;
        logoValid   = true;
        Serial.printf("[LOGO] OK %d bytes, heap=%u\n", len, ESP.getFreeHeap());
        saveCachedLogo(stationIdx);  // persist to flash
    } else {
        Serial.printf("[LOGO] Read mismatch: %d/%d\n", read, len);
        freeLogo();
        logoForIdx = stationIdx;
    }
}

// ═══════════════════════════════════════════════════════════
//  UI COMPONENTS
// ═══════════════════════════════════════════════════════════
void drawBattery(int x, int y) {
    int bw = 18, bh = 10, nub = 2;
    int level = M5.Power.getBatteryLevel();  // 0-100
    bool charging = M5.Power.isCharging();
    // Body outline
    canvas.drawRect(x, y, bw, bh, C_GRAY);
    // Nub on right
    canvas.fillRect(x + bw, y + 3, nub, 4, C_GRAY);
    // Fill level
    int fw = (bw - 4) * level / 100;
    uint16_t fc = (level > 50) ? C_PLAYING : (level > 20) ? C_ACCENT : C_HEADER2;
    if (charging) fc = C_PLAYING;
    if (fw > 0) canvas.fillRect(x + 2, y + 2, fw, bh - 4, fc);
}

void drawHeader(const char *title, const char *right = nullptr) {
    drawGradient(canvas, 0, HEADER_H, C_HEADER1, C_HEADER2);
    canvas.setTextDatum(ML_DATUM);
    canvas.setTextColor(C_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(title, 6, HEADER_H / 2);
    // Battery gauge (right edge)
    drawBattery(SCREEN_W - 24, 6);
    if (right) {
        canvas.setTextDatum(MR_DATUM);
        canvas.setTextColor(C_GRAY);
        canvas.setFont(&fonts::Font0);
        canvas.drawString(right, SCREEN_W - 28, HEADER_H / 2);
    }
    canvas.drawFastHLine(0, HEADER_H - 1, SCREEN_W, C_ACCENT);
}

// Draw a small 5px triangle arrow at (cx,cy) center
void drawArrow(int cx, int cy, int dir, uint16_t col) {
    // dir: 0=up, 1=down, 2=left, 3=right
    int s = 3; // half-size
    switch (dir) {
        case 0: canvas.fillTriangle(cx, cy-s, cx-s, cy+s, cx+s, cy+s, col); break;
        case 1: canvas.fillTriangle(cx, cy+s, cx-s, cy-s, cx+s, cy-s, col); break;
        case 2: canvas.fillTriangle(cx-s, cy, cx+s, cy-s, cx+s, cy+s, col); break;
        case 3: canvas.fillTriangle(cx+s, cy, cx-s, cy-s, cx-s, cy+s, col); break;
    }
}

void drawFooterBrowser() {
    int y = SCREEN_H - FOOTER_H;
    int cy = y + FOOTER_H / 2 + 1;
    canvas.fillRect(0, y, SCREEN_W, FOOTER_H, C_BG_DARK);
    canvas.drawFastHLine(0, y, SCREEN_W, C_DARKGRAY);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_GRAY);
    canvas.setTextDatum(ML_DATUM);
    // up/down :Nav
    int x = 4;
    drawArrow(x + 2, cy, 0, C_GRAY); drawArrow(x + 10, cy, 1, C_GRAY);
    canvas.drawString(":Nav", x + 16, cy);
    // Enter:Play
    x = 60;
    canvas.drawString("Enter:Play", x, cy);
    // f:Fav
    x = 144;
    canvas.drawString("f:Fav", x, cy);
    // left/right :Vol
    x = 186;
    drawArrow(x + 2, cy, 2, C_GRAY); drawArrow(x + 12, cy, 3, C_GRAY);
    canvas.drawString(":Vol", x + 18, cy);
}

void drawFooterPlayer() {
    int y = SCREEN_H - FOOTER_H;
    int cy = y + FOOTER_H / 2 + 1;
    canvas.fillRect(0, y, SCREEN_W, FOOTER_H, C_BG_DARK);
    canvas.drawFastHLine(0, y, SCREEN_W, C_DARKGRAY);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_GRAY);
    canvas.setTextDatum(ML_DATUM);
    // BS:Back
    int x = 4;
    canvas.drawString("BS:Back", x, cy);
    // left/right :Vol
    x = 62;
    drawArrow(x + 2, cy, 2, C_GRAY); drawArrow(x + 12, cy, 3, C_GRAY);
    canvas.drawString(":Vol", x + 18, cy);
    // f:Fav
    x = 120;
    canvas.drawString("f:Fav", x, cy);
    // up/down :Skip
    x = 160;
    drawArrow(x + 2, cy, 0, C_GRAY); drawArrow(x + 10, cy, 1, C_GRAY);
    canvas.drawString(":Skip", x + 16, cy);
}

void drawFooter(const char *text) {
    int y = SCREEN_H - FOOTER_H;
    canvas.fillRect(0, y, SCREEN_W, FOOTER_H, C_BG_DARK);
    canvas.drawFastHLine(0, y, SCREEN_W, C_DARKGRAY);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(C_GRAY);
    canvas.setFont(&fonts::Font0);
    canvas.drawString(text, SCREEN_W / 2, y + FOOTER_H / 2 + 1);
}

void drawVolumeBar(int x, int y, int w, int h) {
    canvas.drawRoundRect(x, y, w, h, 2, C_DARKGRAY);
    int fw = map(volume, 0, 255, 0, w - 4);
    uint16_t vc = (volume > 200) ? C_HEADER2 : C_ACCENT;
    if (fw > 0) canvas.fillRoundRect(x + 2, y + 2, fw, h - 4, 1, vc);
}

void drawEqBars(int x, int y, int w, int h) {
    // Mini EQ in header — driven by real audio data
    int bw = (w - 4) / 5;
    for (int i = 0; i < 5; i++) {
        int bh = visBins[i * 3] * h / 255;
        if (bh < 1) bh = 1;
        uint16_t c = blendRGB(C_PLAYING, C_ACCENT, i * 50);
        canvas.fillRect(x + i * (bw + 1), y + h - bh, bw, bh, c);
    }
}

void drawVisBars(int x, int y, int w, int h, uint16_t color) {
    int bw = max(2, (w - VIS_BINS + 1) / VIS_BINS);
    int gap = 1;
    int totalW = VIS_BINS * (bw + gap) - gap;
    int ox = x + (w - totalW) / 2;
    for (int i = 0; i < VIS_BINS; i++) {
        int bh = visBins[i] * h / 255;
        if (bh < 1) bh = 1;
        uint16_t c = blendRGB(color, C_ACCENT, i * 255 / VIS_BINS);
        canvas.fillRect(ox + i * (bw + gap), y + h - bh, bw, bh, c);
    }
}

void drawVisWave(int x, int y, int w, int h, uint16_t color) {
    int mid = y + h / 2;
    int r = visWaveW;  // read from current write position (oldest sample)
    int step = max(1, VIS_WAVE_N / w);
    int prevY = mid;
    for (int px = 0; px < w; px++) {
        int idx = (r + px * step) % VIS_WAVE_N;
        int sy = mid - (visWave[idx] * h / 256);
        sy = max(y, min(y + h - 1, sy));
        if (px > 0) {
            // Draw line between points
            int y0 = min(prevY, sy), y1 = max(prevY, sy);
            canvas.drawFastVLine(x + px, y0, y1 - y0 + 1, color);
        }
        prevY = sy;
    }
}

void drawVisVU(int x, int y, int w, int h, uint16_t color) {
    static int peakHold = 0;
    static unsigned long peakTime = 0;
    int level = min(w, (int)(visPeak * w / 8000));
    if (level > peakHold) { peakHold = level; peakTime = millis(); }
    if (millis() - peakTime > 800) { peakHold = max(0, peakHold - 2); }
    // Background
    canvas.fillRect(x, y, w, h, C_BG_DARK);
    // Green/yellow/red segments
    int seg1 = w * 60 / 100, seg2 = w * 85 / 100;
    if (level > 0) {
        int g = min(level, seg1);
        canvas.fillRect(x, y, g, h, C_PLAYING);
    }
    if (level > seg1) {
        int yw = min(level, seg2) - seg1;
        canvas.fillRect(x + seg1, y, yw, h, C_ACCENT);
    }
    if (level > seg2) {
        canvas.fillRect(x + seg2, y, level - seg2, h, C_HEADER2);
    }
    // Peak hold marker
    if (peakHold > 2) {
        canvas.fillRect(x + peakHold - 2, y, 2, h, C_WHITE);
    }
}

void drawVisualizer(int x, int y, int w, int h, uint16_t color) {
    switch (visMode) {
        case VIS_BARS: drawVisBars(x, y, w, h, color); break;
        case VIS_WAVE: drawVisWave(x, y, w, h, color); break;
        case VIS_VU:   drawVisVU(x, y, w, h, color); break;
        default: break;
    }
}

void drawLogoBox(int x, int y, int sz, const Station &st) {
    canvas.fillRoundRect(x, y, sz, sz, 6, st.color);
    canvas.drawRoundRect(x, y, sz, sz, 6, blendRGB(st.color, C_BG, 100));
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(C_WHITE);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    String ini = st.title.substring(0, 1);
    ini.toUpperCase();
    canvas.drawString(ini, x + sz / 2, y + sz / 2 + 1);
}

void drawLogo(int x, int y, int sz, int stationIdx) {
    Station &st = stations[stationIdx];
    if (logoValid && logoForIdx == stationIdx && logoData) {
        float sc = (float)sz / 120.0f;  // SOMA FM logos are 120x120
        String url = st.imageUrl;
        if (url.endsWith(".jpg") || url.endsWith(".jpeg")) {
            canvas.drawJpg(logoData, logoDataLen, x, y, sz, sz, 0, 0, sc, sc);
        } else {
            canvas.drawPng(logoData, logoDataLen, x, y, sz, sz, 0, 0, sc, sc);
        }
    } else {
        drawLogoBox(x, y, sz, st);
    }
}

// ═══════════════════════════════════════════════════════════
//  SCREEN: STATION BROWSER
// ═══════════════════════════════════════════════════════════
void drawBrowser() {
    canvas.fillSprite(C_BG);

    String hr = String(stationCount) + " stations";
    drawHeader("SOMA FM", hr.c_str());

    canvas.setFont(&fonts::Font2);
    int vis = min((int)VISIBLE_LINES, stationCount - scrollOffset);

    for (int i = 0; i < vis; i++) {
        int idx = scrollOffset + i;
        int y   = CONTENT_Y + i * LINE_H;
        bool sel = (idx == selectedIdx);
        bool playing = (idx == playingIdx);

        if (sel) {
            for (int j = 0; j < LINE_H; j++) {
                uint16_t c = blendRGB(stations[idx].color, C_BG, j * 200 / LINE_H + 55);
                canvas.drawFastHLine(0, y + j, SCREEN_W, c);
            }
        }

        if (playing) {
            canvas.fillCircle(5, y + LINE_H / 2, 2, C_PLAYING);
        }
        if (stations[idx].fav) {
            // Gold star for favorites
            int sx = playing ? 12 : 5, sy = y + LINE_H / 2;
            canvas.setFont(&fonts::Font0);
            canvas.setTextDatum(MC_DATUM);
            canvas.setTextColor(C_ACCENT);
            canvas.drawString("*", sx, sy);
        }

        int nameX = 18;  // leave room for indicators
        canvas.setFont(&fonts::Font2);
        canvas.setTextDatum(ML_DATUM);
        canvas.setTextColor(sel ? C_WHITE : (playing ? C_PLAYING : C_GRAY));
        canvas.drawString(fitText(canvas, stations[idx].title, SCREEN_W - 66), nameX, y + LINE_H / 2);

        canvas.setFont(&fonts::Font0);
        canvas.setTextDatum(MR_DATUM);
        canvas.setTextColor(stations[idx].color);
        canvas.drawString(shortGenre(stations[idx].genre), SCREEN_W - 5, y + LINE_H / 2);
    }

    if (stationCount > (int)VISIBLE_LINES) {
        int thumbH = max(6, (int)(CONTENT_H * VISIBLE_LINES / stationCount));
        int thumbY = CONTENT_Y + (CONTENT_H - thumbH) * scrollOffset /
                     max(1, stationCount - (int)VISIBLE_LINES);
        canvas.fillRect(SCREEN_W - 2, CONTENT_Y, 2, CONTENT_H, C_BG_DARK);
        canvas.fillRect(SCREEN_W - 2, thumbY, 2, thumbH, C_ACCENT);
    }

    drawFooterBrowser();
    canvas.pushSprite(0, 0);
}

// ═══════════════════════════════════════════════════════════
//  SCREEN: NOW PLAYING
// ═══════════════════════════════════════════════════════════
void drawPlayer() {
    if (playingIdx < 0) return;
    Station &st = stations[playingIdx];

    canvas.fillSprite(C_BG);

    // Header with genre color gradient
    drawGradient(canvas, 0, HEADER_H, blendRGB(st.color, C_BG, 180), st.color);
    canvas.setTextDatum(ML_DATUM);
    canvas.setTextColor(C_WHITE);
    canvas.setFont(&fonts::Font2);
    canvas.drawString("NOW PLAYING", 6, HEADER_H / 2);
    if (aRunning) drawEqBars(SCREEN_W - 54, 4, 24, HEADER_H - 8);
    drawBattery(SCREEN_W - 24, 6);
    canvas.drawFastHLine(0, HEADER_H - 1, SCREEN_W, st.color);

    // Logo (64x64)
    int logoSz = 64;
    int logoX  = 4;
    int logoY  = CONTENT_Y + 2;
    drawLogo(logoX, logoY, logoSz, playingIdx);

    // Right pane: title, genre, listeners, status+volume
    int ix = logoX + logoSz + 6;
    int rw = SCREEN_W - ix - 4;  // available width for right pane

    canvas.setTextDatum(TL_DATUM);
    // Favorite star before title
    int titleX = ix;
    if (st.fav) {
        canvas.setFont(&fonts::Font2);
        canvas.setTextColor(C_ACCENT);
        canvas.drawString("*", ix, CONTENT_Y + 3);
        titleX += 10;
    }
    canvas.setTextColor(C_WHITE);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    drawScrollText(canvas, st.title, titleX, CONTENT_Y + 3, rw - (titleX - ix), scrTitle);

    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(st.color);
    drawScrollText(canvas, st.genre, ix, CONTENT_Y + 20, rw, scrGenre);

    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DARKGRAY);
    canvas.drawString(String(st.listeners) + " listeners", ix, CONTENT_Y + 36);

    // Status + volume bar in right pane
    canvas.setFont(&fonts::Font0);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(aPaused ? C_ACCENT : (aRunning ? C_PLAYING : C_ACCENT));
    String statusTxt = aPaused ? "PAUSED" : (aRunning ? "STREAM" : "BUFFER");
    canvas.drawString(statusTxt, ix, CONTENT_Y + 50);
    drawVolumeBar(ix + 44, CONTENT_Y + 49, rw - 48, 10);

    // Divider (full width, below logo area)
    int dy = CONTENT_Y + 68;
    canvas.drawFastHLine(4, dy, SCREEN_W - 8, C_DARKGRAY);

    int visY = dy + 3;
    int visH = SCREEN_H - FOOTER_H - visY - 2;

    if (visMode == VIS_OFF) {
        // Current song (full width)
        canvas.setFont(&fonts::Font2);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextColor(C_WHITE);
        String trk = nowTrack.length() > 0 ? nowTrack : "Loading track info...";
        drawScrollText(canvas, trk, 6, dy + 8, SCREEN_W - 12, scrSong);
    } else {
        // Visualizer fills the area below divider
        if (aRunning && !aPaused) {
            drawVisualizer(4, visY, SCREEN_W - 8, visH, st.color);
        }
    }

    drawFooterPlayer();
    canvas.pushSprite(0, 0);
}

// ═══════════════════════════════════════════════════════════
//  SCREEN: ERROR
// ═══════════════════════════════════════════════════════════
void drawError() {
    canvas.fillSprite(C_BG);
    drawHeader("ERROR");
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(C_HEADER2);
    canvas.setFont(&fonts::Font2);
    canvas.drawString(errorMsg, SCREEN_W / 2, SCREEN_H / 2 - 8);
    canvas.setTextColor(C_GRAY);
    canvas.setFont(&fonts::Font0);
    canvas.drawString("Press Enter to retry", SCREEN_W / 2, SCREEN_H / 2 + 14);
    drawFooter("Enter: Retry");
    canvas.pushSprite(0, 0);
}

// ═══════════════════════════════════════════════════════════
//  AUDIO CONTROL
// ═══════════════════════════════════════════════════════════
String streamUrl(const String &id) {
    return String("http://ice1.somafm.com/") + id + "-" +
           String(STREAM_BITRATE) + "-" + STREAM_FORMAT;
}

void cleanupAudio() {
    if (mp3)       { if (mp3->isRunning()) mp3->stop(); delete mp3; mp3 = nullptr; }
    if (audioBuf)  { delete audioBuf;  audioBuf  = nullptr; }
    if (audioSrc)  { delete audioSrc;  audioSrc  = nullptr; }
    aRunning = false;
}

// ── Audio FreeRTOS task (Core 0) ─────────────────────────
void audioTask(void *) {
    for (;;) {
        // Check for commands - single variable, no race condition
        int cmd = aCmd;
        if (cmd != ACMD_NONE) {
            aCmd = ACMD_NONE;
            cleanupAudio();
            Serial.printf("[AUDIO] cmd=%d target=%d\n", cmd, aTarget);

            if (cmd == ACMD_PLAY) {
                int idx = aTarget;
                if (idx >= 0 && idx < stationCount) {
                    String url = streamUrl(stations[idx].id);
                    Serial.printf("[AUDIO] Connecting: %s  heap=%u\n",
                                  url.c_str(), ESP.getFreeHeap());

                    audioSrc = new AudioFileSourceHTTPStream(url.c_str());
                    audioBuf = new AudioFileSourceBuffer(audioSrc, AUDIO_BUF_SIZE);
                    mp3      = new AudioGeneratorMP3();

                    if (mp3->begin(audioBuf, audioOut)) {
                        aRunning   = true;
                        playingIdx = idx;
                        Serial.printf("[AUDIO] Playing! heap=%u\n", ESP.getFreeHeap());
                    } else {
                        Serial.println("[AUDIO] begin() FAILED");
                        cleanupAudio();
                    }
                }
            }
            continue;  // Re-check commands before looping audio
        }

        // Run audio decoder
        if (mp3 && mp3->isRunning()) {
            if (!mp3->loop()) {
                Serial.println("[AUDIO] Stream ended, retrying...");
                cleanupAudio();
                // Interruptible wait before retry
                for (int i = 0; i < 20 && aCmd == ACMD_NONE; i++)
                    vTaskDelay(pdMS_TO_TICKS(100));
                // Auto-retry only if no new command arrived
                if (aCmd == ACMD_NONE) {
                    aTarget = playingIdx;
                    aCmd    = ACMD_PLAY;
                }
            }
        }

        vTaskDelay(1);
    }
}

void startPlaying(int idx) {
    aPaused    = false;
    playingIdx = idx;   // Update UI immediately
    selectedIdx = idx;
    aTarget    = idx;
    aCmd       = ACMD_PLAY;  // Single atomic write - audio task handles stop+start
    scrTitle.text = "";  // Reset scroll positions for new station
    scrGenre.text = "";
    scrSong.text  = "";
    saveLastStation();
    Serial.printf("[CMD] play(%d)\n", idx);
}

void stopPlaying() {
    aPaused = false;
    aCmd = ACMD_STOP;
}

void setVolume(uint8_t v) {
    volume = v;
    if (audioOut) audioOut->SetGain((float)v / 200.0f);
}

void saveSettings() {
    prefs.begin("somafm", false);
    prefs.putUChar("vol", volume);
    prefs.putUChar("vis", (uint8_t)visMode);
    prefs.end();
}

void loadSettings() {
    prefs.begin("somafm", true);
    volume  = prefs.getUChar("vol", DEFAULT_VOLUME);
    visMode = prefs.getUChar("vis", VIS_BARS);
    prefs.end();
    if (visMode >= VIS_COUNT) visMode = VIS_BARS;
}

void cycleVisMode() {
    visMode = (visMode + 1) % VIS_COUNT;
    saveSettings();
}

// ═══════════════════════════════════════════════════════════
//  INPUT HANDLING
// ═══════════════════════════════════════════════════════════
void handleBrowserKeys() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    if (millis() - tLastKey < DEBOUNCE_MS) return;
    tLastKey = millis();

    auto ks = M5Cardputer.Keyboard.keysState();

    if (hasKey(ks.word, ';') || hasKey(ks.word, 'w')) {
        if (selectedIdx > 0) {
            selectedIdx--;
            if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
        }
    }
    if (hasKey(ks.word, '.') || hasKey(ks.word, 's')) {
        if (selectedIdx < stationCount - 1) {
            selectedIdx++;
            if (selectedIdx >= scrollOffset + (int)VISIBLE_LINES)
                scrollOffset = selectedIdx - VISIBLE_LINES + 1;
        }
    }
    if (hasKey(ks.word, 'q')) {
        selectedIdx  = max(0, selectedIdx - (int)VISIBLE_LINES);
        scrollOffset = max(0, scrollOffset - (int)VISIBLE_LINES);
    }
    if (hasKey(ks.word, 'e')) {
        selectedIdx = min(stationCount - 1, selectedIdx + (int)VISIBLE_LINES);
        if (selectedIdx >= scrollOffset + (int)VISIBLE_LINES)
            scrollOffset = min(stationCount - (int)VISIBLE_LINES,
                               selectedIdx - (int)VISIBLE_LINES + 1);
    }
    if (ks.enter) {
        nowTrack = "";
        freeLogo();       // Clear old logo
        startPlaying(selectedIdx);
        appState = STATE_PLAYING;
        tLastNP  = 0;
    }
    if (hasKey(ks.word, ' ')) {
        if (playingIdx >= 0) {
            aPaused = !aPaused;
        } else {
            nowTrack = "";
            freeLogo();
            startPlaying(selectedIdx);
            appState = STATE_PLAYING;
            tLastNP  = 0;
        }
    }
    if (hasKey(ks.word, 'f')) { toggleFavorite(selectedIdx); }
    if (ks.tab) { cycleVisMode(); }
    if (hasKey(ks.word, ',')) { setVolume((volume > 15) ? volume - 15 : 0); saveSettings(); }
    if (hasKey(ks.word, '/')) { setVolume((volume < 240) ? volume + 15 : 255); saveSettings(); }
}

void handlePlayerKeys() {
    // G0 button (BtnA) → back to browser
    if (M5.BtnA.wasPressed()) {
        appState = STATE_BROWSER;
        return;
    }
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    if (millis() - tLastKey < DEBOUNCE_MS) return;
    tLastKey = millis();

    auto ks = M5Cardputer.Keyboard.keysState();

    if (ks.del) {
        appState = STATE_BROWSER;
    }
    if (hasKey(ks.word, 'x')) {
        stopPlaying();
        playingIdx = -1;
        appState   = STATE_BROWSER;
    }
    if (hasKey(ks.word, 'f')) { toggleFavorite(playingIdx); }
    if (hasKey(ks.word, ' ')) { aPaused = !aPaused; }
    if (ks.tab) { cycleVisMode(); }
    if (hasKey(ks.word, ',')) { setVolume((volume > 15) ? volume - 15 : 0); saveSettings(); }
    if (hasKey(ks.word, '/')) { setVolume((volume < 240) ? volume + 15 : 255); saveSettings(); }
    if (hasKey(ks.word, '.')) {
        int next = (playingIdx + 1) % stationCount;
        selectedIdx = next;
        nowTrack    = "";
        freeLogo();
        startPlaying(next);
        tLastNP = 0;
    }
    if (hasKey(ks.word, ';')) {
        int prev = (playingIdx - 1 + stationCount) % stationCount;
        selectedIdx = prev;
        nowTrack    = "";
        freeLogo();
        startPlaying(prev);
        tLastNP = 0;
    }
}

void handleErrorKeys() {
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
    auto ks = M5Cardputer.Keyboard.keysState();
    if (ks.enter) { appState = STATE_BOOT; }
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    Serial.begin(115200);
    Serial.println("\n[SOMA FM] Starting...");

    // Display
    M5.Display.setRotation(1);
    M5.Display.setBrightness(80);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    // Show splash immediately
    canvas.fillSprite(C_BG);
    canvas.setTextDatum(MC_DATUM);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextColor(C_ACCENT);
    canvas.drawString("SOMA FM", SCREEN_W / 2, 50);
    canvas.setFont(&fonts::Font0);
    canvas.setTextColor(C_DARKGRAY);
    canvas.drawString("Connecting...", SCREEN_W / 2, 80);
    canvas.pushSprite(0, 0);

    // Start WiFi early (non-blocking) so it connects while we load cache
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    // Persistent flash cache for channels + logos
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed");
    } else {
        Serial.println("[FS] LittleFS mounted");
        if (!LittleFS.exists("/logos")) LittleFS.mkdir("/logos");
    }

    // Release M5.Speaker's I2S port so we can drive it directly
    M5.Speaker.end();
    delay(100);

    // Direct I2S output to ES8311 on port 1 (Cardputer ADV: bck=41, ws=43, dout=42)
    audioOut = new DirectI2SOutput(I2S_NUM_1, 41, 43, 42);
    audioOut->begin();

    // Re-initialize ES8311 DAC registers
    es8311_init_dac();

    // Restore saved volume and visualizer mode
    loadSettings();
    audioOut->SetGain((float)volume / 200.0f);
    Serial.printf("[SETUP] DirectI2S on port 1, ES8311 init, vol=%d vis=%d\n", volume, visMode);

    // Launch audio task on Core 0
    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 2, &audioTaskH, 0);

    Serial.printf("[SETUP] Ready, heap=%u\n", ESP.getFreeHeap());
}

// ═══════════════════════════════════════════════════════════
//  MAIN LOOP  (runs on Core 1)
// ═══════════════════════════════════════════════════════════
void loop() {
    M5Cardputer.update();

    // ── Boot sequence ──
    if (appState == STATE_BOOT) {
        if (loadCachedChannels()) {
            // Instant boot from cache — show browser immediately
            Serial.printf("[BOOT] Cached %d stations\n", stationCount);
            loadFavorites();
            sortStations();
            restoreLastStation();
            appState = STATE_BROWSER;
            needsRefresh = true;  // refresh from network on next passes
            return;  // show UI immediately, don't block on WiFi
        }
        // No cache — must fetch from network
        if (!connectWiFi()) {
            errorMsg = "WiFi connection failed";
            appState = STATE_ERROR;
            drawError();
            return;
        }
        if (!fetchChannels()) {
            appState = STATE_ERROR;
            drawError();
            return;
        }
        loadFavorites();
        sortStations();
        restoreLastStation();
        appState = STATE_BROWSER;
    }

    // ── Deferred network refresh (non-blocking — only when WiFi ready) ──
    if (needsRefresh && WiFi.status() == WL_CONNECTED) {
        needsRefresh = false;
        Serial.println("[REFRESH] WiFi connected, updating channels...");
        if (fetchChannels(false)) {
            loadFavorites();
            sortStations();
            restoreLastStation();
            Serial.println("[REFRESH] Updated from network");
        }
    }

    // ── Input ──
    switch (appState) {
        case STATE_BROWSER: handleBrowserKeys(); break;
        case STATE_PLAYING: handlePlayerKeys();  break;
        case STATE_ERROR:   handleErrorKeys();   break;
        default: break;
    }

    // ── Periodic: fetch now-playing info ──
    if (appState == STATE_PLAYING && playingIdx >= 0) {
        if (millis() - tLastNP > NP_MS || tLastNP == 0) {
            tLastNP = millis();
            fetchNowPlaying();
        }
        // Download logo if not yet cached
        if (logoForIdx != playingIdx) {
            downloadLogo(playingIdx);
        }
    }

    // ── UI redraw ──
    if (millis() - tLastUI > UI_MS) {
        tLastUI = millis();
        switch (appState) {
            case STATE_BROWSER: drawBrowser(); break;
            case STATE_PLAYING: drawPlayer();  break;
            case STATE_ERROR:   drawError();   break;
            default: break;
        }
    }
}
