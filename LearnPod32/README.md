# LearnPod32 — firmware for the **classic ESP32**

Everything about wiring, WiFi, wake word and PC mode is documented in the main
[README at the repo root](../README.md). Quickstart:

1. Arduino IDE → Board: **ESP32 Dev Module**
2. Tools → **Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)** ← required
3. Open `LearnPod32.ino` → Flash
4. Hold the **BOOT button ~2 s** while running → WiFi selector
5. Controls:
   - Tap PTT (≤1 s) → talk to the AI ("Hey LearnPod" works hands-free too)
   - Hold PTT 3 s → PC control mode (run `START_LEARNPOD_PC.bat` on the PC)
   - `ADD:ssid:password` over Serial Monitor → add a WiFi network

> Note: the sketch file lives in a folder with the same name (`LearnPod32`)
> because the Arduino IDE requires folder name = sketch name.
> The ESP32-C3 variant is `learnpod_c3_v5.ino` at the repo root.
> Server and PC-app files are shared by both boards and stay at the repo root.
