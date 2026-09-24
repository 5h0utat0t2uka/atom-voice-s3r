#!/usr/bin/env python3
"""Prepare/configure GPT-Live continuous conversation without persisting plaintext secrets."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
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
    if len(parsed.netloc) >= 256:
        raise ValueError("接続先ホスト名が長すぎます。")
    data.update(live_host=parsed.netloc, device_token=token)
    result = json.dumps(data, ensure_ascii=False).encode("utf-8") + b"\n"
    if len(result) > 1536:
        raise ValueError("設定のサイズが上限を超えました。")
    return result


def configure_device(fd, payload, report=print):
    write_all(fd, b"?\n")
    deadline = time.monotonic() + 10
    while True:
        line = read_line(fd, deadline)
        if line == b"AVS3R_LIVE_READY 1":
            break
        if line.startswith(b"AVS3R_LIVE_READY "):
            raise ValueError("ファームウェアを just live-upload で更新してください。")
    write_all(fd, payload)
    deadline = time.monotonic() + 90
    messages = {b"CONNECTING": "Wi-Fi 接続中…", b"OK WIFI": "Wi-Fi 接続: OK", b"OK TIME": "時刻同期: OK"}
    while True:
        line = read_line(fd, deadline)
        if line == b"LIVE_READY":
            report("設定完了。ボタン1回で会話開始、もう1回で終了します（最大4分・API 利用料金が発生）。LIVE_LISTENING の表示後に話しかけてください。")
            return
        if line in messages:
            report(messages[line])
        elif line in {b"FAIL CONFIG", b"FAIL CONFIG_TIMEOUT", b"FAIL MEMORY", b"FAIL WIFI", b"FAIL TIME", b"FAIL BUSY"}:
            raise ValueError(line.decode("ascii"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "configure"))
    parser.add_argument("port", nargs="?")
    args = parser.parse_args()
    if args.action == "prepare":
        sketch = ROOT / "build/live/live_chat"
        sketch.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "firmware/live_chat/live_chat.ino", sketch / "live_chat.ino")
        shutil.copyfile(ROOT / "firmware/audio_hardware.h", sketch / "audio_hardware.h")
        shutil.copyfile(ROOT / "firmware/live_chat/pcm_queue.h", sketch / "pcm_queue.h")
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
