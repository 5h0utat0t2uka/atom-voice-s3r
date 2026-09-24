#!/usr/bin/env python3
"""Provision GPT-Live settings over USB and save them in the device's NVS."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import select
import subprocess
import termios
import time
import tty
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]


def credentials(env):
    ssid, password = env.get("SSID", ""), env.get("PASS", "")
    if not 1 <= len(ssid.encode("utf-8")) <= 32 or "\0" in ssid:
        raise ValueError("SSID は UTF-8 で1〜32バイト、NUL なしで設定してください。")
    if not 8 <= len(password) <= 63 or any(not 32 <= ord(c) <= 126 for c in password):
        raise ValueError("PASS は8〜63文字の ASCII 文字で設定してください。")
    return json.dumps({"ssid": ssid, "password": password}, ensure_ascii=False).encode("utf-8") + b"\n"


def read_line(fd, deadline):
    line = bytearray()
    while len(line) < 1024:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([fd], [], [], remaining)[0]:
            raise TimeoutError("USB の応答がタイムアウトしました。ポートと書き込んだスケッチを確認してください。")
        byte = os.read(fd, 1)
        if not byte:
            raise EOFError("USB 接続が切断されました。")
        if byte == b"\n":
            return bytes(line).rstrip(b"\r")
        line.extend(byte)
    raise ValueError("USB 応答の長さが上限を超えました。")


def write_all(fd, data):
    remaining = memoryview(data)
    deadline = time.monotonic() + 5
    while remaining:
        timeout = deadline - time.monotonic()
        if timeout <= 0 or not select.select([], [fd], [], timeout)[1]:
            raise TimeoutError("USB への送信がタイムアウトしました。")
        remaining = remaining[os.write(fd, remaining):]


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
        raise ValueError("DEVICE_TOKEN が未設定または不正です。just live-init を実行してください。")
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
        if line == b"AVS3R_LIVE_READY 2":
            break
        if line.startswith(b"AVS3R_LIVE_READY "):
            raise ValueError("ファームウェアを just live-upload で更新してください。")
    write_all(fd, payload)
    deadline = time.monotonic() + 90
    accepted = saved = False
    messages = {b"CONNECTING": "Wi-Fi 接続中…", b"OK WIFI": "Wi-Fi 接続: OK", b"OK TIME": "時刻同期: OK",
                b"OK CONFIG_SAVED": "本体への設定保存: OK"}
    while True:
        line = read_line(fd, deadline)
        if line in {b"FAIL BUSY", b"FAIL MEMORY", b"FAIL CONFIG_TIMEOUT"}:
            raise ValueError(line.decode("ascii"))
        if line == b"CONFIG_ACCEPTED":
            accepted = True
            continue
        # Boot auto-connect can emit READY/failure before our request is handled.
        if not accepted:
            continue
        if line == b"OK CONFIG_SAVED":
            saved = True
        if line == b"LIVE_READY" and saved:
            report("設定・保存完了。次回の起動時は自動接続します。ボタン1回で会話開始、もう1回で終了します（最大4分・API 利用料金が発生）。LIVE_LISTENING の表示後に話しかけてください。")
            return
        if line in messages:
            report(messages[line])
        elif line in {b"FAIL CONFIG", b"FAIL WIFI", b"FAIL WIFI_LOST", b"FAIL TIME", b"FAIL CONFIG_STORAGE"}:
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
    parser.add_argument("action", choices=("init", "configure"))
    parser.add_argument("port", nargs="?")
    args = parser.parse_args()
    if args.action == "init":
        initialize_token(os.environ)
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
