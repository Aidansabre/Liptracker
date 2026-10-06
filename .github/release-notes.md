Focus 3 stream-commit and USB-timeout diagnostics for Steam Frame.

The .4 direct reader also stalled on physical hardware. The default run
received 163840 bytes in ten seconds; capture-before-activation received four
32768-byte payloads before stream-on and then stalled. Neither run delivered
one complete image. This is not enough data to establish whether the camera
actually produces 640×480 or its advertised 640×481 frames.

This release adds controlled comparisons:

- `--recommit-after-activation` reapplies the same negotiated UVC streaming
  commit after the vendor activation commands, testing whether activation
  clears the earlier stream configuration.
- `--bulk-timeout 1000` changes the bulk read timeout from 250 ms to one second,
  testing whether repeated timeout cancellations contribute to the stall.
- Initial transfer logs now include statuses, timestamps, pending partial
  bytes and the first twelve payload prefixes.

These are diagnostic hypotheses, not verified hardware fixes. The original
VFT capture path and the Focus 3 activation bytes are preserved.

Download `vft-stream-focus3-steam-frame.tar.gz` for the static ARM64 executable,
source, helper scripts, reference review and license notices. Stop other camera
processes and unplug/reconnect the tracker before each comparison.

First run:

```sh
sudo ./vft-stream --tracker focus3 --recommit-after-activation 2>focus3-recommit.log
cat focus3-recommit.log
```

Then unplug/reconnect and run the timeout comparison separately:

```sh
sudo ./vft-stream --tracker focus3 --bulk-timeout 1000 2>focus3-timeout.log
cat focus3-timeout.log
```

Share both complete logs if streaming stalls. If it starts, connect to
`http://<Steam-Frame-IP>:8085/` and confirm image visibility/orientation.
Ctrl+C stops capture and attempts normal IR/stream shutdown. A custom timeout
can delay shutdown until the outstanding read finishes.

Native and static ARM64 builds, protocol/image/payload regressions and
23 simulated USB/HTTP integration tests pass, including sanitizer checks.
Tests cover activation clearing UVC configuration, recommit success/failure
with and without early capture, timeout data retention and option validation.
Published package/binary hashes are in `SHA256SUMS`.

This is a hardware-test prerelease. Successful streaming, actual dimensions,
IR behavior, sustained FPS, tracking quality and service plug/unplug behavior
still require physical validation.
