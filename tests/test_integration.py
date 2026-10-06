"""Real capture callback/JPEG/HTTP/cleanup with a simulated USB camera."""
import io
import os
from pathlib import Path
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
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        environment = dict(os.environ, VFT_TEST_MODE=mode, VFT_TEST_LOG=str(log))
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
        self.check_shutdown(log)
        self.assertEqual(len(log.read_text().splitlines()), 5)

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
