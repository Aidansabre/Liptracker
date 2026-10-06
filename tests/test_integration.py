"""Real capture callback/JPEG/HTTP/cleanup with a simulated USB camera."""
import io
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from PIL import Image

binary = sys.argv.pop(1)


class Integration(unittest.TestCase):
    def run_camera(self, mode="", options=()):
        log = Path(self.directory.name) / "usb.log"
        log.write_text("")
        events = log.with_suffix(".events")
        events.write_text("")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        environment = dict(os.environ, VFT_TEST_MODE=mode, VFT_TEST_LOG=str(log),
                           VFT_TEST_EVENTS=str(events))
        process = subprocess.Popen([binary, "-p", str(port), *options], env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, process)
        return process, port, log

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        process.stdout.close()
        process.stderr.close()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def jpeg(self, process, port):
        deadline = time.monotonic() + 5
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                self.assertIsNone(process.poll())
                if time.monotonic() >= deadline:
                    self.fail("HTTP server did not start")
                time.sleep(0.05)
        with sock:
            sock.settimeout(5)
            sock.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            with sock.makefile("rb") as stream:
                self.assertEqual(stream.readline(), b"HTTP/1.1 200 OK\r\n")
                headers = []
                while (line := stream.readline()) != b"\r\n":
                    self.assertTrue(line)
                    headers.append(line)
                self.assertIn(b"Content-Type: multipart/x-mixed-replace; boundary=frame\r\n", headers)
                self.assertEqual(stream.readline(), b"--frame\r\n")
                self.assertEqual(stream.readline(), b"Content-Type: image/jpeg\r\n")
                length = int(stream.readline().split(b":")[1])
                self.assertEqual(stream.readline(), b"\r\n")
                data = stream.read(length)
                self.assertEqual(len(data), length)
                self.assertEqual(stream.read(2), b"\r\n")
        image = Image.open(io.BytesIO(data))
        image.load()
        # stb encodes single-component input as an RGB JPEG with neutral chroma.
        self.assertIn(image.mode, ("L", "RGB"))
        if image.mode == "RGB":
            self.assertEqual(image.getextrema(), ((64, 64), (64, 64), (64, 64)))
        self.assertEqual(image.convert("L").getextrema(), (64, 64))
        return image

    def check_shutdown(self, log):
        commands = log.read_text().splitlines()
        self.assertEqual(commands[-2][:34], "50a2000401801822460001000000000003")
        self.assertEqual(commands[-1][:8], "50140000")

    def test_mjpeg_and_reconnection(self):
        process, port, log = self.run_camera()
        self.assertEqual(self.jpeg(process, port).size, (320, 480))
        self.assertEqual(self.jpeg(process, port).size, (320, 480))
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stderr.decode())
        self.assertIn(b"640x481 YUYV @ 30 fps", stderr)
        self.assertIn(b"max-frame=615680 max-payload=16384", stderr)
        self.assertIn(b"first callback: 640x481", stderr)
        self.check_shutdown(log)
        self.assertEqual(len(log.read_text().splitlines()), 5)
        self.assertEqual(log.with_suffix(".events").read_text().splitlines(),
                         ["commit", "stream-off", "stream-on", "ir-on", "start",
                          "stop", "ir-off", "stream-off", "close"])

    def test_raw_odd_height(self):
        process, port, log = self.run_camera(options=("-r",))
        self.assertEqual(self.jpeg(process, port).size, (640, 481))
        process.send_signal(signal.SIGTERM)
        process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0)
        self.check_shutdown(log)

    def test_diagnostics_does_not_activate(self):
        process, _, log = self.run_camera(options=("--diagnose",))
        _, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stderr.decode())
        self.assertIn(b"selector2-length=64", stderr)
        self.assertEqual(log.read_text(), "")

    def test_reject_unsupported_or_ambiguous(self):
        for mode in ("unknown", "ambiguous", "invalid-length", "missing-xu"):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode)
                process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1)
                self.assertEqual(log.read_text(), "")

    def test_cleanup_on_activation_and_stream_failure(self):
        for mode in ("control-failure", "start-failure"):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode)
                process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1)
                self.check_shutdown(log)
                self.assertEqual(log.with_suffix(".events").read_text().splitlines()[-1], "close")

    def test_commit_failure_does_not_activate(self):
        process, _, log = self.run_camera("commit-failure")
        _, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 1)
        self.assertIn(b"commit capture stream", stderr)
        self.assertEqual(log.read_text(), "")
        self.assertEqual(log.with_suffix(".events").read_text().splitlines(), ["commit"])

    def test_startup_callback_diagnostics(self):
        for mode in ("no-callbacks", "rejected-frames"):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode, options=("--startup-timeout", "1"))
                _, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1)
                self.assertIn(b"no usable startup frame in 1s", stderr)
                counts = re.search(rb"received=(\d+) rejected=(\d+) encoded=(\d+)", stderr)
                self.assertIsNotNone(counts)
                received, rejected, encoded = map(int, counts.groups())
                self.assertEqual(encoded, 0)
                if mode == "no-callbacks":
                    self.assertEqual((received, rejected), (0, 0))
                    self.assertNotIn(b"first callback", stderr)
                else:
                    self.assertGreater(received, 0)
                    self.assertEqual(rejected, received)
                    self.assertIn(b"stride=1280 bytes=614400 expected=615680", stderr)
                    self.assertEqual(stderr.count(b"rejected callback:"), 3)
                self.check_shutdown(log)

    def test_first_frame_can_take_longer_than_stall_deadline(self):
        process, port, log = self.run_camera("delayed-start")
        self.assertEqual(self.jpeg(process, port).size, (320, 480))
        process.send_signal(signal.SIGTERM)
        _, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stderr.decode())
        self.check_shutdown(log)

    def test_bulk_probe_data_and_partial_timeout(self):
        for mode in ("", "probe-partial"):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode, options=("--probe-bulk",))
                _, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr.decode())
                self.assertIn(b"bulk probe endpoint=0x81 read-size=16384", stderr)
                self.assertIn(b"prefix=0c82", stderr)
                expected_bytes = b"1536" if mode == "probe-partial" else b"49152"
                self.assertIn(b"bulk probe summary: reads=3 bytes=" + expected_bytes, stderr)
                self.assertNotIn(b"serving on", stderr)
                self.assertNotIn(b"first callback:", stderr)
                self.check_shutdown(log)
                self.assertEqual(log.with_suffix(".events").read_text().splitlines(),
                                 ["commit", "stream-off", "stream-on", "ir-on",
                                  "bulk-read", "bulk-read", "bulk-read", "ir-off", "stream-off", "close"])

    def test_bulk_probe_errors_and_empty_capture(self):
        for mode, message in (("probe-empty", b"bytes=0 timeouts="),
                              ("probe-pipe", b"LIBUSB_ERROR_PIPE"),
                              ("probe-descriptor-failure", b"bulk probe descriptors:"),
                              ("probe-invalid-endpoint", b"requires one bulk IN endpoint")):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode, options=("--probe-bulk", "--startup-timeout", "1"))
                _, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1, stderr.decode())
                self.assertIn(message, stderr)
                self.check_shutdown(log)
                self.assertEqual(log.with_suffix(".events").read_text().splitlines()[-1], "close")

    def test_capture_first_mjpeg_and_cleanup(self):
        process, port, log = self.run_camera("capture-first-required", options=("--capture-first",))
        self.assertEqual(self.jpeg(process, port).size, (320, 480))
        process.send_signal(signal.SIGTERM)
        _, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stderr.decode())
        self.assertIn(b"USB capture queued before Focus 3 activation", stderr)
        self.check_shutdown(log)
        self.assertEqual(log.with_suffix(".events").read_text().splitlines(),
                         ["commit", "start", "stream-off", "stream-on", "ir-on", "stop", "ir-off", "stream-off", "close"])

    def test_capture_first_failure_cleanup(self):
        for mode in ("start-failure", "control-failure"):
            with self.subTest(mode=mode):
                process, _, log = self.run_camera(mode, options=("--capture-first",))
                _, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 1, stderr.decode())
                events = log.with_suffix(".events").read_text().splitlines()
                if mode == "start-failure":
                    self.assertEqual(log.read_text(), "")
                    self.assertEqual(events, ["commit", "start", "close"])
                else:
                    self.check_shutdown(log)
                    self.assertEqual(events[-4:], ["stop", "ir-off", "stream-off", "close"])

    def test_stall_cleanup(self):
        process, _, log = self.run_camera("stall")
        _, stderr = process.communicate(timeout=7)
        self.assertEqual(process.returncode, 1)
        self.assertIn(b"no frame in 3s", stderr)
        self.check_shutdown(log)

    def test_busy_port_does_not_activate(self):
        with socket.socket(socket.AF_INET6) as listener:
            listener.bind(("::", 0))
            listener.listen()
            port = listener.getsockname()[1]
            log = Path(self.directory.name) / "busy.log"
            result = subprocess.run([binary, "-p", str(port)],
                                    env=dict(os.environ, VFT_TEST_LOG=str(log)),
                                    capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1)
            self.assertIn(b"HTTP listen", result.stderr)
            self.assertFalse(log.exists())


unittest.main()
