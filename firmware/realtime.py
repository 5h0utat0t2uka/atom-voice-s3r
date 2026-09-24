#!/usr/bin/env python3
"""Prepare/configure Realtime push-to-talk without persisting plaintext secrets."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import termios
import time
import tty
from urllib.parse import urlsplit

from wifi import credentials, read_line, write_all

ROOT = Path(__file__).resolve().parents[1]


def configuration(env):
    data = json.loads(credentials(env))
    url = env.get("REALTIME_TOKEN_URL", "")
    parsed = urlsplit(url)
    if (parsed.scheme != "https" or not re.fullmatch(r"[A-Za-z0-9.-]+", parsed.netloc)
            or parsed.path != "/api/realtime/token" or parsed.query or parsed.fragment
            or len(url) >= 512):
        raise ValueError("REALTIME_TOKEN_URL は HTTPS の /api/realtime/token URL にしてください。")
    token = env.get("DEVICE_TOKEN", "")
    if not re.fullmatch(r"[a-f0-9]{64}", token):
        raise ValueError("DEVICE_TOKEN が未設定または不正です。just realtime-init を実行してください。")
    data.update(token_url=url, device_token=token)
    result = json.dumps(data, ensure_ascii=False).encode("utf-8") + b"\n"
    if len(result) > 1536:
        raise ValueError("設定のサイズが上限を超えました。")
    return result


def configure_device(fd, payload, report=print):
    write_all(fd, b"?\n")
    deadline = time.monotonic() + 10
    while True:
        line = read_line(fd, deadline)
        if line == b"AVS3R_REALTIME_READY 3":
            break
        if line.startswith(b"AVS3R_REALTIME_READY "):
            raise ValueError("ファームウェアを just realtime-upload で更新してください。")
    write_all(fd, payload)
    deadline = time.monotonic() + 150
    messages = {b"CONNECTING": "Wi-Fi 接続中…", b"OK WIFI": "Wi-Fi 接続: OK",
                b"OK TIME": "時刻同期: OK", b"MIC_WARMING": "マイク・音声セッション準備中（ボタンを離してください）…",
                b"OK SESSION": "音声セッション接続: OK"}
    while True:
        line = read_line(fd, deadline)
        if line == b"REALTIME_READY":
            report("設定完了。前面ボタンを押している間に音声を送信し、離すと回答を生成します（録音は最大10秒、API 利用料金が発生）。")
            return
        if line in messages:
            report(messages[line])
        elif line in {b"FAIL CONFIG", b"FAIL CONFIG_TIMEOUT", b"FAIL MEMORY", b"FAIL WIFI", b"FAIL TIME",
                      b"FAIL WIFI_LOST", b"FAIL MIC_INIT", b"FAIL MIC_FORMAT", b"FAIL MIC_CAPTURE", b"FAIL MIC_TIMEOUT",
                      b"FAIL TOKEN_INIT", b"FAIL TOKEN", b"FAIL TOKEN_FORMAT", b"FAIL WS_CONNECT", b"FAIL WS_READ",
                      b"FAIL WS_SEND", b"FAIL REALTIME_API", b"FAIL AUDIO_FORMAT", b"FAIL RESPONSE_TIMEOUT",
                      b"FAIL NETWORK_STOP_TIMEOUT", b"RECONNECT_REQUIRED"}:
            raise ValueError(line.decode("ascii"))


def initialize_token(env):
    existing = env.get("DEVICE_TOKEN")
    if existing:
        if not re.fullmatch(r"[a-f0-9]{64}", existing):
            raise ValueError("既存の DEVICE_TOKEN が不正です。自動上書きは行いません。")
        print("既存の DEVICE_TOKEN を使用します。")
        return
    # stdin avoids exposing the new secret in command arguments or shell history.
    result = subprocess.run(
        ["sops", "set", "--input-type", "dotenv", "--output-type", "dotenv", "--value-stdin",
         str(ROOT / ".enc.env"), '["DEVICE_TOKEN"]'],
        input=json.dumps(secrets.token_hex(32)).encode("ascii"), capture_output=True, check=False,
    )
    if result.returncode:
        raise ValueError("DEVICE_TOKEN の暗号化保存に失敗しました。SOPS の設定を確認してください。")
    print("DEVICE_TOKEN を生成し、.enc.env に暗号化して保存しました（値は表示しません）。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("init", "prepare", "configure"))
    parser.add_argument("port", nargs="?")
    args = parser.parse_args()
    if args.action == "init":
        initialize_token(os.environ)
        return
    if args.action == "prepare":
        sketch = ROOT / "build/realtime/realtime_playback"
        sketch.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "firmware/realtime_playback/realtime_playback.ino", sketch / "realtime_playback.ino")
        shutil.copyfile(ROOT / "firmware/atom_voice_s3r_check/sketch.yaml", sketch / "sketch.yaml")
        return
    if not args.port:
        parser.error("configure には USB ポートが必要です。")
    payload = configuration(os.environ)
    for name in ("SSID", "PASS", "DEVICE_TOKEN", "OPENAI_API_KEY"):
        os.environ.pop(name, None)
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        original = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B115200
        settings[2] |= termios.CLOCAL | termios.CREAD
        settings[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        termios.tcflush(fd, termios.TCIFLUSH)
        configure_device(fd, payload)
    finally:
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            os.close(fd)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, EOFError) as error:
        raise SystemExit(str(error)) from error
    except KeyboardInterrupt:
        raise SystemExit("中止しました。")
