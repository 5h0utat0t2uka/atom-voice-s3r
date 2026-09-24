#!/usr/bin/env python3
"""Send SOPS Wi-Fi credentials over USB to the network test; never save them."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import select
import shutil
import termios
import time
import tty

ROOT = Path(__file__).resolve().parents[1]
READY = b"AVS3R_WIFI_READY 1"
MESSAGES = {
    b"CONNECTING": "Wi-Fi に接続しています…",
    b"OK WIFI": "Wi-Fi 接続・IP アドレス取得: OK",
    b"OK DNS": "DNS 名前解決: OK",
    b"OK TIME": "時刻同期: OK",
    b"OK HTTPS": "HTTPS（証明書検証あり）: OK",
    b"WIFI_CHECK_DONE": "疎通テスト完了。接続情報は本体の RAM にのみ保持します。",
}


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


def check(fd, payload, report=print):
    # Require the dedicated sketch's handshake before sending any credentials.
    write_all(fd, b"?\n")
    deadline = time.monotonic() + 10
    while read_line(fd, deadline) != READY:
        pass
    write_all(fd, payload)
    deadline = time.monotonic() + 90
    while True:
        line = read_line(fd, deadline)
        # Allow only known messages/numeric diagnostics; never echo arbitrary USB data.
        if line in MESSAGES:
            report(MESSAGES[line])
        elif re.fullmatch(rb"(?:RSSI -?\d{1,3}|HTTP \d{3}|DISCONNECT_REASON \d{1,3})", line):
            report(line.decode("ascii"))
        elif line in {b"FAIL CONFIG", b"FAIL CONFIG_TIMEOUT", b"FAIL WIFI", b"FAIL DNS",
                      b"FAIL TIME", b"FAIL HTTPS_INIT", b"FAIL HTTPS", b"FAIL HTTP_STATUS"}:
            raise ValueError(line.decode("ascii"))
        if line == b"WIFI_CHECK_DONE":
            return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "check"))
    parser.add_argument("port", nargs="?")
    args = parser.parse_args()
    if args.action == "prepare":
        sketch = ROOT / "build/wifi/wifi_check"
        sketch.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "firmware/wifi_check/wifi_check.ino", sketch / "wifi_check.ino")
        shutil.copyfile(ROOT / "firmware/atom_voice_s3r_check/sketch.yaml", sketch / "sketch.yaml")
        return
    if not args.port:
        parser.error("check には USB ポートが必要です。")
    payload = credentials(os.environ)
    for name in ("SSID", "PASS", "OPENAI_API_KEY"):
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
        check(fd, payload)
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
