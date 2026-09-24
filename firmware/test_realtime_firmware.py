"""Run the sketch's actual state transitions with deterministic audio/network fakes."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class FirmwareStateTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("clang++"), "clang++ is required for native firmware tests")
    def test_capture_streaming_and_usage(self):
        source = (ROOT / "realtime_playback/realtime_playback.ino").read_text()
        # Compile production functions, not a second implementation of the state machine.
        sections = [
            source[source.index("constexpr size_t sampleRate"):source.index("bool stopNetwork()")],
            source[source.index("void clearRecording()"):source.index("bool prepareMic()")],
            source[source.index("double tokenCount("):source.index("bool sessionLoop(")],
            source[source.index("bool beginTurn("):source.index("void setup()")],
            source[source.index("bool sessionLoop("):source.index("void networkTask(")],
        ]
        harness = (ROOT / "tests/realtime.cpp.in").read_text()
        for index, section in enumerate(sections):
            harness = harness.replace(f"// PRODUCTION_{index}", section)
        with tempfile.TemporaryDirectory(prefix="avs3r-test-") as directory:
            cpp = Path(directory) / "realtime.cpp"
            binary = Path(directory) / "realtime"
            cpp.write_text(harness)
            subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
