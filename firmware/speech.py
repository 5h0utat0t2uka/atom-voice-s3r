#!/usr/bin/env python3
"""Generate one Japanese TTS clip, then prepare an offline Arduino playback sketch."""

import argparse
import http.client
import json
import os
from pathlib import Path
import shutil
import ssl
import struct
import wave

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = ROOT / "build/speech"
RATE = 24000
MAX_BYTES = RATE * 2 * 10
REQUEST = {
    "model": "gpt-4o-mini-tts",
    "voice": "marin",
    "input": "こんにちは。これはAIが生成した日本語の音声です。",
    "instructions": "自然で明瞭な日本語で、普通の速さで話してください。",
    "response_format": "pcm",
}


def validate_pcm(data, rate=RATE):
    if rate not in (16000, 24000) or not data or len(data) % 2 or len(data) > rate * 2 * 10:
        raise ValueError("PCM は 16/24 kHz / mono / PCM16、空でなく10秒以内である必要があります。")


def audio_header(data, rate=RATE):
    validate_pcm(data, rate)
    # Preserve signed little-endian samples exactly; no gain or resampling.
    samples = [sample[0] for sample in struct.iter_unpack("<h", data)]
    rows = [", ".join(map(str, samples[i:i + 16])) for i in range(0, len(samples), 16)]
    return ("#pragma once\n#include <stdint.h>\n"
            f"// AI-generated Japanese speech; PCM16 mono, {rate} Hz.\n"
            "static const int16_t speechAudio[] = {\n  "
            + ",\n  ".join(rows) + "\n};\n")


def generate(destination, force=False):
    pcm_path = destination / "speech.pcm"
    if pcm_path.exists() and not force:
        validate_pcm(pcm_path.read_bytes())
        print(f"保存済み音声を使用します（API 呼び出しなし）: {pcm_path}")
        return
    key = os.environ.pop("OPENAI_API_KEY", "").strip()
    if not key:
        raise ValueError("OPENAI_API_KEY がありません。just speech-generate を使用してください。")
    context = ssl.create_default_context()
    context.set_alpn_protocols(["http/1.1"])
    # Audited: create_default_context requires trusted certificates and hostname verification.
    # This rule flags every HTTPSConnection call, including verified TLS (Python docs: http.client.HTTPSConnection).
    # nosemgrep: python.lang.security.audit.httpsconnection-detected.httpsconnection-detected
    connection = http.client.HTTPSConnection("api.openai.com", timeout=60, context=context)
    try:
        connection.request("POST", "/v1/audio/speech",
                           body=json.dumps(REQUEST, ensure_ascii=False).encode("utf-8"),
                           headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
        response = connection.getresponse()
        if response.status != 200:
            # Do not print response bodies or headers that may contain sensitive data.
            raise ValueError(f"音声生成 API が HTTP {response.status} を返しました。再試行はしていません。")
        content_type = response.getheader("Content-Type", "").split(";", 1)[0].lower()
        if not (content_type.startswith("audio/") or content_type == "application/octet-stream"):
            raise ValueError("音声生成 API の応答が音声形式ではありません。")
        data = response.read(MAX_BYTES + 1)
    finally:
        connection.close()
    validate_pcm(data)
    destination.mkdir(parents=True, exist_ok=True)
    pcm_path.write_bytes(data)
    with wave.open(str(destination / "speech.wav"), "wb") as wav:
        wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(data)
    (destination / "speech.json").write_text(
        json.dumps({**REQUEST, "sample_rate": RATE, "channels": 1,
                    "sample_bytes": 2, "duration_seconds": len(data) / (RATE * 2)},
                   ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"AI 音声を保存しました: {destination / 'speech.wav'} ({len(data) / (RATE * 2):.2f}秒)")


def prepare(destination):
    data = (destination / "speech.pcm").read_bytes()
    header = audio_header(data)
    sketch = destination / "speech_playback"
    sketch.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "firmware/speech_playback/speech_playback.ino", sketch / "speech_playback.ino")
    # Share the existing board options and pinned libraries, without a second profile to maintain.
    shutil.copyfile(ROOT / "firmware/atom_voice_s3r_check/sketch.yaml", sketch / "sketch.yaml")
    (sketch / "generated_audio.h").write_text(header, encoding="ascii")
    print(f"再生専用スケッチを準備しました: {sketch}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("generate", "prepare"))
    parser.add_argument("--force", action="store_true", help="音声を再生成する（API 利用料金が発生）")
    args = parser.parse_args()
    os.umask(0o077)
    if args.action == "generate":
        generate(DESTINATION, force=args.force)
    else:
        prepare(DESTINATION)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, http.client.HTTPException) as error:
        raise SystemExit(str(error)) from error
    except KeyboardInterrupt:
        raise SystemExit("中止しました。")
