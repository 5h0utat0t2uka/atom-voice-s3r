import json
import unittest
from unittest.mock import patch, MagicMock

from live import configuration, configure_device, credentials, initialize_token


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
                b"AVS3R_LIVE_READY 2", b"secret", b"CONFIG_ACCEPTED", b"OK WIFI", b"OK TIME", b"OK CONFIG_SAVED", b"LIVE_READY"]):
            configure_device(123, b"payload", messages.append)
            self.assertEqual([c.args[1] for c in write.call_args_list], [b"?\n", b"payload"])
        self.assertNotIn("secret", " ".join(messages))

    def test_ambiguous_or_insecure_hosts_are_rejected(self):
        for url in ["http://device.example/api/realtime/token", "https://user@device.example/api/realtime/token",
                    "https://device.example/api/realtime/token?key=secret", "https://device.example/other"]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                configuration({**self.env(), "REALTIME_TOKEN_URL": url})

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

    def test_existing_token_is_never_rotated_implicitly(self):
        with patch("live.subprocess.run") as run:
            initialize_token(self.env())
            run.assert_not_called()

    def test_new_token_goes_to_sops_stdin_not_arguments(self):
        with patch("live.subprocess.run", return_value=MagicMock(returncode=0)) as run:
            initialize_token({})
            call = run.call_args
            token = json.loads(call.kwargs["input"])
            self.assertEqual(len(token), 64)
            self.assertNotIn(token, " ".join(call.args[0]))
            self.assertIn("--value-stdin", call.args[0])


    def test_configuration_failure_is_reported(self):
        for failure in (b"FAIL WIFI", b"FAIL TIME", b"FAIL MEMORY", b"FAIL CONFIG_STORAGE", b"FAIL WIFI_LOST"):
            with self.subTest(failure=failure), patch("live.write_all"), patch("live.read_line", side_effect=[
                    b"AVS3R_LIVE_READY 2", b"CONFIG_ACCEPTED", failure]):
                with self.assertRaisesRegex(ValueError, failure.decode("ascii")):
                    configure_device(123, b"payload")

    def test_old_firmware_cannot_claim_settings_were_saved(self):
        with patch("live.write_all") as write, patch("live.read_line", return_value=b"AVS3R_LIVE_READY 1"):
            with self.assertRaisesRegex(ValueError, "ファームウェア"):
                configure_device(123, b"secret")
            write.assert_called_once_with(123, b"?\n")

    def test_boot_logs_cannot_complete_provisioning(self):
        lines = [b"AVS3R_LIVE_READY 2", b"OK CONFIG_SAVED", b"LIVE_READY", b"FAIL WIFI",
                 b"CONFIG_ACCEPTED", b"LIVE_READY", b"OK CONFIG_SAVED", b"LIVE_READY"]
        with patch("live.write_all"), patch("live.read_line", side_effect=lines) as read:
            configure_device(123, b"payload", lambda message: None)
            self.assertEqual(read.call_count, len(lines))

    def test_ready_without_saved_confirmation_is_not_success(self):
        with patch("live.write_all"), patch("live.read_line", side_effect=[
                b"AVS3R_LIVE_READY 2", b"CONFIG_ACCEPTED", b"LIVE_READY", TimeoutError()]):
            with self.assertRaises(TimeoutError):
                configure_device(123, b"payload")
