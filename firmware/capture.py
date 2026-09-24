#!/usr/bin/env python3
"""Save one explicitly requested PCM recording over USB; no third-party packages."""

import argparse
import fcntl
import os
from pathlib import Path
import select
import tempfile
import termios
import time
import tty
import wave


class SerialReader:
    def __init__(self, fd, timeout):
        self.fd = fd
        self.deadline = time.monotonic() + timeout
        self.buffer = bytearray()

    def read(self, size):
        while len(self.buffer) < size:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.fd], [], [], remaining)[0]:
                raise TimeoutError("USB capture timed out; no WAV was saved.")
            block = os.read(self.fd, 4096)
            if not block:
                raise EOFError("USB disconnected before the recording was complete.")
            self.buffer.extend(block)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def line(self):
        line = bytearray()
        while len(line) < 4096:
            byte = self.read(1)
            if byte == b"\n":
                return bytes(line).rstrip(b"\r")
            line.extend(byte)
        raise ValueError("Unexpectedly long serial header.")


def checksum(data):
    result = 2166136261
    for byte in data:
        result = ((result ^ byte) * 16777619) & 0xFFFFFFFF
    return result


def receive_recording(fd, timeout=90, report=print, dma=None):
    if dma not in (None, 128, 256):
        raise ValueError("DMA frames must be 128 or 256.")
    reader = SerialReader(fd, timeout)
    logs = []
    requested = False
    armed = False
    config_requested = False
    config_confirmed = dma is None
    os.write(fd, b"?\n")
    while True:
        line = reader.line()
        if line.startswith(b"PCM_BEGIN "):
            if not armed:
                raise ValueError("Unrequested PCM transfer.")
            parts = line.split()
            if len(parts) != 5:
                raise ValueError("Invalid PCM header.")
            length, rate, channels = map(int, parts[1:4])
            expected = int(parts[4], 16)
            if rate != 24000 or channels != 1 or not 0 < length <= 480000 or length % 2:
                raise ValueError("Unexpected PCM size/format.")
            data = reader.read(length)
            if reader.read(9) != b"\nPCM_END\n" or checksum(data) != expected:
                raise ValueError("PCM transfer integrity check failed; no WAV was saved.")
            return data, "\n".join(logs) + "\n" + line.decode("ascii") + "\n"
        message = line.decode("utf-8", errors="replace")
        logs.append(message)
        if len(logs) > 1000:
            raise ValueError("Too much serial output without a recording.")
        report(message)
        if message.startswith("ERROR:") or message == "EXPORT CANCELLED":
            raise ValueError(message)
        if message.startswith("DMA CONFIGURED "):
            if not config_requested or message != f"DMA CONFIGURED {dma}":
                raise ValueError("Unexpected DMA configuration acknowledgment.")
            config_confirmed = True
        elif message.startswith("Ready:") and not requested:
            if not config_requested and dma is not None:
                os.write(fd, b"1\n" if dma == 128 else b"2\n")
                config_requested = True
            elif config_confirmed:
                os.write(fd, b"d\n")
                requested = True
        elif message.startswith("EXPORT ARMED:"):
            if not requested:
                raise ValueError("Export armed before configuration was confirmed.")
            armed = True
            report("ボタンを押し、最初の約1秒は静かに、その後2〜3秒話してから離してください。")


def save_recording(data, logs, root):
    root.mkdir(parents=True, exist_ok=True)
    # Each capture has a private, unique directory. Never overwrite an earlier one.
    destination = Path(tempfile.mkdtemp(prefix="capture-", dir=root))
    for name, content in (("capture.wav", data), ("capture.txt", logs)):
        fd = os.open(destination / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output:
            if name.endswith(".wav"):
                with wave.open(output, "wb") as wav:
                    wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
                    wav.writeframes(content)
            else:
                output.write(content.encode("utf-8"))
    return destination.resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial port; close just monitor first")
    parser.add_argument("--dma", type=int, choices=(128, 256), default=128,
                        help="DMA frames for this recording (default: 128)")
    args = parser.parse_args()
    print("just monitor を終了しておいてください。音声は build/diagnostics/ にのみ保存します。")
    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    original = None
    try:
        # Prevent a second copy of this collector from using the same port.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        original = termios.tcgetattr(fd)
        tty.setraw(fd, termios.TCSANOW)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B115200
        settings[2] |= termios.CLOCAL | termios.CREAD
        settings[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        data, logs = receive_recording(fd, dma=args.dma)
        destination = save_recording(data, logs, Path(__file__).resolve().parents[1] / "build/diagnostics")
        print(f"保存先: {destination}\nMac で capture.wav を再生して比較してください。")
    finally:
        try:
            if original is not None:
                os.write(fd, b"c\n")  # Cancel an unconsumed request, including Ctrl+C.
        except OSError:
            pass
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
