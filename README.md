# LearnPod v5 — Server Pipeline + BLE PC Mode

```
OLD:  ESP32 ─▶ ElevenLabs ─▶ Groq ─▶ ElevenLabs (3 HTTPS sessions, heavy RAM)
      PC mode: WiFi HTTP to learnpod_pc.py

NEW:  ESP32 ── ONE HTTPS ──▶ Render server ──▶ STT + LLM + TTS ──▶ audio back
      PC mode: Bluetooth LE (Nordic UART) to learnpod_ble_pc.py
```

## Files

| File | What it is |
|---|---|
| `learnpod_esp32_v6.ino` | Firmware for **classic ESP32** boards (Dev Module / D1 R32 / WROOM) |
| `learnpod_c3_v5.ino` | Firmware for ESP32-**C3** Super Mini |
| `server/main.py` | FastAPI backend — runs the full AI pipeline |
| `server/requirements.txt` | Python deps for Render |
| `server/render.yaml` | Optional Render Blueprint config |
| `server/.env.example` | Key names (local testing only; use Render env vars) |
| `learnpod_ble_pc.py` | Windows BLE PC controller (replaces `learnpod_pc.py`) |
| `START_LEARNPOD_PC.bat` | One-click PC launcher (auto-installs everything) |

---

## 0. Wiring

### 0a. Classic ESP32 (Dev Module / WROOM) — `learnpod_esp32_v6.ino`

| Module | Module pin | ESP32 pin |
|---|---|---|
| INMP441 mic (I2S in) | VCC / GND | 3V3 / GND |
| | SCK | **GPIO18** |
| | WS | **GPIO19** |
| | SD | **GPIO32** |
| | L/R | **GND** (selects Left channel — required) |
| MAX98357A amp (I2S out) | VIN / GND | 5V (or 3V3 — quieter) / GND |
| | BCLK | **GPIO26** |
| | LRC | **GPIO25** |
| | DIN | **GPIO23** |
| | GAIN | **GND** (12 dB) |
| | Speaker + / − | speaker terminals |
| SSD1306 OLED 128×64 (I2C) | VCC / GND | 3V3 / GND |
| | SDA | **GPIO21** |
| | SCL | **GPIO22** |
| PTT button | legs | **GPIO4** ↔ GND *(internal pull-up — no resistor)* |

Classic ESP32 bonus: the OLED sits on the normal I2C bus (21/22) — none of the
C3 boot-strapping tricks, and both I2S mic and amp get **their own port**, so
no driver swapping at all.

> The BOOT button (GPIO0) is the WiFi-selector button on this board: hold it on
> power-up for the WiFi Selector, exactly like the FLASH button on the C3.

### 0b. ESP32-C3 Super Mini — `learnpod_c3_v5.ino`

```
                        ESP32-C3 Super Mini
                    ┌─────────────────────────┐
                    │                         │
   INMP441 mic ────▶│ GPIO6 SCK               │───▶ OLED SDA = GPIO8
   ("clock")        │ GPIO7 WS                │───▶ OLED SCL = GPIO9
   ("ws")           │ GPIO5 SD                │      PTT btn = GPIO10 ↔ GND
                    │ GPIO4 BCLK  ◀──── MAX98357A amp
                    │ GPIO3 LRC   ◀────     "
                    │ GPIO2 DIN   ◀────     "
                    │ 3V3 / GND  → power for mic + OLED
                    └─────────────────────────┘
```

| Module | Module pin | ESP32-C3 pin |
|---|---|---|
| INMP441 mic (I2S in) | VCC | 3V3 |
| | GND | GND |
| | SCK | GPIO6 |
| | WS | GPIO7 |
| | SD | GPIO5 |
| | L/R | **GND** (selects Left channel — required) |
| MAX98357A amp (I2S out) | VIN | 5V (or 3V3 — quieter) |
| | GND | GND |
| | BCLK | GPIO4 |
| | LRC | GPIO3 |
| | DIN | GPIO2 |
| | GAIN | **GND** (12 dB) |
| | Speaker + / − | speaker terminals |
| SSD1306 OLED 128×64 (I2C) | VCC | 3V3 |
| | GND | GND |
| | SDA | GPIO8 |
| | SCL | GPIO9 |
| PTT button | one leg | GPIO10 |
| | other leg | GND *(input-pullup — no resistor needed)* |

Notes:
- OLED **SDA=8 / SCL=9** — yes, GPIO9 doubles as the built-in BOOT/FLASH button; it works because the display holds the line high. If the board ever fails to boot, unplug the OLED and reboot once.
- PTT is any momentary push-to-talk button; firmware uses the internal pull-up.
- I2S wires are short (<10 cm) and keep SCK/WS away from power rails — audio clicking usually means long or crossed I2S wires.
- GAIN floating = 9 dB, tied to GND = 12 dB, tied to VIN = 15 dB.

---

## 1. Deploy the server to Render

1. Put the `server/` folder in a GitHub repo.
2. Render dashboard → **New → Web Service** → connect the repo.
3. Settings:
   - **Language:** Python 3
   - **Build command:** `pip install -r requirements.txt`
   - **Start command:** `uvicorn main:app --host 0.0.0.0 --port $PORT`
   - **Health check path:** `/health`
4. **Environment variables** (Render → Environment):
   - `ELEVENLABS_API_KEY`
   - `ELEVENLABS_VOICE_ID`
   - `GROQ_API_KEY`
5. Deploy, then test from your PC:

```powershell
curl https://YOUR-SERVICE.onrender.com/health
curl https://YOUR-SERVICE.onrender.com/ -X POST -H "Content-Type: application/octet-stream" --data-binary test.wav
```

> **Free tier note:** instances sleep after ~15 min idle; the first request then
> takes up to ~60 s to wake. Either accept that, ping `/health` with a uptime
> monitor (e.g. cron-job.org every 10 min), or use the Starter plan.

### Server API

| Endpoint | In | Out |
|---|---|---|
| `POST /voice` | raw WAV (16 kHz mono 16-bit) | line 1: `{"transcript":…,"reply":…}` then raw PCM audio |
| `POST /stt` | raw WAV | `{"text": "…"}` |
| `POST /tts` | `{"text": "…"}` | line 1: `{"reply":…}` then raw PCM audio |
| `POST /ask` | `{"text": "…"}` (wake-word prompt) | LLM reply spoken: meta + PCM audio |
| `GET /health` | — | `{"ok": true}` |

Keys stay on the server. The ESP32 sends **no** API keys anymore.

---

## 2. PC side — the easy way (for everyone) 👉 just double-click

**`START_LEARNPOD_PC.bat`** — one double-click does it all:

1. Finds Python on the PC (or opens the download page if it's missing)
2. Installs the required components automatically — once, silently
3. Starts the Bluetooth controller with clear on-screen instructions

> Keep the black window open — closing it stops PC control.
> Nothing to type, ever.

---

### Power-user way (same thing, manual)

```powershell
pip install bleak pyautogui
python learnpod_ble_pc.py
```

It scans for `LearnPod-C3`, connects automatically, and answers commands.
Auto-reconnects if the device reboots.

### Supported voice commands (extend `handle_command()` freely)

`open chrome/notepad/calculator/youtube/spotify/explorer/edge…` ·
`search <query>` · `youtube <query>` / `play <query>` ·
`volume up/down/mute` · `play / pause / next track / previous track` ·
`screenshot` · `minimize all` · `lock pc` · `type <text>` ·
`shutdown` / `cancel shutdown` / `restart`

### BLE protocol (Nordic UART Service)

| | UUID | |
|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| TX (notify) | `6E400002-…` | device → PC |
| RX (write) | `6E400003-…` | PC → device |

Newline-terminated JSON frames both ways:

```
Device → PC: {"cmd":"exec","text":"open chrome"}
Ping:        {"cmd":"ping"}  →  {"cmd":"ping","ok":true,"message":"pong"}
PC → Device: {"cmd":"exec","ok":true,"message":"Opened Chrome"}
```

> Note: the **ESP32-C3 has Bluetooth LE only** (no Classic) — that's why the
> link is BLE GATT. Works fine with `bleak` on Windows.

---

## 3. Firmware

Both variants behave identically — pick the file matching your board:

**Classic ESP32 (Dev Module / WROOM):** `learnpod_esp32_v6.ino`

1. Open `learnpod_esp32_v6.ino`.
2. `SERVER_HOST` is already `learnpod-c3.onrender.com` — fine if that's your Render app.
3. **No API keys in the firmware.**
4. Board: `ESP32 Dev Module`.
5. ⚠️ **Partition Scheme: `Huge APP (3MB No OTA/1MB SPIFFS)`** — required:
   BLE + WiFi + TLS won't fit in the default 1.2 MB app partition.
6. Flash. Then follow the controls below.

**ESP32-C3 Super Mini:** `learnpod_c3_v5.ino`

1. Board: `ESP32C3 Dev Module`, enable **USB CDC On Boot**, Flash 4 MB.

Then:

- **Hold the BOOT/FLASH button on boot** → WiFi Selector.

### Controls (from the factory)

| Action | What happens |
|---|---|
| **Say "Hey LearnPod" / "Hey Buddy"** + your question (hands-free, always armed) | device answers through the server: STT → LLM → TTS |
| Short hold PTT (~1 s), release → AI mode | 🎙️ record → server full pipeline → speaker plays answer |
| Hold PTT 3 s+ → PC mode (BLE) | device advertises, PC app connects, hold PTT again to speak the command |
| Hold FLASH/BOOT button on boot | 📶 WiFi selector |

### Wake word details

- Always listening while idle (PTT button keeps working exactly as before).
- A ~2.5 s clip is uploaded on each speech burst; a false trigger is cheap and
  silent — the screen briefly shows "Not a wake word". Sensitivity is tuned
  inside `wakeStep()` (`WAKE_MIN_LEVEL`, `WAKE_RUN_FRAMES`).
- Render free-tier cold start applies to wake too — the first command after a
  long sleep is slow; a `/health` keep-alive ping fixes that.

### Usage flow for BLE PC mode

1. Start `START_LEARNPOD_PC.bat` on the PC (leave the window open).
2. On the device: hold PTT ~3 s → "PC Mode (BLE) — waiting for PC".
3. PC connects → "Hold PTT to speak command" → press & hold PTT, speak.
4. Command goes to the PC over BLE; result is spoken back via server TTS.

---

## Rollback / fallback

- Old firmware kept as `learnpod_c3 (4).ino` (direct-to-API, WiFi PC mode).
- If the Render service is down, both AI and PC voice modes show
  "Server fail / Server API err". WiFi selector and face UI are unchanged.
