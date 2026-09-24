import json
import unittest
from unittest.mock import patch

from wifi import READY, check, credentials


class WifiTests(unittest.TestCase):
    def test_credentials_preserve_unicode_quotes_and_spaces(self):
        env = {"SSID": "iPhoneの名前", "PASS": ' a"b\\cdef ', "OPENAI_API_KEY": "never-send"}
        payload = credentials(env)
        self.assertLess(len(payload), 512)
        self.assertEqual(json.loads(payload), {"ssid": env["SSID"], "password": env["PASS"]})
        self.assertNotIn(b"never-send", payload)

    def test_invalid_credentials_do_not_appear_in_errors(self):
        for ssid, password in [("", "password"), ("あ" * 11, "password"),
                               ("abc\0def", "password"), ("abc", "short"),
                               ("abc", "秘密パスワード"), ("abc", "x" * 64)]:
            with self.subTest(ssid_length=len(ssid), password_length=len(password)):
                with self.assertRaises(ValueError) as error:
                    credentials({"SSID": ssid, "PASS": password})
                self.assertNotIn(password, str(error.exception))

    def test_no_credentials_sent_without_firmware_handshake(self):
        with patch("wifi.write_all") as write, \
                patch("wifi.read_line", side_effect=TimeoutError):
            with self.assertRaises(TimeoutError):
                check(123, b"secret-payload")
            write.assert_called_once_with(123, b"?\n")

    def test_unknown_usb_output_is_not_logged(self):
        lines = [READY, b"secret-payload", b"OK WIFI", b"RSSI -50", b"OK DNS",
                 b"OK TIME", b"HTTP 200", b"OK HTTPS", b"WIFI_CHECK_DONE"]
        messages = []
        with patch("wifi.write_all") as write, patch("wifi.read_line", side_effect=lines):
            check(123, b"secret-payload", report=messages.append)
            self.assertEqual(write.call_args_list[1].args, (123, b"secret-payload"))
        self.assertNotIn("secret-payload", " ".join(messages))
        self.assertIn("HTTP 200", messages)

    def test_failed_stage_ends_check(self):
        with patch("wifi.write_all"), patch("wifi.read_line", side_effect=[READY, b"FAIL TIME"]):
            with self.assertRaisesRegex(ValueError, "^FAIL TIME$"):
                check(123, b"secret-payload")

    def test_wifi_failure_reports_only_numeric_reason(self):
        messages = []
        with patch("wifi.write_all"), patch("wifi.read_line", side_effect=[
                READY, b"DISCONNECT_REASON 201", b"FAIL WIFI"]):
            with self.assertRaisesRegex(ValueError, "^FAIL WIFI$"):
                check(123, b"secret-payload", report=messages.append)
        self.assertEqual(messages, ["DISCONNECT_REASON 201"])


if __name__ == "__main__":
    unittest.main()
