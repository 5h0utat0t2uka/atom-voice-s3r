import errno
from pathlib import Path
import socket
import stat
import struct
import tempfile
import threading
import unittest
import wave

from capture import checksum
from aec import receive, save


class AecTests(unittest.TestCase):
    def packet(self, data, digest=None, expected=None, overflows=0):
        frames = len(data) // 6
        expected = frames if expected is None else expected
        digest = checksum(data) if digest is None else digest
        return (b"AEC_READY: test\nAEC_RECORDING: test\n"
                + f"AEC_STATS frames={frames} expected={expected} wall_ms=10 pcm_ms=10 rx_overflows={overflows} max_block_us=10000\n".encode()
                + f"AEC_PCM_BEGIN {len(data)} 16000 3 {digest:08X}\n".encode()
                + data + b"\nAEC_PCM_END\n")

    def receive(self, payload):
        client, device = socket.socketpair()

        def transmit():
            try:
                for offset in range(0, len(payload), 17):
                    device.sendall(payload[offset:offset + 17])
                device.shutdown(socket.SHUT_WR)
            except OSError as error:
                if error.errno not in (errno.EPIPE, errno.ENOTCONN, errno.ECONNRESET):
                    raise

        writer = threading.Thread(target=transmit)
        writer.start()
        try:
            return receive(client.fileno(), timeout=2, report=lambda _: None)
        finally:
            client.close()
            writer.join(timeout=2)
            device.close()

    def test_fragmented_capture_preserves_raw_reference_clean_and_private_permissions(self):
        raw = [-32768, 0, 1234, 32767]
        reference = [123, -321, 0, 17]
        clean = [-100, 0, 123, 400]
        pcm = b"".join(struct.pack("<hhh", *frame) for frame in zip(raw, reference, clean))
        received, logs = self.receive(self.packet(pcm))
        self.assertEqual(received, pcm)
        with tempfile.TemporaryDirectory() as root:
            destination = save(received, logs, Path(root))
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o700)
            for name, samples in (("raw", raw), ("reference", reference), ("clean", clean)):
                name += ".wav"
                self.assertEqual(stat.S_IMODE((destination / name).stat().st_mode), 0o600)
                with wave.open(str(destination / name), "rb") as wav:
                    self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 16000))
                    self.assertEqual(wav.readframes(wav.getnframes()), struct.pack("<4h", *samples))
            self.assertEqual(stat.S_IMODE((destination / "capture.txt").stat().st_mode), 0o600)
            self.assertIn("raw: peak=32768", (destination / "capture.txt").read_text())

    def test_listening_copies_use_identical_gain_without_clipping_or_independent_normalization(self):
        pcm = struct.pack("<hhhhhh", 1000, 30000, 100, -2000, -30000, -200)
        with tempfile.TemporaryDirectory() as root:
            destination = save(pcm, "", Path(root))
            for name, expected in (("raw-listen", (8000, -16000)), ("clean-listen", (800, -1600))):
                with wave.open(str(destination / f"{name}.wav"), "rb") as wav:
                    self.assertEqual(struct.unpack("<hh", wav.readframes(2)), expected)
            self.assertIn("listening_copy_gain=8.000000", (destination / "capture.txt").read_text())

    def test_corruption_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "チェックサム"):
            self.receive(self.packet(bytes(18), digest=0))

    def test_short_capture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "予定のフレーム"):
            self.receive(self.packet(bytes(18), expected=10))

    def test_unrequested_capture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "要求"):
            self.receive(b"AEC_PCM_BEGIN 18 16000 3 0\n")

    def test_bad_format_and_excessive_length_are_rejected(self):
        for header in (b"18 24000 3 0", b"18 16000 2 0", b"99999999 16000 3 0"):
            packet = self.packet(bytes(18)).split(b"AEC_PCM_BEGIN")[0]
            with self.subTest(header=header), self.assertRaisesRegex(ValueError, "サイズ・形式"):
                self.receive(packet + b"AEC_PCM_BEGIN " + header + b"\n")

    def test_overflow_evidence_is_preserved_for_diagnosis(self):
        data, logs = self.receive(self.packet(bytes(18), overflows=2))
        self.assertEqual(len(data), 18)
        self.assertIn("rx_overflows=2", logs)

    def test_truncated_transfer_and_device_error_are_rejected(self):
        with self.assertRaises(EOFError):
            self.receive(self.packet(bytes(126))[:-30])
        with self.assertRaisesRegex(ValueError, "CODEC_INIT"):
            self.receive(b"ERROR: CODEC_INIT_OR_READBACK\n")

    def test_no_response_times_out(self):
        client, device = socket.socketpair()
        try:
            with self.assertRaises(TimeoutError):
                receive(client.fileno(), timeout=0.02, report=lambda _: None)
        finally:
            client.close()
            device.close()


if __name__ == "__main__":
    unittest.main()
