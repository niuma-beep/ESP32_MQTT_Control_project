#!/usr/bin/env python3
"""
Voice AI gateway for ESP32-C3 clock.

ESP32-C3 POSTs raw PCM audio to /voice:
  Content-Type: application/octet-stream
  X-Audio-Format: pcm_s16le
  X-Sample-Rate: 8000

This gateway:
  1. Converts PCM to WAV in memory.
  2. Sends WAV to an OpenAI-compatible ASR endpoint.
  3. Sends recognized text to an OpenAI-compatible chat endpoint.
  4. Returns a strict JSON command to ESP32-C3.

Environment variables:
  VOICE_GATEWAY_HOST=0.0.0.0
  VOICE_GATEWAY_PORT=8000

  ASR_PROVIDER=api or local-whisper`r`n  ASR_URL=https://api.openai.com/v1/audio/transcriptions
  ASR_API_KEY=...
  ASR_MODEL=gpt-4o-mini-transcribe

  LLM_URL=https://api.deepseek.com/chat/completions
  LLM_API_KEY=...
  LLM_MODEL=deepseek-v4-flash

  VOICE_GATEWAY_MOCK=1   # Return a test command without calling AI APIs.
"""

from __future__ import annotations

import io
import json
import mimetypes
import os
import re
import sys
import tempfile
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib import request, error

HOST = os.getenv("VOICE_GATEWAY_HOST", "0.0.0.0")
PORT = int(os.getenv("VOICE_GATEWAY_PORT", "8000"))

ASR_PROVIDER = os.getenv("ASR_PROVIDER", "api").lower()
ASR_URL = os.getenv("ASR_URL", "https://api.openai.com/v1/audio/transcriptions")
ASR_API_KEY = os.getenv("ASR_API_KEY") or os.getenv("OPENAI_API_KEY", "")
ASR_MODEL = os.getenv("ASR_MODEL", "gpt-4o-mini-transcribe")

LLM_URL = os.getenv("LLM_URL", "https://api.deepseek.com/chat/completions")
LLM_API_KEY = os.getenv("LLM_API_KEY") or os.getenv("DEEPSEEK_API_KEY", "")
LLM_MODEL = os.getenv("LLM_MODEL", "deepseek-v4-flash")

LOCAL_ASR_MODEL = os.getenv("LOCAL_ASR_MODEL", "base")
LOCAL_ASR_DEVICE = os.getenv("LOCAL_ASR_DEVICE", "cpu")
LOCAL_ASR_COMPUTE_TYPE = os.getenv("LOCAL_ASR_COMPUTE_TYPE", "int8")
LOCAL_ASR_LANGUAGE = os.getenv("LOCAL_ASR_LANGUAGE", "zh")

MOCK = os.getenv("VOICE_GATEWAY_MOCK", "0") == "1"
MAX_AUDIO_BYTES = 160_000
_LOCAL_WHISPER_MODEL = None

SYSTEM_PROMPT = """You are a command parser for an ESP32 smart clock.
Return JSON only. Do not explain.
Supported actions:
1. set_brightness: {"action":"set_brightness","value":0-102}
2. set_display_mode: {"action":"set_display_mode","mode":"time|sensor|weather|waterfall|test"}
3. set_effect: {"action":"set_effect","effect":"normal|rainbow"}
4. set_color: {"action":"set_color","red":0-102,"green":0-102,"blue":0-102}
If the command is unclear, return {"action":"none"}.
"""


def pcm_to_wav(pcm: bytes, sample_rate: int) -> bytes:
    out = io.BytesIO()
    with wave.open(out, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return out.getvalue()


def post_json(url: str, api_key: str, payload: dict[str, Any]) -> dict[str, Any]:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }
    req = request.Request(url, data=data, headers=headers, method="POST")
    with request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read().decode("utf-8"))


def post_multipart(
    url: str,
    api_key: str,
    fields: dict[str, str],
    file_field: str,
    filename: str,
    file_data: bytes,
    content_type: str,
) -> dict[str, Any]:
    boundary = "----esp32voice" + uuid.uuid4().hex
    body = io.BytesIO()

    for name, value in fields.items():
        body.write(f"--{boundary}\r\n".encode())
        body.write(f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode())
        body.write(value.encode("utf-8"))
        body.write(b"\r\n")

    body.write(f"--{boundary}\r\n".encode())
    body.write(
        f'Content-Disposition: form-data; name="{file_field}"; filename="{filename}"\r\n'.encode()
    )
    body.write(f"Content-Type: {content_type}\r\n\r\n".encode())
    body.write(file_data)
    body.write(b"\r\n")
    body.write(f"--{boundary}--\r\n".encode())

    headers = {
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Authorization": f"Bearer {api_key}",
    }
    req = request.Request(url, data=body.getvalue(), headers=headers, method="POST")
    with request.urlopen(req, timeout=90) as resp:
        return json.loads(resp.read().decode("utf-8"))



def transcribe_audio_local(wav_data: bytes) -> str:
    global _LOCAL_WHISPER_MODEL
    try:
        from faster_whisper import WhisperModel
    except ImportError as exc:
        raise RuntimeError(
            "Local ASR requires faster-whisper. Install it with: pip install faster-whisper"
        ) from exc

    if _LOCAL_WHISPER_MODEL is None:
        print(
            f"[gateway] loading local ASR model={LOCAL_ASR_MODEL}, "
            f"device={LOCAL_ASR_DEVICE}, compute={LOCAL_ASR_COMPUTE_TYPE}"
        )
        _LOCAL_WHISPER_MODEL = WhisperModel(
            LOCAL_ASR_MODEL,
            device=LOCAL_ASR_DEVICE,
            compute_type=LOCAL_ASR_COMPUTE_TYPE,
        )

    tmp_path = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            tmp.write(wav_data)
            tmp_path = tmp.name

        segments, _info = _LOCAL_WHISPER_MODEL.transcribe(
            tmp_path,
            language=LOCAL_ASR_LANGUAGE or None,
            vad_filter=True,
        )
        text = "".join(segment.text for segment in segments).strip()
        if not text:
            raise RuntimeError("local ASR produced empty text")
        return text
    finally:
        if tmp_path:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

def transcribe_audio(wav_data: bytes) -> str:
    if ASR_PROVIDER in {"local", "local-whisper", "faster-whisper"}:
        return transcribe_audio_local(wav_data)

    if not ASR_API_KEY:
        raise RuntimeError("ASR_API_KEY is not set")

    result = post_multipart(
        ASR_URL,
        ASR_API_KEY,
        {"model": ASR_MODEL},
        "file",
        "voice.wav",
        wav_data,
        mimetypes.types_map.get(".wav", "audio/wav"),
    )
    text = result.get("text", "")
    if not text:
        raise RuntimeError(f"ASR response has no text: {result}")
    return text.strip()


def strip_json_fence(text: str) -> str:
    text = text.strip()
    match = re.search(r"```(?:json)?\s*(.*?)```", text, re.S)
    if match:
        return match.group(1).strip()
    return text


def normalize_command(cmd: dict[str, Any]) -> dict[str, Any]:
    action = cmd.get("action", "none")
    if action == "set_brightness":
        value = int(cmd.get("value", 32))
        return {"action": action, "value": max(0, min(102, value))}

    if action == "set_display_mode":
        mode = str(cmd.get("mode", "time"))
        if mode not in {"time", "sensor", "weather", "waterfall", "test"}:
            mode = "time"
        return {"action": action, "mode": mode}

    if action == "set_effect":
        effect = str(cmd.get("effect", "normal"))
        if effect not in {"normal", "rainbow"}:
            effect = "normal"
        return {"action": action, "effect": effect}

    if action == "set_color":
        return {
            "action": action,
            "red": max(0, min(102, int(cmd.get("red", 0)))),
            "green": max(0, min(102, int(cmd.get("green", 0)))),
            "blue": max(0, min(102, int(cmd.get("blue", 0)))),
        }

    return {"action": "none"}


def parse_command(text: str) -> dict[str, Any]:
    if not LLM_API_KEY:
        raise RuntimeError("LLM_API_KEY is not set")

    payload = {
        "model": LLM_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": text},
        ],
        "temperature": 0,
    }
    result = post_json(LLM_URL, LLM_API_KEY, payload)
    content = result["choices"][0]["message"]["content"]
    command = json.loads(strip_json_fence(content))
    return normalize_command(command)


class VoiceHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stdout.write("[gateway] " + fmt % args + "\n")

    def send_json(self, status: int, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path == "/health":
            self.send_json(200, {"ok": True})
        else:
            self.send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        if self.path != "/voice":
            self.send_json(404, {"error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > MAX_AUDIO_BYTES:
                self.send_json(413, {"error": "invalid audio size"})
                return

            sample_rate = int(self.headers.get("X-Sample-Rate", "8000"))
            audio_format = self.headers.get("X-Audio-Format", "pcm_s16le")
            if audio_format != "pcm_s16le":
                self.send_json(415, {"error": "unsupported audio format"})
                return

            pcm = self.rfile.read(length)
            print(f"[gateway] received {len(pcm)} bytes, {sample_rate} Hz")

            if MOCK:
                self.send_json(200, {"action": "set_display_mode", "mode": "weather"})
                return

            wav_data = pcm_to_wav(pcm, sample_rate)
            text = transcribe_audio(wav_data)
            print(f"[gateway] ASR: {text}")
            command = parse_command(text)
            print(f"[gateway] command: {command}")
            self.send_json(200, command)
        except error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            print(f"[gateway] upstream HTTP error: {detail}")
            self.send_json(502, {"error": "upstream http error", "detail": detail})
        except Exception as exc:
            print(f"[gateway] internal error: {exc}")
            self.send_json(500, {"error": str(exc)})


def main() -> None:
    server = ThreadingHTTPServer((HOST, PORT), VoiceHandler)
    print(f"[gateway] listening on http://{HOST}:{PORT}")
    print("[gateway] health check: http://127.0.0.1:%d/health" % PORT)
    print(f"[gateway] ASR provider: {ASR_PROVIDER}")
    if ASR_PROVIDER in {"local", "local-whisper", "faster-whisper"}:
        print(f"[gateway] local ASR model: {LOCAL_ASR_MODEL}")
    else:
        print(f"[gateway] ASR URL: {ASR_URL}")
    print(f"[gateway] LLM URL: {LLM_URL}")
    server.serve_forever()


if __name__ == "__main__":
    main()