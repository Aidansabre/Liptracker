Focus 3: reference activation order, bulk endpoint halt and link diagnostics.

Every hardware run so far (`.1` to `.5`) received about four 32768-byte payloads
after the UVC commit and then stalled, whatever the commit/activation order.

Changes:

- The default order matches the reference app again: vendor activation
  (stream off, stream on, IR on), then UVC commit, then capture straight away.
  The `.2`–`.5` order remains available via `--capture-first` and
  `--recommit-after-activation`.
- CLEAR_FEATURE(HALT) is sent to the bulk streaming endpoint before the commit
  and after capture stops, as Windows and Linux uvcvideo do. libuvc never sends it.
- Startup prints the USB link speed, endpoint packet size and required data rate
  (about 18.5 MB/s). It warns when the tracker is not on a high-speed link.
- The first payloads carrying UVC ERR are followed by the camera's stream error
  code (for example `output buffer overrun`).
- The HTTP server falls back to IPv4 when the kernel has no IPv6.

Download `vft-stream-focus3-steam-frame.tar.gz`, unplug/reconnect the tracker,
stop other camera processes and run:

```sh
sudo ./vft-stream --tracker focus3 2>focus3-capture.log
cat focus3-capture.log
```

Share the complete log if streaming stalls, especially the `USB link speed=` and
`UVC stream error code=` lines. If it starts, connect to
`http://<Steam-Frame-IP>:8085/` and confirm image visibility/orientation.

Native and static ARM64 builds, protocol/image/payload regressions and
24 simulated USB/HTTP integration tests pass, including sanitizer checks.
Published package/binary hashes are in `SHA256SUMS`.

This is a hardware-test prerelease. Successful streaming, actual dimensions,
IR behavior, sustained FPS, tracking quality and service plug/unplug behavior
still require physical validation.
