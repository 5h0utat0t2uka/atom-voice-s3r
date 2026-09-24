import json
import ssl
import struct
import tempfile
import unittest
import wave
from pathlib import Path
from unittest.mock import patch, MagicMock

from speech import MAX_BYTES, RATE, audio_header, generate, validate_pcm


class SpeechTests(unittest.TestCase):
    def test_header_preserves_signed_little_endian_samples(self):
        samples = (-32768, -256, -1, 0, 1, 255, 32767)
        data = struct.pack("<7h", *samples)
        header = audio_header(data)
        values = header.split("{", 1)[1].split("}", 1)[0]
        self.assertEqual(tuple(map(int, values.split(","))), samples)

    def test_invalid_pcm_is_rejected(self):
        for data in (b"", b"\x01", bytes(MAX_BYTES + 2)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                validate_pcm(data)
        validate_pcm(bytes(MAX_BYTES))

    def test_existing_clip_never_calls_api(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            (destination / "speech.pcm").write_bytes(bytes(48))
            with patch("speech.http.client.HTTPSConnection") as connection:
                generate(destination)
                connection.assert_not_called()

    def test_saved_wav_contains_exact_api_samples_without_key(self):
        data = struct.pack("<4h", -32768, -1, 0, 32767)
        response = MagicMock(status=200)
        response.getheader.return_value = "audio/pcm"
        response.read.return_value = data
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            with patch.dict("os.environ", {"OPENAI_API_KEY": "test-secret-only"}), \
                    patch("speech.http.client.HTTPSConnection") as connection:
                connection.return_value.getresponse.return_value = response
                generate(destination)
                context = connection.call_args.kwargs["context"]
                self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
                self.assertTrue(context.check_hostname)
                connection.return_value.close.assert_called_once()
            self.assertEqual((destination / "speech.pcm").read_bytes(), data)
            with wave.open(str(destination / "speech.wav"), "rb") as wav:
                self.assertEqual((wav.getnchannels(), wav.getsampwidth(), wav.getframerate()), (1, 2, RATE))
                self.assertEqual(wav.readframes(wav.getnframes()), data)
            metadata = (destination / "speech.json").read_text()
            self.assertNotIn("test-secret-only", metadata)
            self.assertEqual(json.loads(metadata)["channels"], 1)

    def test_api_error_has_no_body_or_key_and_saves_nothing(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            with patch.dict("os.environ", {"OPENAI_API_KEY": "test-secret-only"}), \
                    patch("speech.http.client.HTTPSConnection") as connection:
                response = connection.return_value.getresponse.return_value
                response.status = 401
                with self.assertRaisesRegex(ValueError, "HTTP 401") as error:
                    generate(destination)
                response.read.assert_not_called()
                self.assertNotIn("test-secret-only", str(error.exception))
            self.assertEqual(list(destination.iterdir()), [])

    def test_certificate_failure_stops_without_retry_or_files(self):
        with tempfile.TemporaryDirectory() as root:
            destination = Path(root)
            with patch.dict("os.environ", {"OPENAI_API_KEY": "test-secret-only"}), \
                    patch("speech.http.client.HTTPSConnection") as connection:
                connection.return_value.request.side_effect = ssl.SSLCertVerificationError("untrusted certificate")
                with self.assertRaises(ssl.SSLCertVerificationError):
                    generate(destination)
                connection.return_value.request.assert_called_once()
                connection.return_value.getresponse.assert_not_called()
                connection.return_value.close.assert_called_once()
            self.assertEqual(list(destination.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
