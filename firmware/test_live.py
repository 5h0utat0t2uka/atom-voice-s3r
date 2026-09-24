import json
import unittest
from unittest.mock import patch

from live import configuration, configure_device


class LiveTests(unittest.TestCase):
    def env(self):
        return {"SSID": "テスト", "PASS": " test-pass ", "DEVICE_TOKEN": "a" * 64,
                "REALTIME_TOKEN_URL": "https://device.example/api/realtime/token",
                "OPENAI_API_KEY": "never-send"}

    def test_only_device_configuration_sent_to_same_host(self):
        data = json.loads(configuration(self.env()))
        self.assertEqual(set(data), {"ssid", "password", "device_token", "live_host"})
        self.assertEqual(data["live_host"], "device.example")
        self.assertEqual(data["password"], " test-pass ")

    def test_no_credentials_before_expected_handshake(self):
        with patch("live.write_all") as write, patch("live.read_line", side_effect=TimeoutError):
            with self.assertRaises(TimeoutError):
                configure_device(123, b"secret")
            write.assert_called_once_with(123, b"?\n")

    def test_configuration_does_not_start_recording_or_print_untrusted_output(self):
        messages = []
        with patch("live.write_all") as write, patch("live.read_line", side_effect=[
                b"AVS3R_LIVE_READY 1", b"secret", b"OK WIFI", b"OK TIME", b"LIVE_READY"]):
            configure_device(123, b"payload", messages.append)
            self.assertEqual([c.args[1] for c in write.call_args_list], [b"?\n", b"payload"])
        self.assertNotIn("secret", " ".join(messages))

    def test_ambiguous_or_insecure_hosts_are_rejected(self):
        for url in ["http://device.example/api/realtime/token", "https://user@device.example/api/realtime/token",
                    "https://device.example/api/realtime/token?key=secret", "https://device.example/other"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                configuration({**self.env(), "REALTIME_TOKEN_URL": url})
