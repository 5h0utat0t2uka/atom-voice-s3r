import errno
import socket
import stat
import tempfile
import threading
import unittest
import wave
from pathlib import Path

from capture import checksum, receive_recording, save_recording


class CaptureTests(unittest.TestCase):
    def receive(self, payload, dma=None):
        client, device = socket.socketpair()

        def transmit():
            try:
                for offset in range(0, len(payload), 37):
                    device.sendall(payload[offset:offset + 37])
                device.shutdown(socket.SHUT_WR)
            except OSError as error:
                # Rejection tests intentionally close the peer before EOF.
                if error.errno not in (errno.EPIPE, errno.ENOTCONN, errno.ECONNRESET):
                    raise

        writer = threading.Thread(target=transmit)
        writer.start()
        try:
            return receive_recording(client.fileno(), timeout=2, report=lambda _: None, dma=dma)
        finally:
            client.close()
            writer.join(timeout=2)
            device.close()

    def packet(self, data, digest=None):
        digest = checksum(data) if digest is None else digest
        return (b"Ready: test\nEXPORT ARMED: test\nCapture: wall_ms=100\n"
                + f"PCM_BEGIN {len(data)} 24000 1 {digest:08X}\n".encode()
                + data + b"\nPCM_END\n")

    def test_fragmented_binary_roundtrip_and_private_wav(self):
        data = bytes(range(256)) * 4  # Includes newlines, nulls and non-UTF-8 bytes.
        received, logs = self.receive(self.packet(data))
        self.assertEqual(received, data)
        self.assertIn("wall_ms=100", logs)
        with tempfile.TemporaryDirectory() as root:
            destination = save_recording(received, logs, Path(root))
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((destination / "capture.wav").stat().st_mode), 0o600)
            with wave.open(str(destination / "capture.wav"), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, 24000))
                self.assertEqual(wav.readframes(wav.getnframes()), data)

    def test_bad_checksum_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "integrity"):
            self.receive(self.packet(b"\x01\x02", digest=0))

    def test_dma_reconfiguration_waits_for_ack_and_new_ready(self):
        # An old queued Ready before the acknowledgment must not arm capture.
        preamble = b"Ready: old\nReady: duplicate\nDMA CONFIGURED 256\n"
        data, logs = self.receive(preamble + self.packet(bytes(16)), dma=256)
        self.assertEqual(data, bytes(16))
        self.assertIn("DMA CONFIGURED 256", logs)

    def test_wrong_dma_ack_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "acknowledgment"):
            self.receive(b"Ready: old\nDMA CONFIGURED 128\n", dma=256)

    def test_export_without_dma_ack_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "before configuration"):
            self.receive(self.packet(bytes(16)), dma=256)

    def test_truncated_transfer_is_rejected(self):
        with self.assertRaises(EOFError):
            self.receive(self.packet(bytes(128))[:-30])

    def test_unbounded_header_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "size/format"):
            self.receive(b"Ready: test\nEXPORT ARMED: test\nPCM_BEGIN 99999999 24000 1 0\n")

    def test_no_response_times_out(self):
        client, device = socket.socketpair()
        try:
            with self.assertRaises(TimeoutError):
                receive_recording(client.fileno(), timeout=0.02, report=lambda _: None)
        finally:
            client.close()
            device.close()


if __name__ == "__main__":
    unittest.main()
