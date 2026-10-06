# Focus 3 reference and capture review

Reviewed the current `Kirisame-Nanoha/Lip_Camera_IP_Server` HEAD,
[`ec070f191f80d1325b6faa8d08d70bc4eb1cb126`](https://github.com/Kirisame-Nanoha/Lip_Camera_IP_Server/tree/ec070f191f80d1325b6faa8d08d70bc4eb1cb126).

## Mode selection and frame rate

[`camera.py`](https://github.com/Kirisame-Nanoha/Lip_Camera_IP_Server/blob/ec070f191f80d1325b6faa8d08d70bc4eb1cb126/src/camera.py#L122)
selects YUY2, prefers a reported format whose minimum frame rate is at least
30 FPS, and passes the selected format index to DirectShow. It takes width and
height from the format entry; it does not force 640x480. The selected maximum
frame rate is stored as camera information, not separately set by the Python
capture loop. The 60 Hz read loop polls the graph; it does not select a 60 FPS
camera format.

[`camera_pipeline.py`](https://github.com/Kirisame-Nanoha/Lip_Camera_IP_Server/blob/ec070f191f80d1325b6faa8d08d70bc4eb1cb126/src/camera_pipeline.py#L266)
rotates luminance 90 degrees CCW and resizes it to 320x480 for MJPEG delivery.
The server is limited to 30 FPS and the preview to 15 FPS. These are output
settings, not USB capture dimensions.

The Steam Frame USB descriptor reports one YUY2 frame mode: 640x481, interval
333333 (about 30 FPS), frame size 615680 bytes, bulk endpoint 0x81. Negotiation
returns those dimensions and a 32768-byte maximum payload. This matches the
reference's format-selection strategy. No alternative USB mode appears in the
reported descriptors.

Actual frame size still requires measurement. A complete 640x480 YUY2 image
would contain 614400 bytes. The direct reader reports completed frame sizes
and rejects mismatches rather than guessing a missing row or padding it.

## Activation and shutdown

[`tracker_control.py`](https://github.com/Kirisame-Nanoha/Lip_Camera_IP_Server/blob/ec070f191f80d1325b6faa8d08d70bc4eb1cb126/src/tracker_control.py#L390)
queries selector 2's length and uses SET_CUR without polling for command echoes
for HTC Lip Camera. Its Focus 3 commands are:

| Action | Command prefix | Delay afterwards |
| --- | --- | --- |
| Stream off | `50140000` | 250 ms |
| Stream on | `50140001` | 250 ms |
| IR on | `50a2000401801822460001000000000011` | 250 ms |
| IR off during shutdown | `50a2000401801822460001000000000003` | 250 ms |
| Stream off during shutdown | `50140000` | 250 ms |

The remaining bytes are zero-padded to the queried length. A comparison of the
reference's original pure command-generation methods against `tracker.c`
confirmed all five 64-byte commands and all five delays match exactly. The
Focus 3 branch does not run the legacy VFT's magic/sensor-register sequence.

## Capture lifecycle and transport

The worker opens/configures the DirectShow graph, constructs the tracker
controller (activating the camera), then starts capture when requested.
`camera.py` delivers complete YUY2 samples through a DirectShow sample grabber.
Neither it nor the controller reads or assembles raw UVC USB payloads. Underlying
USB probe/commit requests and scheduling are handled by the Windows driver;
their precise ordering is not established by the Python source alone.

On Steam Frame, libuvc capture stalled, but `--probe-bulk` received three
32768-byte transfers. Their prefixes began `0c0d`, `0c0d`, and `0c4d`: 12-byte
UVC headers carrying FID/PTS/SCR, with ERR set on the third payload. The partial
libuvc callback was 131024 bytes, exactly four times `(32768 - 12)`. These
observations establish byte delivery but do not establish a complete frame,
an incorrect resolution, or the cause of the ERR flag.

The Focus 3 direct backend uses the successful probe's synchronous read method
and assembles payloads using FID, PTS and EOF. It preserves partial data returned
with USB timeouts, handles header-only EOF and accepts the observed headers
without requiring their unset EOH bit. Incomplete/oversize frames and frames
with ERR are rejected by default. `--allow-uvc-errors` permits inspection of
full-size ERR-marked frames; it never permits incomplete frames. The original
libuvc backend remains available for comparison.

Protocol/image/decoder and simulated USB/HTTP tests validate the implementation.
Physical streaming, error rates, image size, orientation and performance on
Steam Frame still need hardware validation.

## Direct-reader hardware results and next tests

The `.4` default reader received 163840 bytes in ten seconds: five payloads,
forty read timeouts and no full frames. Capture-before-activation received four
32768-byte payloads in total and then stalled. The first three were logged
before stream-on; the old log does not establish the fourth payload's timing. Two of those
payloads carried ERR, and the total image data was 131024 bytes. There is too
little data to establish a complete frame at either 640x480 or 640x481.
Changing frame-size acceptance or allowing UVC ERR cannot make that missing
image data appear.

The `.5` diagnostic `--recommit-after-activation` repeats the negotiated UVC
commit after all three vendor activation writes/delays. This tests whether
activation clears an earlier streaming configuration. It uses libuvc's public
`uvc_stream_ctrl` API on the already-open stream without starting libuvc capture.
`--bulk-timeout 1000` separately tests fewer USB timeout cancellations than
the default 250 ms. Both hypotheses remain unverified on hardware. Their
simulated tests cover successful image delivery, failure cleanup and retained
partial timeout data.

Logs include the first twelve payload prefixes and read statuses/timestamps,
including data received with timeouts. This provides visibility into later
headers and frame boundaries that the earlier three-prefix log omitted.
