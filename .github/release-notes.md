Focus 3 bulk-capture diagnostics for Steam Frame.

Hardware tests of `.1` and `.2` timed out without usable frames. The latest
USB log shows a zero-byte bulk completion followed by five-second transfer
timeouts. This release provides two ways to investigate that failure:

- `--capture-first` queues USB capture before sending the existing Focus 3
  sensor/IR activation sequence. This is an experimental startup option;
  default activation timing is unchanged.
- `--probe-bulk` reads the negotiated bulk endpoint directly, one transfer at
  a time, without libuvc frame parsing or MJPEG output. It reports statuses,
  byte counts, transfer prefixes and partial data returned with timeouts.

Both options print actual vendor command prefixes and shut down IR/stream
state when finished.

Download `vft-stream-focus3-steam-frame.tar.gz` for the static ARM64 executable,
helper scripts, complete application source and licenses. `SHA256SUMS` verifies
the package and standalone executable.

Extract the package and try capture with the alternative timing:

```sh
tar -xzf vft-stream-focus3-steam-frame.tar.gz
cd vft-stream-focus3
sudo ./vft-stream --tracker focus3 --capture-first 2>focus3-capture.log
cat focus3-capture.log
```

If it still stalls, collect direct USB-read results:

```sh
sudo ./vft-stream --tracker focus3 --probe-bulk --startup-timeout 5 2>focus3-bulk.log
cat focus3-bulk.log
```

The probe exits after three nonempty reads or its deadline. A zero exit status
only establishes that USB bytes arrived; it does not establish image decoding.
The two options cannot be combined. Stop other camera processes before testing.

GitHub Actions builds native and static ARM64 binaries, runs protocol/image
regressions and sanitizer checks, and runs fourteen simulated USB/HTTP
integration tests. These cover capture ordering, failure cleanup, direct bulk
reads, partial timeouts and missing data. Original VFT image output matches
the upstream fixture byte for byte.

This is a **hardware-test prerelease**. Successful camera streaming on Steam
Frame remains unverified. IR operation, orientation, sustained FPS, Baballonia
tracking quality and service plug/unplug behavior also need hardware validation.
