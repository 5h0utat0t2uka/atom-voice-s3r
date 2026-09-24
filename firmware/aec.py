#!/usr/bin/env python3
"""Prepare and collect an on-device AEC comparison; never call an audio API."""

import argparse
import fcntl
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import termios
import tty
import wave

from capture import SerialReader, checksum
from speech import audio_header, validate_pcm

ROOT = Path(__file__).resolve().parents[1]
RATE = 16000
MAX_BYTES = RATE * 6 * 27


def prepare():
    pcm = ROOT / "build/speech/speech.pcm"
    if not pcm.exists():
        raise ValueError("保存済み音声がありません。先に just speech-generate を実行してください。")
    data = pcm.read_bytes()
    validate_pcm(data)
    # Use macOS's maintained sample-rate converter, with anti-alias filtering.
    # Keep the original 24 kHz asset; this conversion is only for the AEC test.
    with tempfile.TemporaryDirectory(prefix="avs3r-resample-") as temporary:
        source = Path(temporary) / "source.wav"
        converted = Path(temporary) / "converted.wav"
        with wave.open(str(source), "wb") as wav:
            wav.setparams((1, 2, 24000, 0, "NONE", "not compressed"))
            wav.writeframes(data)
        subprocess.run(["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000",
                        "-c", "1", "-r", "127", str(source), str(converted)], check=True)
        with wave.open(str(converted), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, RATE):
                raise ValueError("サンプルレート変換後の形式が不正です。")
            data = wav.readframes(wav.getnframes())
    header = audio_header(data, RATE)
    sketch = ROOT / "build/aec/aec_check"
    sketch.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / "firmware/aec_check/aec_check.ino", sketch / "aec_check.ino")
    shutil.copyfile(ROOT / "firmware/audio_hardware.h", sketch / "audio_hardware.h")
    shutil.copyfile(ROOT / "firmware/atom_voice_s3r_check/sketch.yaml", sketch / "sketch.yaml")
    (sketch / "generated_audio.h").write_text(header, encoding="ascii")
    print(f"AEC 比較スケッチを準備しました（API 呼び出しなし）: {sketch}")


def receive(fd, timeout=60, report=print):
    reader = SerialReader(fd, timeout)
    logs = []
    requested = False
    recording = False
    stats = None
    os.write(fd, b"?\n")
    while True:
        line = reader.line()
        if line.startswith(b"AEC_PCM_BEGIN "):
            if not recording or stats is None:
                raise ValueError("要求・計測結果のない音声転送です。")
            parts = line.split()
            if len(parts) != 5:
                raise ValueError("音声ヘッダーが不正です。")
            length, rate, channels = map(int, parts[1:4])
            digest = int(parts[4], 16)
            if rate != RATE or channels != 3 or not 0 < length <= MAX_BYTES or length % 6:
                raise ValueError("音声サイズ・形式が不正です。")
            if stats["frames"] != length // 6 or stats["expected"] != length // 6:
                raise ValueError("録音が予定のフレーム数に達していません。")
            data = reader.read(length)
            if reader.read(len(b"\nAEC_PCM_END\n")) != b"\nAEC_PCM_END\n" or checksum(data) != digest:
                raise ValueError("音声転送のチェックサムまたは終端が不正です。WAV は保存しません。")
            return data, "\n".join(logs + [line.decode("ascii")]) + "\n"
        message = line.decode("utf-8", errors="replace")
        logs.append(message)
        if len(logs) > 200:
            raise ValueError("録音を受信できず、ログの上限に達しました。")
        report(message)
        if message.startswith("ERROR:"):
            raise ValueError(message)
        if message.startswith("AEC_READY:") and not requested:
            os.write(fd, b"d\n")
            requested = True
        elif message.startswith("AEC_RECORDING:"):
            if not requested or recording:
                raise ValueError("要求に対応しない録音開始です。")
            recording = True
            report("日本語は2回流れます。1回目は静かにし、2回目だけ声を重ねてください。")
        elif message.startswith("AEC_STATS "):
            if not recording:
                raise ValueError("録音開始前の計測結果です。")
            stats = dict(field.split("=", 1) for field in message.split()[1:])
            stats = {key: int(value) for key, value in stats.items()}
            if not {"frames", "expected", "wall_ms", "pcm_ms", "rx_overflows", "max_block_us"} <= stats.keys():
                raise ValueError("計測結果が不足しています。")


def save(data, logs, root):
    root.mkdir(parents=True, exist_ok=True)
    destination = Path(tempfile.mkdtemp(prefix="capture-", dir=root))
    slots = [[], [], []]
    peaks = [0, 0, 0]
    squares = [0, 0, 0]
    clipped = [0, 0, 0]
    for frame in struct.iter_unpack("<hhh", data):
        for channel, sample in enumerate(frame):
            slots[channel].append(sample)
            peaks[channel] = max(peaks[channel], abs(sample))
            squares[channel] += sample * sample
            clipped[channel] += sample in (-32768, 32767)
    names = ("raw", "reference", "clean")
    # Apply exactly the SAME constant gain to both listening copies. Independent
    # normalization would conceal echo attenuation. Preserve original samples too.
    gain = min(8.0, 26000 / max(1, peaks[0], peaks[2]))
    outputs = [(f"{name}.wav", samples) for name, samples in zip(names, slots)]
    outputs += [(f"{name}-listen.wav", [round(sample * gain) for sample in slots[channel]])
                for name, channel in (("raw", 0), ("clean", 2))]
    for name, samples in outputs:
        pcm = struct.pack(f"<{len(samples)}h", *samples)
        fd = os.open(destination / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as output, wave.open(output, "wb") as wav:
            wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
            wav.writeframes(pcm)
    for channel, name in enumerate(names):
        rms = math.sqrt(squares[channel] / (len(data) // 6))
        logs += f"{name}: peak={peaks[channel]} rms={rms:.2f} clipped={clipped[channel]}\n"
    logs += f"listening_copy_gain={gain:.6f} ({20 * math.log10(gain):.2f} dB, same for raw/clean)\n"
    fd = os.open(destination / "capture.txt", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        output.write(logs)
    return destination.resolve()


def collect(port):
    print("monitor を終了してください。録音は USB 経由で build/aec/ に保存します。")
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
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
        data, logs = receive(fd)
        destination = save(data, logs, ROOT / "build/aec")
        print(f"保存先: {destination}\nraw-listen.wav と clean-listen.wav を同じ再生音量で比較してください。")
    finally:
        if original is not None:
            try:
                os.write(fd, b"c\n")
            except OSError:
                pass
        try:
            if original is not None:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("prepare")
    sub.add_parser("capture").add_argument("port")
    args = parser.parse_args()
    os.umask(0o077)
    if args.action == "prepare":
        prepare()
    else:
        collect(args.port)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, EOFError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
    except KeyboardInterrupt:
        raise SystemExit("中止しました。")
