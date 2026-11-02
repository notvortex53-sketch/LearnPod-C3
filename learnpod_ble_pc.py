#!/usr/bin/env python3
"""
LearnPod BLE PC Controller — Windows
Replaces the old learnpod_pc.py HTTP server with a Bluetooth LE link.

The ESP32-C3 advertises itself as "LearnPod-C3" exposing the standard
Nordic_UART_Service (NUS):

  service      6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  TX (notify)  6E400002-...   device -> PC
  RX (write)   6E400003-...   PC -> device

Protocol: newline-terminated JSON frames both ways.
  Device -> PC: {"cmd":"exec","text":"open chrome"}
  PC -> Device: {"cmd":"exec","ok":true,"message":"Opened Chrome"}

Setup:
  pip install bleak pyautogui
  python learnpod_ble_pc.py
"""

import asyncio
import ctypes
import json
import os
import subprocess
import sys
import webbrowser
from datetime import datetime
from urllib.parse import quote_plus

import pyautogui
from bleak import BleakClient, BleakScanner

pyautogui.FAILSAFE = False  # disable the mouse-corner panic feature

BLE_DEVICE_NAME = "LearnPod-C3"
NUS_TX_CHAR = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # device -> PC (notify)
NUS_RX_CHAR = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # PC -> device (write)

# Windows virtual key codes (safer than library key-name guessing)
VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF
VK_MEDIA_NEXT = 0xB0
VK_MEDIA_PREV = 0xB1
VK_MEDIA_STOP = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3

APP_ALIASES = {
    "chrome": "chrome",
    "google chrome": "chrome",
    "edge": "msedge",
    "notepad": "notepad",
    "calculator": "calc",
    "word": "winword",
    "excel": "excel",
    "paint": "mspaint",
    "explorer": "explorer",
    "file explorer": "explorer",
    "cmd": "cmd",
    "terminal": "cmd",
}


def press_vk(vk: int, times: int = 1) -> None:
    for _ in range(times):
        ctypes.windll.user32.keybd_event(vk, 0, 0, 0)
        ctypes.windll.user32.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP


def open_app(name: str):
    target = name.strip().lower()

    if target in ("youtube",):
        webbrowser.open("https://www.youtube.com")
        return "Opened YouTube"
    if target in ("spotify",):
        webbrowser.open("https://open.spotify.com")
        return "Opened Spotify"
    if target in ("google",):
        webbrowser.open("https://www.google.com")
        return "Opened Google"

    exe = APP_ALIASES.get(target)
    if exe:
        try:
            subprocess.Popen(["cmd", "/c", "start", "", exe], shell=True)
            return f"Opened {name.strip()}"
        except Exception as e:
            return f"Could not open {name}: {e}"

    # Try as URL
    if "." in target and " " not in target:
        webbrowser.open("https://" + target)
        return f"Opened {target}"

    return f"I don't know the app {name}"


def handle_command(text: str):
    """Dispatch a spoken/typed command -> (ok, message). Extend freely."""
    t = text.strip().lower()
    if not t:
        return False, "Empty command"

    if t == "volume up" or t == "louder":
        press_vk(VK_VOLUME_UP, 4)
        return True, "Volume up"
    if t == "volume down" or t == "quieter":
        press_vk(VK_VOLUME_DOWN, 4)
        return True, "Volume down"
    if t == "mute" or t == "volume mute" or t == "unmute":
        press_vk(VK_VOLUME_MUTE)
        return True, "Mute toggled"
    if t in ("play", "pause", "play pause", "media play pause", "resume", "continue"):
        press_vk(VK_MEDIA_PLAY_PAUSE)
        return True, "Play/Pause pressed"
    if t in ("next track", "next song", "skip"):
        press_vk(VK_MEDIA_NEXT)
        return True, "Next track"
    if t in ("previous track", "previous song", "back track"):
        press_vk(VK_MEDIA_PREV)
        return True, "Previous track"
    if t in ("stop", "stop music"):
        press_vk(VK_MEDIA_STOP)
        return True, "Media stopped"

    if t == "screenshot" or t == "take a screenshot":
        path = os.path.join(os.path.expanduser("~/Desktop"),
                            f"learnpod_{datetime.now():%Y%m%d_%H%M%S}.png")
        pyautogui.screenshot(path)
        return True, "Screenshot saved to Desktop"

    if t in ("minimise all", "minimize all", "show desktop"):
        pyautogui.hotkey("winleft", "m")
        return True, "Minimised all windows"

    if t == "lock pc" or t == "lock computer":
        ctypes.windll.user32.LockWorkStation()
        return True, "PC locked"

    if t.startswith("open "):
        return True, open_app(text[5:])

    if t.startswith("search ") or t.startswith("google "):
        query = text.split(" ", 1)[1]
        webbrowser.open(f"https://www.google.com/search?q={quote_plus(query)}")
        return True, f"Searching for {query}"

    if t.startswith("youtube ") or t.startswith("play "):
        query = text.split(" ", 1)[1]
        webbrowser.open(f"https://www.youtube.com/results?search_query={quote_plus(query)}")
        return True, f"Searching YouTube for {query}"

    if t.startswith("type "):
        pyautogui.typewrite(text[5:], interval=0.02)
        return True, f"Typed: {text[5:]}"

    if t in ("shutdown", "shut down"):
        subprocess.Popen(["shutdown", "/s", "/t", "30"])
        return True, "Shutting down in 30s (say 'cancel shutdown' to stop)"
    if t == "cancel shutdown":
        subprocess.Popen(["shutdown", "/a"])
        return True, "Shutdown cancelled"
    if t == "restart":
        subprocess.Popen(["shutdown", "/r", "/t", "10"])
        return True, "Restarting in 10s"

    return False, f"Unknown command: {text}"


class LearnPodBLE:
    def __init__(self):
        self.client: BleakClient | None = None
        self._buffer = bytearray()

    # device -> PC notifications
    async def _on_notify(self, _char, data: bytearray):
        self._buffer.extend(data)
        while b"\n" in self._buffer:
            line, _, rest = self._buffer.partition(b"\n")
            self._buffer = bytearray(rest)
            if not line.strip():
                continue
            try:
                msg = json.loads(line.decode("utf-8"))
            except Exception:
                print(f"  ! Unparseable frame: {line!r}")
                continue
            await self._dispatch(msg)

    async def _dispatch(self, msg: dict):
        cmd = msg.get("cmd", "")
        if cmd == "exec":
            text = msg.get("text", "")
            print(f"  < command: {text}")
            ok, message = handle_command(text)
            print(f"  > {message}")
            await self.send({"cmd": "exec", "ok": ok, "message": message})
        elif cmd == "ping":
            await self.send({"cmd": "ping", "ok": True, "message": "pong"})
        else:
            print(f"  ! Unknown cmd: {cmd}")

    async def send(self, obj: dict):
        if not self.client or not self.client.is_connected:
            return
        payload = (json.dumps(obj) + "\n").encode("utf-8")
        if len(payload) > 500:  # keep within one BLE packet
            msg = obj.get("message", "")
            obj["message"] = msg[:400]
            payload = (json.dumps(obj) + "\n").encode("utf-8")
        try:
            await self.client.write_gatt_char(NUS_RX_CHAR, payload, response=True)
        except Exception as e:
            print(f"  ! send failed: {e}")


async def run() -> None:
    print("=== LearnPod BLE PC Controller ===")
    print(f"Looking for Bluetooth device named '{BLE_DEVICE_NAME}'...")
    print("On the LearnPod: hold PTT ~3s to enter PC mode.\n")

    learnpod = LearnPodBLE()

    while True:
        device = await BleakScanner.find_device_by_name(BLE_DEVICE_NAME, timeout=15)
        if device is None:
            print("Device not advertising — will retry in 5s...")
            await asyncio.sleep(5)
            continue

        print(f"Found {BLE_DEVICE_NAME}. Connecting...")
        stop = asyncio.Event()
        try:
            async with BleakClient(
                device, disconnected_callback=lambda _c: stop.set()
            ) as client:
                learnpod.client = client
                learnpod._buffer = bytearray()
                print("Connected to LearnPod!")

                await client.start_notify(NUS_TX_CHAR, learnpod._on_notify)
                await learnpod.send({"cmd": "ping"})  # let device know we're here

                await stop.wait()
                print("Disconnected from LearnPod.")
        except Exception as e:
            print(f"Connection error: {e}")

        learnpod.client = None
        print("Rescanning in 3s...\n")
        await asyncio.sleep(3)


if __name__ == "__main__":
    if os.name != "nt":
        print("Designed for Windows (keybd_event / LockWorkStation).")
        sys.exit(1)
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        print("\nBye!")
