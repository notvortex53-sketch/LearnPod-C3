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
| `learnpod_c3_v5.ino` | Updated firmware (replace/supersede `learnpod_c3 (4).ino`) |
| `server/main.py` | FastAPI backend — runs the full AI pipeline |
| `server/requirements.txt` | Python deps for Render |
| `server/render.yaml` | Optional Render Blueprint config |
| `server/.env.example` | Key names (local testing only; use Render env vars) |
| `learnpod_ble_pc.py` | Windows BLE PC controller (replaces `learnpod_pc.py`) |

---

## 0. Wiring (ESP32-C3 Super Mini)

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

1. Open `learnpod_c3_v5.ino`.
2. Set `SERVER_HOST` to your Render URL host (e.g. `learnpod-api.onrender.com`).
3. **No API keys in the firmware anymore.**
4. Board: `ESP32C3 Dev Module`, enable **USB CDC On Boot**, Flash 4 MB.
5. Flash. Then:
   - **Hold PTT (~1 s), release → AI mode** (server full pipeline)
   - **Hold PTT 3 s+ → PC mode (BLE)**: device advertises, PC app connects,
     then hold PTT again to speak the command.

Usage flow for BLE PC mode:
1. Start `learnpod_ble_pc.py` on the PC (leave it running).
2. On the device: hold PTT ~3 s → "PC Mode (BLE) — waiting for PC".
3. PC connects → "Hold PTT to speak command" → press & hold PTT, speak.
4. Command goes to the PC over BLE; result is spoken back via server TTS.

---

## Rollback / fallback

- Old firmware kept as `learnpod_c3 (4).ino` (direct-to-API, WiFi PC mode).
- If the Render service is down, both AI and PC voice modes show
  "Server fail / Server API err". WiFi selector and face UI are unchanged.
