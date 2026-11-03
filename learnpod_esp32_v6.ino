/*
 * LearnPod AI — ESP32 Classic  (v6 — Server pipeline + BLE PC mode + Wake word)
 *
 * PIPELINE (AI mode):
 *   PTT -> INMP441 (I2S) record -> ONE HTTPS upload to Render server ->
 *   server does ElevenLabs STT -> Groq LLM -> ElevenLabs TTS ->
 *   streams 16 kHz PCM back -> played on MAX98357A, captions on SSD1306.
 *
 * WAKE WORD (always on while idle — no button needed):
 *   Says "Hey LearnPod" or "Hey Buddy" + your question, e.g.
 *   "Hey Buddy, what's the capital of France?"
 *   Device listens continuously (energy VAD + 1.2 s pre-roll), sends the
 *   clip to the server, STT confirms the wake word, then the remainder
 *   goes to /ask (LLM) and the answer is spoken. PTT still works normally.
 *
 * PC MODE (BLE — replaces learnpod_pc.py over WiFi):
 *   Hold PTT 3 s -> device advertises BLE "LearnPod-ESP32" (Nordic UART Serv.)
 *   -> PC app (learnpod_ble_pc.py) connects -> record command ->
 *   server STT -> command sent over BLE -> PC executes -> result
 *   spoken through server TTS.
 *
 * PIN ASSIGNMENTS:
 *   INMP441 mic  : SCK=GPIO18 WS=GPIO19 SD=GPIO32
 *   MAX98357A amp: BCLK=GPIO26 LRC=GPIO25 DIN=GPIO23  GAIN->GND (12dB)
 *   SSD1306 OLED : SDA=GPIO21 SCL=GPIO22  (I2C)
 *   PTT button   : GPIO4 -> GND (INPUT_PULLUP)
 *   BOOT button  : GPIO0 (built-in) — hold 2 s while running -> WiFi Selector
 *
 * WiFi Selector (hold BOOT 2 s while the device is running):
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
 * BOARD: "ESP32 Dev Module" in Arduino IDE (partition "Huge APP 3MB")
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
#include "BluetoothA2DPSource.h"   // Library Manager: "ESP32-A2DP" by pschatzmann

// ─── SERVER (Render) ─────────────────────────────────────────────────────────
#define SERVER_HOST "learnpod-c3.onrender.com"   // your Render service URL
#define SERVER_PORT 443
#define PC_MODE_HOLD_MS 3000   // hold PTT this long to enter BLE PC mode

// ─── BLE (Nordic UART Service) ───────────────────────────────────────────────
#define BLE_DEVICE_NAME    "LearnPod-ESP32"
#define BLE_WAIT_MS        90000   // wait for PC to connect before giving up
#define NUS_SERVICE_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID        "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // device->PC notify
#define NUS_RX_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // PC->device write
#define BLE_REPLY_TIMEOUT  15000

// ─── SYSTEM PROMPT (redundant — LLM runs on the server — kept for reference) ─
// "You are LearnPod, a concise voice AI assistant. Always reply in 25 words
//  or fewer. Be direct and clear."  ->  now lives in server/main.py

// ─── PINS ────────────────────────────────────────────────────────────────────
#define MIC_SCK       18
#define MIC_WS        19
#define MIC_SD        32
#define AMP_BCLK      26
#define AMP_LRC       25
#define AMP_DIN       23
#define PTT_PIN       4
#define FLASH_BTN     0   // built-in BOOT button on ESP32 Dev Boards

// ─── I2S ─────────────────────────────────────────────────────────────────────
// Classic ESP32 has TWO I2S ports — mic and amp each get their own, so there
// is no driver swapping (below, switchToMic/switchToAmp are simple no-ops).
#define I2S_MIC_PORT  I2S_NUM_0
#define I2S_AMP_PORT  I2S_NUM_1

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
// Actual buffer capacity in SAMPLES (may shrink to 3 s when the Bluetooth
// headphones stack is eating the heap).
size_t audioBufCapSamples = SAMPLE_RATE * RECORD_SECONDS;

// Allocate the record buffer once; cope with a tight heap gracefully.
static bool ensureAudioBuf() {
  if (audioBuf) return true;
  audioBuf = (uint8_t*)malloc(MAX_AUDIO_BYTES);
  if (audioBuf) { audioBufCapSamples = SAMPLE_RATE * RECORD_SECONDS; return true; }
  // Bluetooth classic stack active? Retry with a 3 s buffer.
  audioBuf = (uint8_t*)malloc(MAX_AUDIO_BYTES / 2);
  if (audioBuf) { audioBufCapSamples = SAMPLE_RATE * RECORD_SECONDS / 2; return true; }
  return false;
}

// BLE globals
static BLEServer*          bleServer   = nullptr;
static BLECharacteristic*  bleTxChar   = nullptr;
static volatile bool       bleConnected = false;
static String              bleLastLine  = "";
static volatile bool       bleLineFresh = false;
static String              bleIncoming  = "";

// ─── WAKE WORD ENGINE ("Hey LearnPod" / "Hey Buddy") ─────────────────────────
#define WAKE_FRAME_SAMPLES 160    // 10 ms frames at 16 kHz
#define WAKE_PREROLL_BYTES 38400  // 1.2 s pre-roll (16-bit mono 16 kHz)
#define WAKE_CLIP_BYTES    80000  // 2.5 s total clip sent to the server
#define WAKE_RUN_FRAMES    5      // 50 ms of sustained sound triggers a check
#define WAKE_MIN_LEVEL     800    // absolute floor: quieter than speech
#define WAKE_SILENCE_MAX   40     // 400 ms of silence ends the clip early
enum WakePhase { WAKE_SCAN, WAKE_CAPTURE, WAKE_BACKOFF };
static WakePhase wakePhase        = WAKE_SCAN;
static uint8_t   wakeRing[WAKE_PREROLL_BYTES];   // circular pre-roll
static size_t    wakeRingPos      = 0;
static bool      wakeRingFull     = false;
static uint8_t   wakeSpeechRun    = 0;
static uint8_t   wakeSilenceRun   = 0;
static uint32_t  wakeNoiseFloor   = 300;
static uint32_t  wakeBackoffUntil = 0;
static size_t    wakeClipLen      = 0;
static bool      wakeMicArmed     = false;
static int16_t   wakeFrame16[WAKE_FRAME_SAMPLES];

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

// ═════════════════════════════════════════════════════════════════════════════
// BLUETOOTH HEADPHONES / SPEAKER (A2DP source — classic ESP32 only)
// ═════════════════════════════════════════════════════════════════════════════
// The device connects TO your Bluetooth headphones/speaker and plays the bot's
// voice there ("like headphones"). Library needed in Arduino IDE:
//    Library Manager -> "ESP32-A2DP" by Phil Schatzmann
//
// UX:
//   Hold PTT 8 s           -> pairing: scan nearby audio devices, tap PTT to
//                             cycle, HOLD 1.5 s to connect & save
//   Next boots             -> auto-connects to the saved headphones
//   While connected        -> bot's voice plays on headphones (wired speaker
//                             stays silent); unplug headphone battery or
//                             re-pair by holding PTT 8 s again
// ═════════════════════════════════════════════════════════════════════════════

#define BT_HOLD_MS          8000   // PTT held this long opens BT pairing
#define BT_SELECT_HOLD_MS   1500   // confirm selection in the picker
#define BT_SCAN_SECS        20     // discovery window
#define BT_SCAN_MAX         12     // remembered candidate devices
#define BT_RING_SAMPLES     12288  // ~0.76 s of 16 kHz mono FIFO

BluetoothA2DPSource a2dpSrc;

static int16_t*  btRing          = nullptr;
static volatile size_t btRingR   = 0;
static volatile size_t btRingW   = 0;
static volatile size_t btRingUsed = 0;
static uint32_t  btResampleFrac  = 0;   // 16 kHz -> 44.1 kHz upsampler
static int16_t   btLastSample    = 0;
static volatile bool btSavedFlag = false; // connection state (from callback)
static bool      btA2dpStarted   = false;
static bool      btSession       = false; // pairing menu active
static String    btSavedName     = "";
static String    btScanList[BT_SCAN_MAX];
static int       btScanCount     = 0;

// Producer: loop task. Blocks while FIFO full (server stream is faster than
// realtime playback), which also throttles the download.
static void btRingWrite(const int16_t* src, size_t n) {
  if (!btRing) return;
  while (n > 0) {
    size_t space = BT_RING_SAMPLES - btRingUsed;
    if (space == 0) { delay(10); continue; }
    size_t take = (n < space) ? n : space;
    size_t w = btRingW;
    for (size_t i = 0; i < take; i++) {
      btRing[(w + i) % BT_RING_SAMPLES] = src[i];
    }
    btRingW = (w + take) % BT_RING_SAMPLES;
    btRingUsed += take;
    src += take;
    n -= take;
  }
}

// Consumer: A2DP callback task.
static size_t btRingRead(int16_t* dst, size_t want) {
  if (btRingUsed == 0 || !btRing) return 0;
  size_t take = (want < btRingUsed) ? want : btRingUsed;
  size_t r = btRingR;
  for (size_t i = 0; i < take; i++) {
    dst[i] = btRing[(r + i) % BT_RING_SAMPLES];
  }
  btRingR = (r + take) % BT_RING_SAMPLES;
  btRingUsed -= take;
  return take;
}

static void btRingReset() {
  btRingR = 0; btRingW = 0; btRingUsed = 0;
}

// Data callback: fill 44.1 kHz stereo 16-bit frames from the 16 kHz mono FIFO.
int32_t btSoundData(uint8_t* data, int32_t len) {
  int32_t frames = len / 4;              // stereo, 2 bytes/ch
  if (frames <= 0) return 0;
  int16_t* out = (int16_t*)data;
  static const uint32_t STEP_IN = 16000; // source sample rate
  static const uint32_t STEP_OUT = 44100;
  for (int32_t i = 0; i < frames; i++) {
    btResampleFrac += STEP_IN;
    int spins = 0;
    while (btResampleFrac >= STEP_OUT && spins < 4) {
      btResampleFrac -= STEP_OUT;
      int16_t s;
      if (btRingRead(&s, 1)) btLastSample = s;
      spins++;
    }
    if (btRingUsed == 0) btLastSample = 0;   // smooth tail -> silence
    out[2 * i] = btLastSample;
    out[2 * i + 1] = btLastSample;
  }
  return len;
}

// Device discovery: while pairing we just RECORD names (return false keeps
// scanning); outside pairing we auto-accept the saved headphones only.
bool btSsidCallback(const char* ssid, esp_bd_addr_t addr, int rssi) {
  (void)addr; (void)rssi;
  String n = ssid;
  if (!btSession) {
    if (btSavedName.length() > 0 && n.startsWith(btSavedName)) return true;
    return false;
  }
  for (int i = 0; i < btScanCount; i++) {
    if (btScanList[i] == n) return false;
  }
  if (btScanCount < BT_SCAN_MAX) { btScanList[btScanCount++] = n; }
  return false;
}

void btConnectionCallback(esp_a2d_connection_state_t st, void* obj) {
  (void)obj;
  btSavedFlag = (st == ESP_A2D_CONNECTION_STATE_CONNECTED);
}

// Whole pairing flow, blocking (only runs after an 8 s hold — user is waiting)
void btAudioPair() {
  btSession = true;
  btScanCount = 0;
  btSavedFlag = false;
  btRingReset();
  if (!btRing) {
    btRing = (int16_t*)malloc(BT_RING_SAMPLES * sizeof(int16_t));
    if (!btRing) { oledMsg("ERR: no memory", "", "", FACE_ERROR); btSession = false; delay(1500); return; }
  }
  oledMsg("BT Headphones", "Put headphones in", "pairing mode!");
  delay(2000);

  if (btA2dpStarted) {                 // re-pairing mid-session
    a2dpSrc.cancel_discovery();
    a2dpSrc.end(false);
    btA2dpStarted = false;
  }
  a2dpSrc.start();                     // discovery, ssid_callback records list
  btA2dpStarted = true;

  String picked = "";
  int idx = 0;
  unsigned long startT = millis();
  unsigned long tapStart = 0;
  bool lastPTT = false;

  while (true) {
    if (btSavedFlag) break;            // callback already accepted a device

    if (btScanCount == 0) {
      if (millis() - startT > BT_SCAN_SECS * 1000) break;
      oledMsg("BT Headphones", "Searching devices...");
    } else {
      oledMsg("BT: tap=next " + String(idx + 1) + "/" + String(btScanCount),
              btScanList[idx], "HOLD 1.5s = connect");
    }

    // picker controls
    bool p = (digitalRead(PTT_PIN) == LOW);
    if (p && !lastPTT) tapStart = millis();
    if (p && lastPTT && (millis() - tapStart >= BT_SELECT_HOLD_MS)) {
      picked = btScanList[idx];
      break;
    }
    if (!p && lastPTT) {
      unsigned long d = millis() - tapStart;
      if (d < 600 && btScanCount > 0) idx = (idx + 1) % btScanCount;
    }
    lastPTT = p;
    delay(30);
  }

  if (picked.isEmpty()) {
    a2dpSrc.cancel_discovery();
    a2dpSrc.end(false);
    btA2dpStarted = false;
    btSession = false;
    oledMsg("BT Headphones", "Found none", "Try again");
    delay(2200);
    return;
  }

  // Connect to the picked device by name
  a2dpSrc.cancel_discovery();
  delay(400);
  a2dpSrc.end(false);
  btA2dpStarted = false;

  btSavedName = picked;
  prefs.begin("learnpod", false);
  prefs.putString("btname", btSavedName);
  prefs.end();

  a2dpSrc.start(btSavedName.c_str());
  btA2dpStarted = true;

  unsigned long waitT = millis();
  while (!a2dpSrc.is_connected() && millis() - waitT < 12000) delay(200);

  if (a2dpSrc.is_connected()) {
    oledMsg("BT Connected!", btSavedName, "voice goes there");
  } else {
    oledMsg("BT: not connected", "Saved " + btSavedName, "will auto-connect");
  }
  delay(2500);
  btSession = false;
}


// Holding the BOOT button across a RESET would put the ESP32 in ROM download
// mode (sketch never runs). The reliable way: hold BOOT ~2 s while the device
// is RUNNING -> set a flag in NVS -> reboot into the WiFi selector.
void requestWifiSelector() {
  prefs.begin("learnpod", false);
  prefs.putBool("selboot", true);
  prefs.end();
  ESP.restart();
}

bool bootWifiSelectorFlag() {
  prefs.begin("learnpod", true);
  bool v = prefs.getBool("selboot", false);
  prefs.end();
  if (v) {                              // consume the request
    prefs.begin("learnpod", false);
    prefs.putBool("selboot", false);
    prefs.end();
  }
  return v;
}

// ─── BT headphones name load/save ─────────────────────────────────────────────
void loadBtName() {
  prefs.begin("learnpod", true);
  btSavedName = prefs.getString("btname", "");
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
// Entered via a 2 s hold of the BOOT button while running (see requestWifiSelector).
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
// I2S SWAP HELPERS — classic ESP32 has two ports, both stay installed, so
// these are only kept for compatibility with the shared code path.
// ═════════════════════════════════════════════════════════════════════════════

void switchToMic() {
  // no-op on classic ESP32: mic has its own I2S port, always armed
}

void switchToAmp() {
  // no-op on classic ESP32: amp has its own I2S port, always armed
}

// ═════════════════════════════════════════════════════════════════════════════
// RECORD AUDIO
// ═════════════════════════════════════════════════════════════════════════════

bool recordAudio() {
  if (!ensureAudioBuf()) { oledMsg("ERR: no memory"); return false; }
  audioLen = 0;
  oledMsg("Recording...", "Release to send", "", FACE_LISTEN);

  int32_t raw32[DMA_BUF_LEN];
  int16_t* out16 = (int16_t*)audioBuf;
  size_t   maxSamples = audioBufCapSamples;

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
  bool useBT = btA2dpStarted && btRing && a2dpSrc.is_connected();

  if (useBT) {
    btRingReset();  // drop any stale audio from a previous stream
  } else {
    i2s_zero_dma_buffer(I2S_AMP_PORT);
  }
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
        if (useBT) {
          btRingWrite(samples, count);   // paced: blocks while FIFO full
        } else {
          size_t written = 0;
          i2s_write(I2S_AMP_PORT, chunk, got, &written, portMAX_DELAY);
        }
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
  if (useBT) {
    delay(400);  // let the BT FIFO drain out
  } else {
    delay(300);
    i2s_zero_dma_buffer(I2S_AMP_PORT);
  }
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

// ─── Wake-word mode: send text prompt to /ask, play LLM TTS ───────────────────
bool serverAsk(const String& text) {
  oledMsg("Thinking on server", "", "", FACE_THINK);

  StaticJsonDocument<512> req;
  req["text"] = text;
  String body;
  serializeJson(req, body);

  String head = String("POST /ask HTTP/1.1\r\nHost: ") + SERVER_HOST +
    "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
    "\r\nConnection: close\r\n\r\n";

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    oledMsg("Server fail", "connect err", "", FACE_ERROR); delay(1500); return false;
  }
  client.print(head);
  client.print(body);

  switchToAmp();

  String status;
  if (!skipHttpHeaders(client, status)) {
    oledMsg("Server API err", "check Render logs", "", FACE_ERROR); delay(1500); return false;
  }

  String meta = client.readStringUntil('\n');
  Serial.println("[SRV] Meta: " + meta);
  StaticJsonDocument<768> md;
  if (deserializeJson(md, meta)) {
    oledMsg("Server meta err", "", "", FACE_ERROR); delay(1500); return false;
  }
  String reply = md["reply"] | "";
  const char* err = md["error"];
  if (err && reply.isEmpty()) {
    String e = String(err);
    oledCaption("SRV: " + e, FACE_ERROR);
    delay(2500);
    return false;
  }
  if (reply.isEmpty()) {
    oledMsg("Empty reply", "", "", FACE_ERROR); delay(1500); return false;
  }

  Serial.println("Bot:  " + reply);
  oledCaption("Bot: " + reply);
  streamPcmToSpeaker(client);
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// WAKE WORD ENGINE
// Always-on while idle: 10 ms energy frames -> sustained speech -> pre-roll
// clip sent to server STT -> matches "hey learnpod"/"hey buddy" (any position)
// -> remainder becomes the prompt for /ask. PTT button keeps working normally.
// ═════════════════════════════════════════════════════════════════════════════

static void wakeAppendRing(const int16_t* s, int cnt) {
  size_t bytes = (size_t)cnt * 2;
  size_t space = WAKE_PREROLL_BYTES - wakeRingPos;
  size_t take  = (bytes > space) ? space : bytes;
  memcpy(wakeRing + wakeRingPos, (const uint8_t*)s, take);
  wakeRingPos += take;
  if (bytes > take) {                       // wrapped
    memcpy(wakeRing, ((const uint8_t*)s) + take, bytes - take);
    wakeRingPos = bytes - take;
    wakeRingFull = true;
  }
}

// Copy ring (oldest first) into dst; returns bytes copied
static size_t wakeCopyRing(int16_t* dst) {
  if (!wakeRingFull) {
    memcpy(dst, wakeRing, wakeRingPos);
    return wakeRingPos;
  }
  size_t head = WAKE_PREROLL_BYTES - wakeRingPos;
  memcpy(dst, wakeRing + wakeRingPos, head);
  memcpy(((uint8_t*)dst) + head, wakeRing, wakeRingPos);
  return WAKE_PREROLL_BYTES;
}

// Find wake word in a transcript; returns true and puts the text AFTER the
// wake word into prompt (lowercased, punctuation stripped).
static bool wakeSplit(const String& t, String& prompt) {
  String s = t;
  s.toLowerCase();
  const char* punct = ",.!?;:\"'()";
  for (const char* p = punct; *p; p++) s.replace(String(*p), " ");
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  s.trim();

  const char* variants[] = { "learnpod", "learn pod", "buddy" };
  int best = -1; int bestLen = 0;
  for (const char* v : variants) {
    int p = s.indexOf(v);
    if (p >= 0 && (best < 0 || p < best)) { best = p; bestLen = (int)strlen(v); }
  }
  if (best < 0) return false;
  prompt = s.substring(best + bestLen);
  prompt.trim();
  return true;
}

// One-shot: STT the captured clip, act on it. Blocking (allowed: we're idle).
static void processWakeClip() {
  String text = serverSTT();
  Serial.println("[WW] Heard: " + text + "  (len " + String(audioLen) + ")");

  String prompt;
  if (text.isEmpty() || !wakeSplit(text, prompt)) {
    // not the wake word -> ignore silently, tiny face reset
    oledMsg("LearnPod", "Not a wake word", "", FACE_IDLE);
    delay(600);
    return;
  }

  Serial.println("[WW] Wake word OK. Prompt: " + prompt);

  if (prompt.length() < 2) {
    // Wake word alone: politely answer and re-arm
    oledMsg("Yes?", "", "", FACE_IDLE);
    switchToAmp();
    serverSpeak("Yes? I'm listening.");
    return;
  }

  Serial.println("User: " + prompt);
  oledCaption("You: " + prompt, FACE_THINK);
  delay(300);
  serverAsk(prompt);
}

// Called from loop() when idle. Reads one 10 ms mic frame per pass — keeps
// the loop responsive to PTT and Serial.
void wakeStep() {
  if (wakePhase == WAKE_BACKOFF) {
    if (millis() < wakeBackoffUntil) return;
    wakePhase = WAKE_SCAN;
    wakeRingPos  = 0; wakeRingFull = false;
    wakeSpeechRun = 0; wakeSilenceRun = 0;
  }

  // Amp steals I2S after playback; re-arm the mic driver lazily
  if (!wakeMicArmed) { switchToMic(); wakeMicArmed = true; wakeRingPos = 0; wakeRingFull = false; }

  // One 10 ms frame (fails open if DMA not ready yet)
  int32_t raw32[WAKE_FRAME_SAMPLES];
  size_t n = 0;
  i2s_read(I2S_MIC_PORT, raw32, sizeof(raw32), &n, pdMS_TO_TICKS(10));
  if (n < sizeof(raw32)) return;

  int cnt = n / 4;
  uint32_t sum = 0;
  for (int i = 0; i < cnt; i++) {
    int16_t v = (int16_t)(raw32[i] >> 16);
    wakeFrame16[i] = v;
    sum += (v < 0) ? -v : v;
  }
  uint32_t lvl = sum / (uint32_t)cnt;

  // Adaptive noise floor: tracks down quickly, creeps up slowly
  if (lvl < wakeNoiseFloor)              wakeNoiseFloor = (wakeNoiseFloor * 95  + lvl * 5) / 100;
  else if (lvl < wakeNoiseFloor * 3)     wakeNoiseFloor = (wakeNoiseFloor * 995 + lvl * 5) / 1000;

  bool speech = (lvl > wakeNoiseFloor * 4) && (lvl > WAKE_MIN_LEVEL);

  if (wakePhase == WAKE_SCAN) {
    wakeAppendRing(wakeFrame16, cnt);
    wakeSpeechRun = speech ? (uint8_t)(wakeSpeechRun + 1) : 0;
    if (wakeSpeechRun >= WAKE_RUN_FRAMES) {
      // Triggered: ring already holds ~1.2 s of context (pre-roll)
      if (!ensureAudioBuf()) {
        wakeBackoffUntil = millis() + 4000;
        wakePhase = WAKE_BACKOFF;
        return;
      }
      wakeClipLen = wakeCopyRing((int16_t*)audioBuf);
      wakeSilenceRun = 0;
      oledMsg("Wake?", "listening...", "", FACE_LISTEN);
      wakePhase = WAKE_CAPTURE;
    }

  } else if (wakePhase == WAKE_CAPTURE) {
    // Append live audio into the clip: pre-roll + live = up to ~2.5 s
    size_t space = (audioBufCapSamples * 2) - wakeClipLen;
    size_t want  = (size_t)cnt * 2;
    size_t take  = (want > space) ? space : want;
    memcpy(audioBuf + wakeClipLen, (const uint8_t*)wakeFrame16, take);
    wakeClipLen  += take;
    wakeSilenceRun = speech ? 0 : (uint8_t)(wakeSilenceRun + 1);

    bool fullOrQuiet =
      (wakeClipLen >= WAKE_CLIP_BYTES) ||
      (wakeSilenceRun >= WAKE_SILENCE_MAX && wakeClipLen >= WAKE_CLIP_BYTES / 2);
    if (fullOrQuiet) {
      audioLen = wakeClipLen;
      wakeBackoffUntil = millis() + 2500;
      wakePhase = WAKE_BACKOFF;
      processWakeClip();
    }
  }
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
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    while (1);
  }
  oledMsg("LearnPod AI", "ESP32 Classic", "Booting...", FACE_IDLE);
  delay(400);

  // Load saved networks from NVS
  loadWifiList();
  Serial.printf("Loaded %d network(s) from NVS\n", wifiCount);

  // WiFi Selector: enters if a 2 s BOOT hold (while running) rebooted us, or
  // if the button happens to be low at boot on boards that reach this point.
  bool selRequested = bootWifiSelectorFlag();
  if (selRequested || digitalRead(FLASH_BTN) == LOW) {
    delay(80);  // debounce
    if (selRequested || digitalRead(FLASH_BTN) == LOW) {
      Serial.println("Entering WiFi Selector mode");
      wifiSelectorMode();
      // Falls through after successful connect
    }
  } else {
    // Auto-connect to last used / first saved network
    if (wifiCount == 0) {
      oledMsg("No WiFi saved!", "Hold BOOT 2 s", "to pick network");
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
        oledMsg("Auto-connect fail", "Hold BOOT 2 s", "to pick network");
        delay(3000);
        wifiSelectorMode();
      }
    }
  }

  oledMsg("WiFi OK", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  delay(1000);

  // I2S — start in mic mode (amp switched in when needed)
  initMic();
  initAmp();   // classic ESP32: second I2S port for the speaker, always ready

  // Bluetooth headphones: register callbacks once, auto-connect if saved
  loadBtName();
  a2dpSrc.set_ssid_callback(btSsidCallback);
  a2dpSrc.set_data_callback(btSoundData);
  a2dpSrc.set_on_connection_state_changed(btConnectionCallback, nullptr);
  if (btSavedName.length() > 0) {
    Serial.printf("BT: auto-connecting to %s\n", btSavedName.c_str());
    a2dpSrc.set_auto_reconnect(true);
    a2dpSrc.start(btSavedName.c_str());
    btA2dpStarted = true;
  }

  oledMsg("LearnPod Ready", "Hold PTT", "8s=BT audio 3s=PC", FACE_IDLE);
}

// ═════════════════════════════════════════════════════════════════════════════
// LOOP
// ═════════════════════════════════════════════════════════════════════════════

void loop() {
  // BOOT button held ~2 s while RUNNING -> reboot into WiFi Selector.
  // (Holding it across a reset enters ROM download mode instead, so we do it
  // this way: flag in NVS, restart, selector.)
  static uint32_t bootHoldStart = 0;
  if (digitalRead(FLASH_BTN) == LOW) {
    if (bootHoldStart == 0) bootHoldStart = millis();
    else if (millis() - bootHoldStart > 2000) {
      oledMsg("WiFi Selector", "rebooting...", "");
      requestWifiSelector();
    }
  } else {
    bootHoldStart = 0;
  }

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

  // ── PTT detection + WAKE WORD listening ──────────────────────────────────
  if (digitalRead(PTT_PIN) != LOW) {
    // Idle: one 10 ms mic frame in -> wake word engine ("Hey LearnPod"/"Hey Buddy")
    wakeStep();
    delay(2);
    return;
  }
  delay(50);  // debounce
  if (digitalRead(PTT_PIN) != LOW) return;

  // Measure how long PTT is held before release OR until PC_MODE_HOLD_MS
  unsigned long pressStart = millis();
  bool pcMode = false;
  bool btMode = false;

  while (digitalRead(PTT_PIN) == LOW) {
    unsigned long held = millis() - pressStart;
    if (held >= BT_HOLD_MS && !btMode && !pcMode) {
      // 8 s -> Bluetooth headphones pairing (overrides PC mode)
      btMode = true;
      oledMsg("BT Headphones", "will pair after", "you release...", FACE_LISTEN);
    } else if (held >= PC_MODE_HOLD_MS && !pcMode && !btMode) {
      // 3 s -> switch to PC mode UI ("keep holding: BT audio at 8 s")
      pcMode = true;
      oledMsg("PC Mode", "BLE starting...", "8s hold = BT audio", FACE_LISTEN);
    }
    delay(10);
  }

  // ── BLUETOOTH HEADPHONES PAIRING ──────────────────────────────────────────
  if (btMode) {
    btAudioPair();
    oledMsg("LearnPod Ready", "Hold PTT", "8s=BT audio 3s=PC", FACE_IDLE);
    return;
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
