from pathlib import Path
import subprocess
import tempfile
import unittest


class LiveFirmwareTests(unittest.TestCase):
    def test_pcm_queue_boundaries_wraparound_and_concurrent_order(self):
        source = Path(__file__).parent / "tests/pcm_queue.cpp"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "queue"
            subprocess.run(["c++", "-std=c++17", "-pthread", "-O2", str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
