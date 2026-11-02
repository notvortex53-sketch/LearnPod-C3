"""
LearnPod Server — full voice AI pipeline for the ESP32-C3 mini.

The device records a WAV, uploads it ONCE over HTTPS, and this server does
the whole pipeline:  ElevenLabs STT  ->  Groq LLM  ->  ElevenLabs TTS,
then streams raw 16 kHz PCM straight back. The device plays as it receives.

The ESP32 no longer needs the API keys, multipart headers, or JSON parsing
power that used to choke it.

Endpoints:
  POST /voice   raw WAV body  ->  {"transcript":..., "reply":...} meta line
                    followed by raw 16-bit 16 kHz mono PCM audio
  POST /stt     raw WAV body  ->  {"text": "..."} JSON           (BLE PC mode)
  POST /tts     JSON {"text": "..."}  ->  meta line + PCM audio (BLE PC mode)
  GET  /health  liveness probe for Render

Response wire format (voice/tts):
  line 1 = one line of JSON: {"transcript": "...", "reply": "..."} + "\\n"
  rest   = raw little-endian 16-bit PCM @ 16 kHz mono (play directly on MAX98357A)

Deploy on Render (free tier works — see README.md).
"""

import io
import json
import os
import re
import wave

import requests
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.concurrency import run_in_threadpool

load_dotenv()

# ─── Secrets / config (set these as Render Environment Variables) ────────────
ELEVEN_API_KEY = os.getenv("ELEVENLABS_API_KEY", "")
ELEVEN_VOICE_ID = os.getenv("ELEVENLABS_VOICE_ID", "")
ELEVEN_TTS_MODEL = os.getenv("ELEVEN_TTS_MODEL", "eleven_turbo_v2")
ELEVEN_TTS_FORMAT = os.getenv("ELEVEN_TTS_FORMAT", "pcm_16000")
GROQ_API_KEY = os.getenv("GROQ_API_KEY", "")
GROQ_MODEL = os.getenv("GROQ_MODEL", "qwen/qwen3-27b")

SYSTEM_PROMPT = (
    "You are LearnPod, a concise voice AI assistant. "
    "Always reply in 25 words or fewer. Be direct and clear."
)

MAX_UPLOAD = 4 * 1024 * 1024  # ~3 s of 16 kHz/16-bit was ~100 KB; leave headroom

app = FastAPI(title="LearnPod API", version="1.0.0")


# ═══════════════════════════════════════════════════════════════════════════════
# HELPERS
# ═══════════════════════════════════════════════════════════════════════════════

def _require_env():
    missing = [k for k, v in (
        ("ELEVENLABS_API_KEY", ELEVEN_API_KEY),
        ("ELEVENLABS_VOICE_ID", ELEVEN_VOICE_ID),
        ("GROQ_API_KEY", GROQ_API_KEY),
    ) if not v]
    if missing:
        raise HTTPException(500, f"Missing env var(s): {', '.join(missing)}")


def _validate_wav(data: bytes):
    """Exact-format check: must be a real 16 kHz mono 16-bit WAV (what the ESP32 sends)."""
    try:
        with wave.open(io.BytesIO(data)) as w:
            rate, ch, width = w.getframerate(), w.getnchannels(), w.getsampwidth()
    except Exception:
        raise HTTPException(400, "Body is not a valid WAV (RIFF) file")
    if (rate, ch, width) != (16000, 1, 2):
        raise HTTPException(400, f"Expected 16 kHz mono 16-bit WAV, got {rate} Hz, {ch} ch, {width*8} bit")


def _stt(wav_bytes: bytes) -> str:
    """ElevenLabs speech-to-text, same endpoint the device used to call directly."""
    r = requests.post(
        "https://api.elevenlabs.io/v1/speech-to-text",
        headers={"xi-api-key": ELEVEN_API_KEY},
        files={"file": ("audio.wav", io.BytesIO(wav_bytes), "audio/wav")},
        timeout=30,
    )
    if r.status_code != 200:
        raise HTTPException(502, f"ElevenLabs STT error {r.status_code}: {r.text[:200]}")
    return (r.json().get("text") or "").strip()


def _llm(user_text: str) -> str:
    """Groq chat completion. Qwen3 may emit <think>…</think> — strip it."""
    r = requests.post(
        "https://api.groq.com/openai/v1/chat/completions",
        headers={"Authorization": f"Bearer {GROQ_API_KEY}"},
        json={
            "model": GROQ_MODEL,
            "max_tokens": 80,
            "messages": [
                {"role": "system", "content": SYSTEM_PROMPT},
                {"role": "user", "content": user_text},
            ],
        },
        timeout=30,
    )
    if r.status_code != 200:
        raise HTTPException(502, f"Groq error {r.status_code}: {r.text[:200]}")
    content = (r.json()["choices"][0]["message"].get("content") or "").strip()
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.S | re.I).strip()
    return content


def _tts_start(text: str) -> requests.Response:
    """Open the ElevenLabs TTS stream (raw PCM). Caller iterates it."""
    r = requests.post(
        f"https://api.elevenlabs.io/v1/text-to-speech/{ELEVEN_VOICE_ID}/stream"
        f"?output_format={ELEVEN_TTS_FORMAT}",
        headers={"xi-api-key": ELEVEN_API_KEY, "Content-Type": "application/json"},
        json={
            "text": text,
            "model_id": ELEVEN_TTS_MODEL,
            "voice_settings": {"stability": 0.5, "similarity_boost": 0.75},
        },
        stream=True,
        timeout=(10, None),
    )
    if r.status_code != 200:
        body = r.raw.read(300, decode_content=True)
        raise HTTPException(502, f"ElevenLabs TTS error {r.status_code}: {body.decode(errors='replace')}")
    return r


def _meta_line(**fields) -> bytes:
    # One JSON line + newline: the ESP32 reads exactly one line, then streams PCM
    return (json.dumps(fields, ensure_ascii=False) + "\n").encode("utf-8")


def _pcm_chunks(tts_resp: requests.Response):
    yield from tts_resp.iter_content(chunk_size=1024)


# ═══════════════════════════════════════════════════════════════════════════════
# ENDPOINTS
# ═══════════════════════════════════════════════════════════════════════════════

@app.get("/")
async def root():
    return JSONResponse({"service": "learnpod-api", "model": GROQ_MODEL, "endpoints": ["/voice", "/stt", "/tts", "/health"]})


@app.get("/health")
async def health():
    return JSONResponse({"ok": True})


@app.post("/voice")
async def voice(request: Request):
    """AI assistant mode: WAV in -> transcript + reply meta + PCM audio out."""
    _require_env()

    wav = await request.body()
    if not wav:
        raise HTTPException(400, "Empty body — POST the raw WAV bytes")
    if len(wav) > MAX_UPLOAD:
        raise HTTPException(413, "Recording too large")
    _validate_wav(wav)

    # STT + LLM run in a worker thread so the event loop stays responsive
    try:
        transcript, reply = await run_in_threadpool(_voice_pipeline, wav)
    except HTTPException:
        raise

    def stream():
        tts = _tts_start(reply)
        yield _meta_line(transcript=transcript, reply=reply)
        yield from _pcm_chunks(tts)
        tts.close()

    return StreamingResponse(stream(), media_type="application/octet-stream")


def _voice_pipeline(wav: bytes):
    transcript = _stt(wav)
    if not transcript:
        raise HTTPException(422, "STT returned empty transcript")
    print(f"[SRV] Transcript: {transcript}")
    reply = _llm(transcript)
    print(f"[SRV] Reply: {reply}")
    if not reply:
        raise HTTPException(502, "LLM returned empty reply")
    return transcript, reply


@app.post("/stt")
async def stt(request: Request):
    """BLE PC mode: WAV in -> {"text": "..."} out (transcript becomes the PC command)."""
    _require_env()
    wav = await request.body()
    if not wav:
        raise HTTPException(400, "Empty body")
    if len(wav) > MAX_UPLOAD:
        raise HTTPException(413, "Recording too large")
    _validate_wav(wav)

    text = await run_in_threadpool(_stt, wav)
    print(f"[SRV] STT: {text}")
    return JSONResponse({"text": text})


@app.post("/tts")
async def tts(request: Request):
    """BLE PC mode: {"text": "..."} in -> meta line + PCM audio out."""
    _require_env()
    try:
        payload = await request.json()
    except Exception:
        raise HTTPException(400, "Body must be JSON")
    text = (payload.get("text") or "").strip()
    if not text:
        raise HTTPException(400, "Missing 'text'")

    def stream():
        tts_resp = _tts_start(text)
        yield _meta_line(reply=text)
        yield from _pcm_chunks(tts_resp)
        tts_resp.close()

    return StreamingResponse(stream(), media_type="application/octet-stream")
