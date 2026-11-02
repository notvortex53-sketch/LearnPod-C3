/*
 * LearnPod AI — ESP32-C3 Mini  (v5 — Server pipeline + BLE PC mode)
 *
 * PIPELINE (AI mode):
 *   PTT -> INMP441 (I2S) record -> ONE HTTPS upload to Render server ->
 *   server does ElevenLabs STT -> Groq LLM -> ElevenLabs TTS ->
 *   streams 16 kHz PCM back -> played on MAX98357A, captions on SSD1306.
 *
 * PC MODE (BLE — replaces learnpod_pc.py over WiFi):
 *   Hold PTT 3 s -> device advertises BLE "LearnPod-C3" (Nordic UART Serv.)
 *   -> PC app (learnpod_ble_pc.py) connects -> record command ->
 *   server STT -> command sent over BLE -> PC executes -> result
 *   spoken through server TTS.
 *
 * PIN ASSIGNMENTS (unchanged):
 *   INMP441 mic  : SCK=GPIO6  WS=GPIO7  SD=GPIO5
 *   MAX98357A amp: BCLK=GPIO4 LRC=GPIO3 DIN=GPIO2  GAIN->GND (12dB)
 *   SSD1306 OLED : SDA=GPIO8  SCL=GPIO9  (I2C)
 *   PTT button   : GPIO10 -> GND (INPUT_PULLUP)
 *   FLASH button : GPIO9 (built-in) — hold on boot -> WiFi Selector
 *
 * WiFi Selector (hold FLASH on boot):
 *   - Scroll networks: short-press PTT
 *   - Connect:         hold PTT (1.5 s)
 *   - Add network:     send "ADD:ssid:password" over Serial (115200)
 *   - Up to 5 networks stored in NVS (survive reboot/flash)
 *
 * LIBRARIES:
 *   - ESP32 Arduino core (espressif/arduino-esp32) — BLE is bundled
 *   - Adafruit SSD1306 + Adafruit GFX
 *   - ArduinoJson
 *   - Preferences (built-in)
 *
 * BOARD: "ESP32C3 Dev Module" in Arduino IDE (USB CDC On Boot: Enabled)
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

// BLE (bundled with the ESP32 core)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ─── SERVER (Render) ─────────────────────────────────────────────────────────
#define SERVER_HOST "learnpod-c3.onrender.com"   // your Render service URL
#define SERVER_PORT 443
#define PC_MODE_HOLD_MS 3000   // hold PTT this long to enter BLE PC mode

// ─── BLE (Nordic UART Service) ───────────────────────────────────────────────
#define BLE_DEVICE_NAME    "LearnPod-C3"
#define BLE_WAIT_MS        90000   // wait for PC to connect before giving up
#define NUS_SERVICE_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // device->PC notify
#define NUS_RX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // PC->device write
#define BLE_REPLY_TIMEOUT  15000

// ─── SYSTEM PROMPT (redundant — LLM runs on the server — kept for reference) ─
// "You are LearnPod, a concise voice AI assistant. Always reply in 25 words
//  or fewer. Be direct and clear."  ->  now lives in server/main.py

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
// 5 s buffer now that the server does the heavy lifting (still tiny in RAM).
// If you see "ERR: no memory", drop back to 3-4 s.
#define SAMPLE_RATE     16000
#define RECORD_SECONDS  5
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

// BLE globals
static BLEServer*          bleServer   = nullptr;
static BLECharacteristic*  bleTxChar   = nullptr;
static volatile bool       bleConnected = false;
static String              bleLastLine  = "";
static volatile bool       bleLineFresh = false;
static String              bleIncoming  = "";

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

// Add or update a network. Returns false if list is full and SSID is new.
bool addNetwork(const String& ssid, const String& pass) {
  // Check if SSID already exists -> update password
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

    // PTT not pressed -> idle
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
      // Long press -> connect
      oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid, true);
      if (connectToNetwork(selectedIdx)) return;  // success -> exit selector
      oledWifiSelector(selectedIdx, wifiCount, wifiList[selectedIdx].ssid);
    } else {
      // Short press -> next network
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

// Wait for a PTT press (edge), used before recording in PC mode
bool waitForPTTPress(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (digitalRead(PTT_PIN) == LOW) { delay(30); return (digitalRead(PTT_PIN) == LOW); }
    delay(10);
  }
  return false;
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
// SERVER (Render) — replaces direct ElevenLabs/Groq calls
// Wire format for /voice and /tts:
//   line 1 = JSON {"transcript":...,  "reply":...}   (or {"reply":...} for /tts)
//   rest   = raw 16-bit 16 kHz mono PCM, play directly on MAX98357A
// ═════════════════════════════════════════════════════════════════════════════

// Read+discard HTTP response headers; false on non-200
bool skipHttpHeaders(WiFiClientSecure& client, String& statusLine) {
  statusLine = "";
  bool first = true;
  unsigned long t0 = millis();
  // 90 s: covers Render free-tier cold start (~60 s wake) + STT/LLM time
  while ((client.connected() || client.available()) && millis() - t0 < 90000) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      t0 = millis();  // reset timeout on data
      if (first) { statusLine = line; first = false; }
      if (line.startsWith("HTTP/") && line.indexOf(" 200 ") < 0) {
        Serial.println("[SRV] HTTP error: " + line);
        return false;
      }
      if (line == "\r" || line.length() <= 1) break;  // end of headers
    } else {
      delay(1);
    }
  }
  return statusLine.indexOf(" 200 ") >= 0;
}

// Stream remaining client bytes to the amp (16-bit PCM 16 kHz)
void streamPcmToSpeaker(WiFiClientSecure& client) {
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  delay(20);

  const size_t CHUNK = 512;
  uint8_t chunk[CHUNK];
  unsigned long lastData = millis();

  while (true) {
    int avail = client.available();
    if (avail > 0) {
      int got = client.read(chunk, min(avail, (int)CHUNK));
      if (got > 0) {
        // Scale down slightly to avoid clipping hum on 16-ohm speaker
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
      break;
    } else {
      if (millis() - lastData > 8000) break; // covers server thinking time + gaps
      delay(2);
    }
  }
  client.stop();
  delay(300);
  i2s_zero_dma_buffer(I2S_AMP_PORT);
}

// ─── AI mode: upload WAV to /voice, show captions, play server audio ─────────
bool serverVoice() {
  size_t wavLen = 44 + audioLen;
  uint8_t* wavBuf = (uint8_t*)malloc(wavLen);
  if (!wavBuf) { oledMsg("ERR: no memory", "", "", FACE_ERROR); delay(1500); return false; }
  buildWav(wavBuf, audioBuf, audioLen);

  String path = String("/voice");
  String head = String("POST ") + path + " HTTP/1.1\r\nHost: " + SERVER_HOST +
    "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + String(wavLen) +
    "\r\nConnection: close\r\n\r\n";

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    free(wavBuf); oledMsg("Server fail", "connect err", "", FACE_ERROR); delay(1500); return false;
  }
  client.print(head);
  client.write(wavBuf, wavLen);
  free(wavBuf);
  Serial.println("[SRV] /voice uploaded " + String(wavLen) + " bytes");

  // Server now runs STT -> LLM -> TTS (first request after Render cold-start
  // can take ~30-60s). Show thinking face while waiting.
  oledMsg("Thinking on server", "", "", FACE_THINK);

  // Switch I2S to amp while the server thinks (no data flying yet — safe)
  switchToAmp();

  String status;
  if (!skipHttpHeaders(client, status)) {
    oledMsg("Server API err", "check Render logs", "", FACE_ERROR); delay(1500); return false;
  }

  // First body line = JSON meta
  String meta = client.readStringUntil('\n');
  Serial.println("[SRV] Meta: " + meta);
  StaticJsonDocument<768> md;
  if (deserializeJson(md, meta)) {
    oledMsg("Server meta err", "", "", FACE_ERROR); delay(1500); return false;
  }
  String transcript = md["transcript"] | "";
  String reply      = md["reply"] | "";
  if (reply.isEmpty()) { oledMsg("Empty reply", "", "", FACE_ERROR); delay(1500); return false; }

  Serial.println("User: " + transcript);
  Serial.println("Bot:  " + reply);
  oledCaption("Bot: " + reply);
  streamPcmToSpeaker(client);
  return true;
}

// ─── BLE PC mode: transcript only ─────────────────────────────────────────────
String serverSTT() {
  oledMsg("Transcribing...", "", "", FACE_THINK);

  size_t wavLen = 44 + audioLen;
  uint8_t* wavBuf = (uint8_t*)malloc(wavLen);
  if (!wavBuf) { oledMsg("ERR: no memory", "", "", FACE_ERROR); delay(1500); return ""; }
  buildWav(wavBuf, audioBuf, audioLen);

  String head = String("POST /stt HTTP/1.1\r\nHost: ") + SERVER_HOST +
    "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + String(wavLen) +
    "\r\nConnection: close\r\n\r\n";

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    free(wavBuf); oledMsg("Server fail", "connect err", "", FACE_ERROR); delay(1500); return "";
  }
  client.print(head);
  client.write(wavBuf, wavLen);
  free(wavBuf);

  String status;
  if (!skipHttpHeaders(client, status)) {
    oledMsg("Server API err", "check Render logs", "", FACE_ERROR); delay(1500); return "";
  }

  // Read full JSON body
  String resp = "";
  unsigned long t = millis();
  while ((client.connected() || client.available()) && millis() - t < 15000) {
    if (client.available()) { resp += (char)client.read(); t = millis(); }
    else delay(1);
  }
  client.stop();

  int j = resp.indexOf('{');
  if (j < 0) return "";
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, resp.substring(j))) return "";
  String text = doc["text"] | "";
  Serial.println("[SRV] Command transcript: " + text);
  return text;
}

// ─── BLE PC mode: speak the PC's result through the server ────────────────────
bool serverSpeak(const String& text) {
  StaticJsonDocument<256> req;
  req["text"] = text;
  String body;
  serializeJson(req, body);

  String head = String("POST /tts HTTP/1.1\r\nHost: ") + SERVER_HOST +
    "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
    "\r\nConnection: close\r\n\r\n";

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    oledMsg("Server fail", "connect err", "", FACE_ERROR); delay(1500); return false;
  }
  client.print(head);
  client.print(body);

  String status;
  if (!skipHttpHeaders(client, status)) {
    oledMsg("Server API err", "check Render logs", "", FACE_ERROR); delay(1500); return false;
  }
  // meta line ({"reply": ...}) then PCM
  String meta = client.readStringUntil('\n');
  streamPcmToSpeaker(client);
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// BLE — Nordic UART Service, PC mode peripheral
// ═════════════════════════════════════════════════════════════════════════════

class LearnPodServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    bleConnected = true;
    Serial.println("[BLE] PC connected");
  }
  void onDisconnect(BLEServer* s) override {
    bleConnected = false;
    Serial.println("[BLE] PC disconnected — advertising again");
    BLEDevice::startAdvertising();
  }
};

class LearnPodRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String v = String(ch->getValue().c_str());
    if (v.length() == 0) return;
    bleIncoming += v;
    int nl = bleIncoming.indexOf('\n');
    while (nl >= 0) {
      bleLastLine  = bleIncoming.substring(0, nl);
      bleIncoming  = bleIncoming.substring(nl + 1);
      bleLineFresh = true;
      nl = bleIncoming.indexOf('\n');
    }
    if (bleIncoming.length() > 1024) bleIncoming = "";
  }
};

void startBLE() {
  if (bleServer) { BLEDevice::startAdvertising(); return; }
  Serial.println("[BLE] Init...");
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(517);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new LearnPodServerCallbacks());

  BLEService* svc = bleServer->createService(NUS_SERVICE_UUID);
  bleTxChar = svc->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTxChar->addDescriptor(new BLE2902());

  BLECharacteristic* rx = svc->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new LearnPodRxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as " BLE_DEVICE_NAME);
}

bool bleSend(const String& objJson) {
  if (!bleConnected || !bleTxChar) return false;
  String out = objJson + "\n";
  bleTxChar->setValue((uint8_t*)out.c_str(), out.length());
  bleTxChar->notify();
  return true;
}

// Wait for a newline-terminated JSON frame from the PC
bool bleWaitReply(String& replyJson, uint32_t timeoutMs) {
  uint32_t t0 = millis();
  bleLineFresh = false;
  while (millis() - t0 < timeoutMs) {
    if (!bleConnected) return false;
    if (bleLineFresh) {
      bleLineFresh = false;
      replyJson = bleLastLine;
      return true;
    }
    delay(20);
  }
  return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// PC MODE OVER BLE — full flow
// ═════════════════════════════════════════════════════════════════════════════

bool blePCMode() {
  startBLE();

  // Advertise until the PC app connects
  oledMsg("PC Mode (BLE)", "Waiting for PC", "run python app", FACE_LISTEN);
  uint32_t t0 = millis();
  while (!bleConnected) {
    if (millis() - t0 > BLE_WAIT_MS) {
      BLEDevice::stopAdvertising();
      oledMsg("No PC found", "Run app &", "try again", FACE_ERROR);
      delay(2000);
      return false;
    }
    delay(100);
  }

  // Ping the PC app; make sure a controller is actually listening
  oledMsg("PC Connected!", "Checking app...", "", FACE_THINK);
  String reply;
  bleSend("{\"cmd\":\"ping\"}");
  if (!bleWaitReply(reply, 5000)) {
    oledMsg("PC app not ready", "rerun python", "and retry", FACE_ERROR);
    delay(2000);
    return false;
  }

  // Record a command
  oledMsg("PC Connected!", "Hold PTT to", "speak command", FACE_IDLE);
  if (!waitForPTTPress(20000)) { BLEDevice::stopAdvertising(); return false; }

  switchToMic();
  if (!recordAudio()) {
    BLEDevice::stopAdvertising();
    return false;
  }

  String transcript = serverSTT();
  if (transcript.isEmpty()) {
    oledMsg("Didn't catch", "Try again", "", FACE_ERROR);
    delay(1500);
    BLEDevice::stopAdvertising();
    return false;
  }

  oledCaption("PC: " + transcript);
  delay(400);

  // Send {"cmd":"exec","text":"..."} to the PC
  StaticJsonDocument<512> cmd;
  cmd["cmd"]  = "exec";
  cmd["text"] = transcript;
  String cmdStr;
  serializeJson(cmd, cmdStr);
  oledMsg("PC Mode", "Executing...", "", FACE_THINK);
  bleSend(cmdStr);

  String result = "PC did not reply";
  if (bleWaitReply(reply, BLE_REPLY_TIMEOUT)) {
    StaticJsonDocument<512> respDoc;
    if (!deserializeJson(respDoc, reply)) {
      const char* msg = respDoc["message"];
      if (msg) result = String(msg);
    }
  }
  Serial.println("[BLE] PC result: " + result);

  // Speak the result through the server TTS
  oledCaption("PC: " + result);
  switchToAmp();
  serverSpeak(result);

  BLEDevice::stopAdvertising();
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

  // Check if FLASH button held at boot -> WiFi Selector mode
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

  oledMsg("LearnPod Ready", "Hold PTT", "to talk (BLE 3s=PC)", FACE_IDLE);
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
      // Crossed threshold -> switch to PC mode UI
      pcMode = true;
      oledMsg("PC Mode", "BLE starting...", "", FACE_LISTEN);
    }
    delay(10);
  }

  // ── PC AUTOMATION MODE (over BLE) ─────────────────────────────────────────
  if (pcMode) {
    blePCMode();
    oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
    return;
  }

  // ── AI ASSISTANT MODE ────────────────────────────────────────────────────
  // 1+2+3. Record -> upload to server (STT+LLM+TTS) -> play streamed audio
  switchToMic();
  if (!recordAudio()) return;

  if (!serverVoice()) {
    oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
    return;
  }

  oledMsg("LearnPod Ready", "Hold PTT", "to talk", FACE_IDLE);
}
