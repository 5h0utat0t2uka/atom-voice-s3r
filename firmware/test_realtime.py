import json
import unittest
from unittest.mock import patch, MagicMock

from realtime import configuration, configure_device, initialize_token


class RealtimeTests(unittest.TestCase):
    def env(self):
        return {"SSID": "テスト", "PASS": " test-pass ", "DEVICE_TOKEN": "a" * 64,
                "REALTIME_TOKEN_URL": "https://device.example/api/realtime/token",
                "OPENAI_API_KEY": "never-send"}

    def test_only_device_configuration_is_sent(self):
        payload = configuration(self.env())
        self.assertLess(len(payload), 1536)
        self.assertNotIn(b"never-send", payload)
        self.assertEqual(json.loads(payload)["password"], " test-pass ")

    def test_plaintext_or_ambiguous_urls_are_rejected(self):
        for url in ["http://device.example/api/realtime/token", "https://user:secret@device.example/api/realtime/token",
                    "https://device.example/api/realtime/token?key=secret", "https://device.example/other"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                configuration({**self.env(), "REALTIME_TOKEN_URL": url})

    def test_no_credentials_sent_without_matching_sketch(self):
        with patch("realtime.write_all") as write, patch("realtime.read_line", side_effect=TimeoutError):
            with self.assertRaises(TimeoutError):
                configure_device(123, b"secret")
            write.assert_called_once_with(123, b"?\n")

    def test_serial_output_is_allowlisted(self):
        messages = []
        with patch("realtime.write_all"), patch("realtime.read_line", side_effect=[
                b"AVS3R_REALTIME_READY 3", b"secret", b"OK WIFI", b"OK TIME", b"MIC_WARMING", b"REALTIME_READY"]):
            configure_device(123, b"secret", report=messages.append)
        self.assertNotIn("secret", " ".join(messages))

    def test_old_playback_firmware_is_rejected_before_sending_secrets(self):
        with patch("realtime.write_all") as write, patch("realtime.read_line", return_value=b"AVS3R_REALTIME_READY 1"):
            with self.assertRaisesRegex(ValueError, "realtime-upload"):
                configure_device(123, b"secret")
            write.assert_called_once_with(123, b"?\n")

    def test_microphone_failure_is_reported_instead_of_waiting_for_ready(self):
        for code in (b"MIC_INIT", b"MIC_FORMAT", b"MIC_CAPTURE", b"MIC_TIMEOUT", b"WIFI_LOST", b"TOKEN", b"WS_CONNECT", b"REALTIME_API"):
            with self.subTest(code=code), patch("realtime.write_all"), patch("realtime.read_line", side_effect=[
                    b"AVS3R_REALTIME_READY 3", b"OK TIME", b"FAIL " + code]):
                with self.assertRaisesRegex(ValueError, code.decode("ascii")):
                    configure_device(123, b"secret")

    def test_existing_token_is_never_rotated_implicitly(self):
        with patch("realtime.subprocess.run") as run:
            initialize_token(self.env())
            run.assert_not_called()

    def test_new_token_goes_to_sops_stdin_not_arguments(self):
        with patch("realtime.subprocess.run", return_value=MagicMock(returncode=0)) as run:
            initialize_token({})
            call = run.call_args
            token = json.loads(call.kwargs["input"])
            self.assertEqual(len(token), 64)
            self.assertNotIn(token, " ".join(call.args[0]))
            self.assertIn("--value-stdin", call.args[0])


if __name__ == "__main__":
    unittest.main()
