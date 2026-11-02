/*
 * LearnPod AI — ESP32-C3 Mini
 * Pipeline: PTT → INMP441 (I2S) → ElevenLabs STT → Groq LLM → ElevenLabs TTS → MAX98357A (I2S)
 *           + SSD1306 OLED captions
 *
 * PIN ASSIGNMENTS:
 *   INMP441 mic  : SCK=GPIO6  WS=GPIO7  SD=GPIO5
 *   MAX98357A amp: BCLK=GPIO4 LRC=GPIO3 DIN=GPIO2  GAIN→GND (12dB)
 *   SSD1306 OLED : SDA=GPIO8  SCL=GPIO9  (I2C)
 *   PTT button   : GPIO10 → GND (INPUT_PULLUP)
 *   FLASH button : GPIO9 (built-in) — hold on boot → WiFi Selector
 *
 * WiFi Selector (hold FLASH on boot):
 *   - Scroll networks: short-press PTT
 *   - Connect:         hold PTT (1.5 s)
 *   - Add network:     send "ADD:ssid:password" over Serial (115200)
 *   - Up to 5 networks stored in NVS (survive reboot/flash)
 *
 * LIBRARIES:
 *   - ESP32 Arduino core (espressif/arduino-esp32)
 *   - Adafruit SSD1306 + Adafruit GFX
 *   - ArduinoJson
 *   - Preferences (built-in with ESP32 core)
 *
 * BOARD: "ESP32C3 Dev Module" in Arduino IDE
 */

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ─── API KEYS ────────────────────────────────────────────────────────────────
#define GROQ_API_KEY     "YOUR_GROQ_API_KEY"
#define ELEVENLABS_KEY   "YOUR_ELEVENLABS_API_KEY"
#define ELEVENLABS_VOICE "YOUR_VOICE_ID"

#define SYSTEM_PROMPT \
  "You are LearnPod, a concise voice AI assistant. " \
  "Always reply in 25 words or fewer. Be direct and clear."

// ─── PC AUTOMATION ───────────────────────────────────────────────────────────
// Run learnpod_pc.py on your Windows PC, then put its local IP here
#define PC_HOST "192.168.1.100"   // <-- change to your PC's local IP
#define PC_PORT 5005
// Hold PTT for this long to enter PC mode instead of AI mode
#define PC_MODE_HOLD_MS 3000

// ─── PINS ────────────────────────────────────────────────────────────────────
#define MIC_SCK       6
#define MIC_WS        7
#define MIC_SD        5
#define AMP_BCLK      4
#define AMP_LRC       3
#define AMP_DIN       2
#define PTT_PIN       10
#define FLASH_BTN     9   // built-in FLASH/BOOT button on C3 Mini

// ─── I2S ─────────────────────────────────────────────────────────────────────
// ESP32-C3 has only ONE I2S port — mic and amp share it (driver swapped per use)
#define I2S_MIC_PORT  I2S_NUM_0
#define I2S_AMP_PORT  I2S_NUM_0

// ─── AUDIO ───────────────────────────────────────────────────────────────────
#define SAMPLE_RATE     16000
#define RECORD_SECONDS  3
#define BITS_PER_SAMPLE 16
#define DMA_BUF_COUNT   8
#define DMA_BUF_LEN     512
#define MAX_AUDIO_BYTES (SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * RECORD_SECONDS)

// ─── OLED ────────────────────────────────────────────────────────────────────
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_ADDR   0x3C

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ─── WIFI STORE ──────────────────────────────────────────────────────────────
#define MAX_NETWORKS  5
Preferences prefs;

struct WifiEntry {
  String ssid;
  String pass;
};

WifiEntry wifiList[MAX_NETWORKS];
int       wifiCount    = 0;
int       lastUsedIdx  = -1;  // index of last connected network

// ─── GLOBALS ─────────────────────────────────────────────────────────────────
uint8_t* audioBuf = nullptr;
size_t   audioLen = 0;

// ═════════════════════════════════════════════════════════════════════════════
// FACE DRAWING
// Face occupies right half (64–127 x, 0–63 y). Text goes on left (0–58).
// ═════════════════════════════════════════════════════════════════════════════

// Face states
enum FaceState { FACE_IDLE, FACE_LISTEN, FACE_THINK, FACE_SPEAK, FACE_ERROR };

// Blink state for idle animation
bool _eyesClosed   = false;
uint32_t _blinkAt  = 0;
uint16_t _animFrame = 0;

// ── single eye ────────────────────────────────────────────────────────────────
// Ported directly from LearnPod ESP8266 Phase 6 drawEye()
// White filled rounded-rect socket + black pupil circle + white glint dot
void drawEye(int16_t cx, int16_t cy, bool closed, bool listening) {
  if (closed) {
    // Blink: two horizontal lines
    oled.drawLine(cx - 10, cy, cx + 10, cy, SSD1306_WHITE);
    oled.drawLine(cx - 8,  cy + 2, cx + 8, cy + 2, SSD1306_WHITE);
    return;
  }
  // Eye socket
  oled.fillRoundRect(cx - 10, cy - 9, 20, 20, 6, SSD1306_WHITE);
  if (listening) {
    // Bigger pupil when listening
    oled.fillCircle(cx, cy + 1, 5, SSD1306_BLACK);
    oled.fillCircle(cx + 2, cy - 1, 2, SSD1306_WHITE); // glint
  } else {
    oled.fillCircle(cx, cy + 1, 4, SSD1306_BLACK);
    oled.fillCircle(cx + 2, cy - 1, 1, SSD1306_WHITE); // glint
  }
}

// ── mouth ─────────────────────────────────────────────────────────────────────
// mood 0 = calm smile, 1 = open O (listening), 2 = flat line (thinking)
void drawMouth(uint8_t mood) {
  if (mood == 0) {
    oled.drawPixel(61, 52, SSD1306_WHITE);
    oled.drawLine( 62, 53, 66, 53, SSD1306_WHITE);
    oled.drawPixel(67, 52, SSD1306_WHITE);
  } else if (mood == 1) {
    oled.drawRoundRect(61, 49, 7, 6, 2, SSD1306_WHITE);
  } else if (mood == 2) {
    oled.drawLine(61, 52, 68, 52, SSD1306_WHITE);
  }
}

// ── header ────────────────────────────────────────────────────────────────────
void drawHeader(const char* label) {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(2, 1);
  oled.print("LEARNPOD");
  oled.drawLine(2, 10, 125, 10, SSD1306_WHITE);
  oled.setCursor(82, 1);
  oled.print(label);
}

// ── full face ─────────────────────────────────────────────────────────────────
void drawFace(FaceState state) {
  // Eyes at same positions as Phase 6: left=43, right=85, y=27
  if (state == FACE_IDLE) {
    drawHeader("READY");
    uint32_t now = millis();
    if (now - _blinkAt > 2900) {
      _eyesClosed = true; _blinkAt = now;
    } else if (now - _blinkAt > 150) {
      _eyesClosed = false;
    }
    drawEye(43, 27, _eyesClosed, false);
    drawEye(85, 27, _eyesClosed, false);
    drawMouth(0);

  } else if (state == FACE_LISTEN) {
    drawHeader("LISTEN");
    drawEye(43, 25, false, true);
    drawEye(85, 25, false, true);
    drawMouth(1);
    // Audio bars at bottom (same as Phase 6)
    for (uint8_t i = 0; i < 18; i++) {
      uint8_t h = 2 + ((_animFrame * (i % 3 + 1) + i * 7) % 13);
      oled.fillRect(7 + i * 6, 63 - h, 3, h, SSD1306_WHITE);
    }
    _animFrame++;

  } else if (state == FACE_THINK) {
    drawHeader("THINK");
    drawEye(43, 27, false, false);
    drawEye(85, 27, false, false);
    drawMouth(2);
    // Spinning dot ring at bottom (same as Phase 6)
    const int16_t cx = 64, cy = 54;
    for (uint8_t i = 0; i < 8; i++) {
      uint8_t idx = (i + _animFrame / 2) % 8;
      int16_t x1 = cx + (int16_t)(6  * cos(idx * 0.7854f));
      int16_t y1 = cy + (int16_t)(6  * sin(idx * 0.7854f));
      int16_t x2 = cx + (int16_t)(10 * cos(idx * 0.7854f));
      int16_t y2 = cy + (int16_t)(10 * sin(idx * 0.7854f));
      oled.drawLine(x1, y1, x2, y2, i < 3 ? SSD1306_WHITE : SSD1306_BLACK);
    }
    _animFrame++;

  } else if (state == FACE_SPEAK) {
    drawHeader("SPEAK");
    drawEye(43, 27, false, false);
    drawEye(85, 27, false, false);
    drawMouth(0); // smile while speaking

  } else if (state == FACE_ERROR) {
    drawHeader("ERR");
    // X eyes — draw X inside each eye socket area
    oled.fillRoundRect(33, 18, 20, 20, 6, SSD1306_WHITE);
    oled.fillRoundRect(75, 18, 20, 20, 6, SSD1306_WHITE);
    oled.drawLine(36, 21, 50, 35, SSD1306_BLACK);
    oled.drawLine(50, 21, 36, 35, SSD1306_BLACK);
    oled.drawLine(78, 21, 92, 35, SSD1306_BLACK);
    oled.drawLine(92, 21, 78, 35, SSD1306_BLACK);
    // Sad mouth
    oled.drawPixel(61, 54, SSD1306_WHITE);
    oled.drawLine( 62, 53, 66, 53, SSD1306_WHITE);
    oled.drawPixel(67, 54, SSD1306_WHITE);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// OLED HELPERS
// ═════════════════════════════════════════════════════════════════════════════

// Full-screen: header bar at top, face (eyes + mouth) below
void oledMsg(const char* line1, const char* line2 = "", const char* line3 = "", FaceState face = FACE_IDLE) {
  oled.clearDisplay();
  drawFace(face);
  // Override bottom text area with status lines (below eyes, y>=46)
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  if (strlen(line2) > 0) { oled.setCursor(2, 46); oled.print(line2); }
  if (strlen(line3) > 0) { oled.setCursor(2, 55); oled.print(line3); }
  oled.display();
}

// Caption: header + face + scrolling answer text at bottom
void oledCaption(const String& text, FaceState face = FACE_SPEAK) {
  oled.clearDisplay();
  drawFace(face);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  // Print up to 42 chars (2 lines) at bottom
  String t = text.length() > 42 ? text.substring(0, 42) : text;
  oled.setCursor(2, 46);
  oled.setTextWrap(true);
  oled.print(t);
  oled.display();
}

// Draw WiFi selector screen
// Shows current index, SSID, signal (if scanned), and instructions
void oledWifiSelector(int idx, int total, const String& ssid, bool connecting = false) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Header
  oled.setCursor(0, 0);
  oled.print("WiFi Selector");
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Network count indicator  e.g. [2/4]
  String counter = "[" + String(idx + 1) + "/" + String(total) + "]";
  oled.setCursor(128 - (counter.length() * 6), 0);
  oled.print(counter);

  // SSID (truncate if too long)
  oled.setCursor(0, 16);
  oled.setTextSize(1);
  String display_ssid = ssid.length() > 18 ? ssid.substring(0, 17) + "~" : ssid;
  oled.print(display_ssid);

  if (connecting) {
    oled.setCursor(0, 36);
    oled.print("Connecting...");
  } else {
    oled.setCursor(0, 36);
    oled.print("PTT: next");
    oled.setCursor(0, 48);
    oled.print("Hold PTT: connect");
  }
  oled.display();
}

// ═════════════════════════════════════════════════════════════════════════════
// NVS WiFi STORAGE
// ═════════════════════════════════════════════════════════════════════════════

// ─── DEFAULT WIFI (change before flashing) ───────────────────────────────────
#define DEFAULT_SSID  "YOUR_WIFI_SSID"
#define DEFAULT_PASS  "YOUR_WIFI_PASSWORD"

void loadWifiList() {
  prefs.begin("learnpod", true);  // read-only
  wifiCount   = prefs.getInt("count", 0);
  lastUsedIdx = prefs.getInt("lastUsed", -1);
  for (int i = 0; i < wifiCount && i < MAX_NETWORKS; i++) {
    wifiList[i].ssid = prefs.getString(("s" + String(i)).c_str(), "");
    wifiList[i].pass = prefs.getString(("p" + String(i)).c_str(), "");
  }
  prefs.end();

  // Seed first network from placeholders if NVS is empty
  if (wifiCount == 0 && String(DEFAULT_SSID) != "YOUR_WIFI_SSID") {
    wifiList[0].ssid = DEFAULT_SSID;
    wifiList[0].pass = DEFAULT_PASS;
    wifiCount = 1;
    lastUsedIdx = 0;
    saveWifiList();
  }
}

void saveWifiList() {
  prefs.begin("learnpod", false);  // read-write
  prefs.putInt("count", wifiCount);
  prefs.putInt("lastUsed", lastUsedIdx);
  for (int i = 0; i < wifiCount; i++) {
    prefs.putString(("s" + String(i)).c_str(), wifiList[i].ssid);
    prefs.putString(("p" + String(i)).c_str(), wifiList[i].pass);
  }
  prefs.end();
}

// Add or update a network. Returns false if list is full and SSID is new.
bool addNetwork(const String& ssid, const String& pass) {
  // Check if SSID already exists → update password
  for (int i = 0; i < wifiCount; i++) {
    if (wifiList[i].ssid == ssid) {
      wifiList[i].pass = pass;
      saveWifiList();
      return true;
    }
  }
  if (wifiCount >= MAX_NETWORKS) return false;
  wifiList[wifiCount].ssid = ssid;
  wifiList[wifiCount].pass = pass;
  wifiCount++;
  saveWifiList();
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// WiFi CONNECTION
// ═════════════════════════════════════════════════════════════════════════════

bool connectToNetwork(int idx) {
  if (idx < 0 || idx >= wifiCount) return false;
  oledMsg("Connecting...", wifiList[idx].ssid.c_str());
  WiFi.begin(wifiList[idx].ssid.c_str(), wifiList[idx].pass.c_str());
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); tries++;
    // Animate dots on OLED
    oled.fillRect(0, 44, 128, 10, SSD1306_BLACK);
    oled.setCursor(0, 44);
    String dots = "";
    for (int d = 0; d < (tries % 4); d++) dots += ".";
    oled.print(dots);
    oled.display();
  }
  if (WiFi.status() == WL_CONNECTED) {
    lastUsedIdx = idx;
    saveWifiList();
    oledMsg("Connected!", wifiList[idx].ssid.c_str(), WiFi.localIP().toString().c_str());
    delay(1500);
    return true;
  }
  oledMsg("Failed!", wifiList[idx].ssid.c_str(), "Try another");
  delay(1500);
  return false;
}

// Try last-used network automatically (no button held)
bool autoConnect() {
  if (lastUsedIdx < 0 || lastUsedIdx >= wifiCount) {
    // Fall back to first in list
    if (wifiCount > 0) lastUsedIdx = 0;
    else return false;
  }
  oledMsg("Auto-connecting...", wifiList[lastUsedIdx].ssid.c_str());
  return connectToNetwork(lastUsedIdx);
}

// ═════════════════════════════════════════════════════════════════════════════
// WiFi SELECTOR MODE
// Entered when FLASH button held at boot.
// PTT short press = next network
// PTT held 1.5 s  = connect to highlighted network
// Serial "ADD:ssid:password" = add network
// ═════════════════════════════════════════════════════════════════════════════

void wifiSelectorMode() {
  oledMsg("WiFi Selector", "Loading...");
  delay(300);

  if (wifiCount == 0) {
    oledMsg("No networks saved", "Send via Serial:", "ADD:ssid:pass");
    // Wait for serial input only
    while (true) {
      if (Serial.available()) {
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.startsWith("ADD:")) {
          line = line.substring(4);
          int sep = line.indexOf(':');
          if (sep > 0) {
            String s = line.substring(0, sep);
            String p = line.substring(sep + 1);
            if (addNetwork(s, p)) {
              Serial.println("Added: " + s);
              oledMsg("Network added!", s.c_str(), "Rebooting...");
              delay(1500);
              ESP.restart();
            }
          }
        }
      }
      delay(50);
    }
  }

  int selectedIdx = (lastUsedIdx >= 0 && lastUsedIdx < wifiCount) ? lastUsedIdx : 0;
  oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid);

  while (true) {
    // Serial: add new network on the fly
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.startsWith("ADD:")) {
        line = line.substring(4);
        int sep = line.indexOf(':');
        if (sep > 0) {
          String s = line.substring(0, sep);
          String p = line.substring(sep + 1);
          if (addNetwork(s, p)) {
            Serial.println("Added: " + s);
            oledMsg("Network added!", s.c_str());
            delay(1000);
            oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid);
          } else {
            Serial.println("List full (max 5). Remove one first.");
          }
        }
      }
    }

    // PTT not pressed → idle
    if (digitalRead(PTT_PIN) != LOW) {
      delay(20);
      continue;
    }

    // PTT pressed — measure hold duration
    delay(50);  // debounce
    if (digitalRead(PTT_PIN) != LOW) continue;

    unsigned long pressStart = millis();
    while (digitalRead(PTT_PIN) == LOW) delay(10);
    unsigned long held = millis() - pressStart;

    if (held >= 1500) {
      // Long press → connect
      oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid, true);
      if (connectToNetwork(selectedIdx)) return;  // success → exit selector
      oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid);
    } else {
      // Short press → next network
      selectedIdx = (selectedIdx + 1) % wifiCount;
      oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid);
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// I2S MIC
// ═════════════════════════════════════════════════════════════════════════════

void initMic() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

// ═════════════════════════════════════════════════════════════════════════════
// I2S AMP
// ═════════════════════════════════════════════════════════════════════════════

void initAmp() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = 16000,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = DMA_BUF_COUNT,
    .dma_buf_len          = DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = AMP_BCLK,
    .ws_io_num    = AMP_LRC,
    .data_out_num = AMP_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_AMP_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_AMP_PORT, &pins);
  i2s_zero_dma_buffer(I2S_AMP_PORT);
}

// ═════════════════════════════════════════════════════════════════════════════
// I2S SWAP HELPERS — C3 has one port, must uninstall before switching mode
// ═════════════════════════════════════════════════════════════════════════════

void switchToMic() {
  i2s_driver_uninstall(I2S_NUM_0);
  initMic();
}

void switchToAmp() {
  i2s_driver_uninstall(I2S_NUM_0);
  initAmp();
}

// ═════════════════════════════════════════════════════════════════════════════
// RECORD AUDIO
// ═════════════════════════════════════════════════════════════════════════════

bool recordAudio() {
  if (!audioBuf) {
    audioBuf = (uint8_t*)malloc(MAX_AUDIO_BYTES);
    if (!audioBuf) { oledMsg("ERR: no memory"); return false; }
  }
  audioLen = 0;
  oledMsg("Recording...", "Release to send", "", FACE_LISTEN);

  int32_t raw32[DMA_BUF_LEN];
  int16_t* out16 = (int16_t*)audioBuf;
  size_t   maxSamples = SAMPLE_RATE * RECORD_SECONDS;

  while (digitalRead(PTT_PIN) == LOW && (audioLen / 2) < maxSamples) {
    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, raw32, sizeof(raw32), &bytesRead, portMAX_DELAY);
    int samplesRead = bytesRead / 4;
    for (int i = 0; i < samplesRead && (audioLen / 2) < maxSamples; i++) {
      out16[audioLen / 2] = (int16_t)(raw32[i] >> 16);
      audioLen += 2;
    }
  }
  if (audioLen < 3200) {
    oledMsg("Too short,", "hold longer");
    delay(1000);
    return false;
  }
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// WAV HEADER
// ═════════════════════════════════════════════════════════════════════════════

size_t buildWav(uint8_t* wavBuf, const uint8_t* pcm, size_t pcmLen) {
  uint32_t totalSize  = 36 + pcmLen;
  uint32_t byteRate   = SAMPLE_RATE * 1 * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = 1 * (BITS_PER_SAMPLE / 8);
  uint8_t hdr[44] = {
    'R','I','F','F',
    (uint8_t)(totalSize),(uint8_t)(totalSize>>8),(uint8_t)(totalSize>>16),(uint8_t)(totalSize>>24),
    'W','A','V','E','f','m','t',' ',
    16,0,0,0, 1,0, 1,0,
    (uint8_t)(SAMPLE_RATE),(uint8_t)(SAMPLE_RATE>>8),(uint8_t)(SAMPLE_RATE>>16),(uint8_t)(SAMPLE_RATE>>24),
    (uint8_t)(byteRate),(uint8_t)(byteRate>>8),(uint8_t)(byteRate>>16),(uint8_t)(byteRate>>24),
    (uint8_t)(blockAlign),(uint8_t)(blockAlign>>8),
    (uint8_t)(BITS_PER_SAMPLE),(uint8_t)(BITS_PER_SAMPLE>>8),
    'd','a','t','a',
    (uint8_t)(pcmLen),(uint8_t)(pcmLen>>8),(uint8_t)(pcmLen>>16),(uint8_t)(pcmLen>>24)
  };
  memcpy(wavBuf, hdr, 44);
  memcpy(wavBuf + 44, pcm, pcmLen);
  return 44 + pcmLen;
}

// ═════════════════════════════════════════════════════════════════════════════
// ELEVENLABS STT
// ═════════════════════════════════════════════════════════════════════════════

String elevenSTT(const uint8_t* pcm, size_t pcmLen) {
  oledMsg("Transcribing...", "", "", FACE_THINK);

  size_t wavLen = 44 + pcmLen;
  uint8_t* wavBuf = (uint8_t*)malloc(wavLen);
  if (!wavBuf) { oledMsg("ERR: no memory"); return ""; }
  buildWav(wavBuf, pcm, pcmLen);

  String boundary = "----LearnPodBound";
  String bodyHead =
    "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String bodyTail = "\r\n--" + boundary + "--\r\n";
  size_t totalLen = bodyHead.length() + wavLen + bodyTail.length();

  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("[STT] Connecting to api.elevenlabs.io...");
  if (!client.connect("api.elevenlabs.io", 443)) {
    free(wavBuf); oledMsg("STT connect fail"); Serial.println("[STT] Connection failed"); return "";
  }
  Serial.println("[STT] Connected. Sending request...");
  client.printf(
    "POST /v1/speech-to-text HTTP/1.1\r\n"
    "Host: api.elevenlabs.io\r\n"
    "xi-api-key: %s\r\n"
    "Content-Type: multipart/form-data; boundary=%s\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n",
    ELEVENLABS_KEY, boundary.c_str(), totalLen
  );
  client.print(bodyHead);
  client.write(wavBuf, wavLen);
  client.print(bodyTail);
  free(wavBuf);
  Serial.println("[STT] Request sent. Waiting for response...");

  // Read full response — keep draining even after connected() drops
  String resp = "";
  unsigned long t = millis();
  while ((client.connected() || client.available()) && millis() - t < 12000) {
    if (client.available()) {
      resp += (char)client.read();
      t = millis(); // reset timeout on each byte received
    } else {
      delay(1);
    }
  }
  client.stop();

  Serial.println("[STT] Raw response:");
  Serial.println(resp.substring(0, 500)); // print first 500 chars

  int bodyStart = resp.indexOf("\r\n\r\n");
  if (bodyStart < 0) { oledMsg("STT parse err"); Serial.println("[STT] No header boundary found"); return ""; }
  String json = resp.substring(bodyStart + 4);

  // Handle chunked transfer encoding (strip chunk size lines)
  if (resp.indexOf("Transfer-Encoding: chunked") >= 0 ||
      resp.indexOf("transfer-encoding: chunked") >= 0) {
    String unchunked = "";
    int pos = 0;
    while (pos < (int)json.length()) {
      int nl = json.indexOf("\r\n", pos);
      if (nl < 0) break;
      String sizeLine = json.substring(pos, nl);
      sizeLine.trim();
      int chunkSize = strtol(sizeLine.c_str(), nullptr, 16);
      if (chunkSize == 0) break;
      pos = nl + 2;
      if (pos + chunkSize > (int)json.length()) break;
      unchunked += json.substring(pos, pos + chunkSize);
      pos += chunkSize + 2;
    }
    json = unchunked;
  }

  // Find first '{' in case there's leading whitespace
  int jsonStart = json.indexOf('{');
  if (jsonStart < 0) { oledMsg("STT: no JSON"); Serial.println("[STT] No JSON object in body: " + json); return ""; }
  json = json.substring(jsonStart);

  Serial.println("[STT] JSON body: " + json.substring(0, 200));

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) { oledMsg("STT JSON err"); Serial.println("[STT] JSON parse error: " + String(err.c_str())); return ""; }
  const char* text = doc["text"];
  if (!text) { Serial.println("[STT] No 'text' field. Keys present: "); serializeJson(doc, Serial); Serial.println(); return ""; }
  Serial.println("[STT] Transcript: " + String(text));
  return String(text);
}

// ═════════════════════════════════════════════════════════════════════════════
// GROQ LLM
// ═════════════════════════════════════════════════════════════════════════════

String groqChat(const String& userText) {
  oledMsg("Thinking...", userText.substring(0, 10).c_str(), "", FACE_THINK);

  StaticJsonDocument<512> req;
  req["model"] = "qwen/qwen3-27b";
  req["max_tokens"] = 80;
  JsonArray msgs = req.createNestedArray("messages");
  JsonObject sys = msgs.createNestedObject();
  sys["role"] = "system"; sys["content"] = SYSTEM_PROMPT;
  JsonObject usr = msgs.createNestedObject();
  usr["role"] = "user"; usr["content"] = userText;

  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.groq.com", 443)) { oledMsg("Groq connect fail"); return ""; }
  client.printf(
    "POST /openai/v1/chat/completions HTTP/1.1\r\n"
    "Host: api.groq.com\r\n"
    "Authorization: Bearer %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n",
    GROQ_API_KEY, body.length()
  );
  client.print(body);

  String resp = "";
  unsigned long t = millis();
  while (client.connected() && millis() - t < 10000) {
    if (client.available()) resp += (char)client.read();
  }
  client.stop();

  int bodyStart = resp.indexOf("\r\n\r\n");
  if (bodyStart < 0) { oledMsg("Groq parse err"); return ""; }
  String json = resp.substring(bodyStart + 4);
  if (json.indexOf("{") > 0) json = json.substring(json.indexOf("{"));

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, json)) { oledMsg("Groq JSON err"); return ""; }
  const char* reply = doc["choices"][0]["message"]["content"];
  return reply ? String(reply) : "";
}

// ═════════════════════════════════════════════════════════════════════════════
// ELEVENLABS TTS + PLAY
// ═════════════════════════════════════════════════════════════════════════════

void elevenTTS(const String& text, const String& caption) {
  oledCaption(caption);

  StaticJsonDocument<256> req;
  req["text"] = text;
  req["model_id"] = "eleven_turbo_v2";
  JsonObject vs = req.createNestedObject("voice_settings");
  vs["stability"] = 0.5;
  vs["similarity_boost"] = 0.75;

  String body;
  serializeJson(req, body);

  String path = "/v1/text-to-speech/";
  path += ELEVENLABS_VOICE;
  path += "/stream?output_format=pcm_16000";

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.elevenlabs.io", 443)) { oledMsg("TTS connect fail"); return; }
  client.printf(
    "POST %s HTTP/1.1\r\n"
    "Host: api.elevenlabs.io\r\n"
    "xi-api-key: %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n",
    path.c_str(), ELEVENLABS_KEY, body.length()
  );
  client.print(body);

  // Skip HTTP response headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line == "\r\n" || line.length() == 0) break;
    // Bail on HTTP error (non-200)
    if (line.startsWith("HTTP/") && line.indexOf(" 200 ") < 0) {
      Serial.println("TTS HTTP error: " + line);
      client.stop();
      oledMsg("TTS API error", line.substring(9, 12).c_str());
      delay(1500);
      return;
    }
  }

  // Clear DMA before playback to avoid residual noise
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  delay(20);

  const size_t CHUNK = 512;
  uint8_t chunk[CHUNK];
  unsigned long lastData = millis();

  // Stream PCM → I2S. Keep reading even after connected() drops (data still in socket buffer).
  while (true) {
    int avail = client.available();
    if (avail > 0) {
      int got = client.read(chunk, min(avail, (int)CHUNK));
      if (got > 0) {
        // Scale down slightly to avoid clipping hum on 16Ω speaker
        int16_t* samples = (int16_t*)chunk;
        int count = got / 2;
        for (int i = 0; i < count; i++) {
          samples[i] = (int16_t)(samples[i] * 0.85f);
        }
        size_t written = 0;
        i2s_write(I2S_AMP_PORT, chunk, got, &written, portMAX_DELAY);
        lastData = millis();
      }
    } else if (!client.connected()) {
      break;  // no data and connection closed → done
    } else {
      if (millis() - lastData > 4000) break;  // safety timeout
      delay(2);
    }
  }
  client.stop();
  delay(300);  // let DMA flush last buffer
  i2s_zero_dma_buffer(I2S_AMP_PORT);
}

// ═════════════════════════════════════════════════════════════════════════════
// PC AUTOMATION
// Sends transcript to learnpod_pc.py running on Windows PC over plain HTTP
// ═════════════════════════════════════════════════════════════════════════════

bool sendPCCommand(const String& command) {
  oledMsg("PC Mode", "Sending...", "", FACE_THINK);
  Serial.println("[PC] Sending command: " + command);

  WiFiClient client;  // plain HTTP, not HTTPS
  if (!client.connect(PC_HOST, PC_PORT)) {
    oledMsg("PC: No connect", "Is script running?", "", FACE_ERROR);
    Serial.println("[PC] Could not connect to " PC_HOST);
    delay(2000);
    return false;
  }

  // Build JSON body
  String body = "{\"command\":\"" + command + "\"}";
  client.printf(
    "POST /command HTTP/1.1\r\n"
    "Host: %s:%d\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %d\r\n"
    "Connection: close\r\n\r\n",
    PC_HOST, PC_PORT, body.length()
  );
  client.print(body);

  // Read response
  String resp = "";
  unsigned long t = millis();
  while ((client.connected() || client.available()) && millis() - t < 5000) {
    if (client.available()) { resp += (char)client.read(); t = millis(); }
    else delay(1);
  }
  client.stop();
  Serial.println("[PC] Response: " + resp);

  // Parse result message from JSON {"result":"done","message":"Opened Chrome"}
  int bodyStart = resp.indexOf("\r\n\r\n");
  String result = "Done";
  if (bodyStart >= 0) {
    String json = resp.substring(bodyStart + 4);
    int jsonStart = json.indexOf('{');
    if (jsonStart >= 0) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, json.substring(jsonStart))) {
        const char* msg = doc["message"];
        if (msg) result = String(msg);
      }
    }
  }

  oledMsg("PC Mode", result.substring(0, 18).c_str(), "", FACE_SPEAK);
  // Speak the result back
  switchToAmp();
  elevenTTS(result, result);
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  pinMode(PTT_PIN,   INPUT_PULLUP);
  pinMode(FLASH_BTN, INPUT_PULLUP);

  // OLED
  Wire.begin(8, 9);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    while (1);
  }
  oledMsg("LearnPod AI", "ESP32-C3 Mini", "Booting...", FACE_IDLE);
  delay(400);

  // Load saved networks from NVS
  loadWifiList();
  Serial.printf("Loaded %d network(s) from NVS\n", wifiCount);

  // Check if FLASH button held at boot → WiFi Selector mode
  if (digitalRead(FLASH_BTN) == LOW) {
    delay(80);  // debounce
    if (digitalRead(FLASH_BTN) == LOW) {
      Serial.println("Entering WiFi Selector mode");
      wifiSelectorMode();
      // Falls through after successful connect
    }
  } else {
    // Auto-connect to last used / first saved network
    if (wifiCount == 0) {
      oledMsg("No WiFi saved!", "Hold FLASH btn", "on boot to add");
      Serial.println("No networks saved. Send ADD:ssid:pass via Serial.");
      while (WiFi.status() != WL_CONNECTED) {
        if (Serial.available()) {
          String line = Serial.readStringUntil('\n');
          line.trim();
          if (line.startsWith("ADD:")) {
            line = line.substring(4);
            int sep = line.indexOf(':');
            if (sep > 0) {
              addNetwork(line.substring(0, sep), line.substring(sep + 1));
              ESP.restart();
            }
          }
        }
        delay(100);
      }
    } else {
      if (!autoConnect()) {
        oledMsg("Auto-connect fail", "Hold FLASH btn", "to pick network");
        delay(3000);
        wifiSelectorMode();
      }
    }
  }

  oledMsg("WiFi OK", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  delay(1000);

  // I2S — start in mic mode (amp switched in when needed)
  initMic();

  oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  // Serial: add network at runtime (useful without rebooting)
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("ADD:")) {
      line = line.substring(4);
      int sep = line.indexOf(':');
      if (sep > 0) {
        String s = line.substring(0, sep);
        String p = line.substring(sep + 1);
        if (addNetwork(s, p)) Serial.println("Saved: " + s);
        else Serial.println("List full (max 5)");
      }
    }
  }

  // ── PTT detection ─────────────────────────────────────────────────────────
  if (digitalRead(PTT_PIN) != LOW) { delay(20); return; }
  delay(50);  // debounce
  if (digitalRead(PTT_PIN) != LOW) return;

  // Measure how long PTT is held before release OR until PC_MODE_HOLD_MS
  unsigned long pressStart = millis();
  bool pcMode = false;

  while (digitalRead(PTT_PIN) == LOW) {
    unsigned long held = millis() - pressStart;
    if (held >= PC_MODE_HOLD_MS && !pcMode) {
      // Crossed threshold → switch to PC mode UI
      pcMode = true;
      oledMsg("PC Mode", "Speak command", "", FACE_LISTEN);
    }
    delay(10);
  }

  // ── PC AUTOMATION MODE ────────────────────────────────────────────────────
  if (pcMode) {
    switchToMic();
    if (!recordAudio()) {
      oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
      return;
    }
    String command = elevenSTT(audioBuf, audioLen);
    if (command.isEmpty()) {
      oledMsg("Didn't catch", "Try again", "", FACE_ERROR);
      delay(1500);
      oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
      return;
    }
    Serial.println("[PC] Command: " + command);
    oledCaption("PC: " + command);
    delay(400);
    sendPCCommand(command);
    oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
    return;
  }

  // ── AI ASSISTANT MODE ─────────────────────────────────────────────────────
  // 1. Record
  switchToMic();
  if (!recordAudio()) return;

  // 2. STT
  String transcript = elevenSTT(audioBuf, audioLen);
  if (transcript.isEmpty()) {
    oledMsg("Didn't catch", "Try again", "", FACE_ERROR);
    delay(1500);
    oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
    return;
  }
  Serial.println("User: " + transcript);
  oledCaption("You: " + transcript);
  delay(600);

  // 3. LLM
  String reply = groqChat(transcript);
  if (reply.isEmpty()) {
    oledMsg("No response", "Try again", "", FACE_ERROR);
    delay(1500);
    oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
    return;
  }
  Serial.println("Bot: " + reply);

  // 4. TTS + play
  switchToAmp();
  elevenTTS(reply, "Bot: " + reply);

  oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
}
