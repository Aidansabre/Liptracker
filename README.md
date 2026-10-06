# vft-stream with Vive Focus 3 support

Activates a USB Vive Facial Tracker or Vive Focus 3 Facial Tracker and serves
its camera as an MJPEG stream for Baballonia's Wireless/IP Camera input.
The target is Steam Frame: capture uses libuvc/libusb from userspace and the
Steam Frame build is a static ARM64 executable with no runtime packages to install.

This is an extension of jaerven-in-vr/vft-stream at commit `6678300`, using the
Focus 3 activation protocol from Kirisame-Nanoha/Lip_Camera_IP_Server at
`ec070f1`. See `THIRD_PARTY_NOTICES.md` and `LICENSE` for attribution and licenses.

## Supported profiles

| Profile | USB device | Activation | Output |
| --- | --- | --- | --- |
| `vft` | `0bb4:0321` | Existing VFT sensor-register sequence with acknowledgements | Existing left-half crop, resize, blur and gamma; 400×400 |
| `focus3` | `0bb4:06a1` / HTC Lip Camera | Stream off/on, then system register `0x2246=0x11`; SET_CUR only | Full luminance, 90° CCW rotation, area resize to 320×480 |

A Steam Frame descriptor report confirms Focus 3 extension unit 4 with GUID
`2ccb0bda-6331-4fdb-850e-79054dbd5671`, selector 2, and **640×481 YUY2 at
approximately 30 FPS over bulk USB**. The selector's payload length is queried
at runtime; it is not the endpoint packet size. The extra row is retained.

Automatic selection uses the exact supported VID/PID pairs. It rejects ambiguous
or unknown devices. `--tracker focus3` selects the Focus 3 when both models are
present. An explicitly selected unknown HTC PID can use a profile via
`--tracker focus3 --pid HEX`; commands are never trialled across profiles.
The extension unit is discovered by its GUID, with `--xu-unit` available for diagnostics.

## First run on Steam Frame

Extract the Steam Frame package, then run these commands inside its folder:

```sh
chmod +x vft-stream
sudo ./vft-stream --tracker focus3 --diagnose
sudo ./vft-stream --tracker focus3
```

Diagnostics print the extension-unit ID, selector-2 payload length and UVC modes
without activating the stream or IR. Run only one camera process at a time.
If an older service is active, stop it before the manual test.

In Baballonia on the PC, select Wireless/IP Camera and enter:

```text
http://<Steam-Frame-IP>:8085/
```

The stream listens on all interfaces. Keep Frame and PC on a network where the
PC can reach Frame's TCP port 8085. Verify the mouth is fully visible and upright,
then adjust Baballonia's camera ROI as needed.

Options:

```text
-p PORT                 HTTP port (default 8085)
-q QUALITY              JPEG quality 1–100 (default 90)
-r                      full raw luminance frame, without rotation or preprocessing
--tracker auto|vft|focus3  default auto
--pid HEX               choose a specific HTC USB product ID
--xu-unit 1–255          override the discovered extension unit
--rotation 0|90|180|270  Focus 3 rotation in degrees CCW (default 90)
--startup-timeout 1–120 first usable frame deadline in seconds (default 10)
--diagnose              inspect descriptors without activating the tracker
```

If the camera is sideways or upside-down, stop it with Ctrl+C and try another
`--rotation` value. `-r` exposes the complete 640×481 source image for comparison.
The Focus 3 profile does not apply the older VFT's crop or gamma correction.
Focus 3 capture commits the UVC mode before enabling the sensor and IR, then
queues USB transfers without another mode commit. The first usable frame has a
ten-second deadline; after capture begins, three seconds without a usable frame
triggers shutdown. Ctrl+C/SIGTERM, activation failure, stream-start failure and
the watchdog attempt to turn IR off and disable streaming before releasing USB.

For troubleshooting, run `sudo sh diag.sh` next to the binary; it writes `diag.txt`.
Startup prints negotiated frame/payload sizes and the first callback's dimensions,
stride and byte count. A watchdog exit includes received, rejected and encoded
frame counts. `received=0` means libuvc delivered no complete frame callbacks;
nonzero `received` with `encoded=0` means callbacks reached image processing but
no usable frame was produced. Short frames remain rejected rather than silently
discarding the advertised extra row.

The first hardware test of `v0.3.0-focus3.1` reached capture startup but timed out
without a usable frame. `v0.3.0-focus3.2` changes the commit/activation order and
adds these diagnostics; successful streaming on Steam Frame still needs testing.
If capture stalls, share the complete application output from this command:

```sh
sudo ./vft-stream --tracker focus3 2>focus3-capture.log
cat focus3-capture.log
```

For deeper USB diagnostics, use `sudo LIBUSB_DEBUG=4 ./vft-stream --tracker focus3
2>focus3-usb.log` and retain the whole log. Cancelled transfers at shutdown do not
identify the original capture failure; the startup portion is also needed.

## Optional automatic service

After manual streaming works, run **as the normal SteamOS user**:

```sh
sh install-service.sh
```

The installer copies the binary atomically to `~/.local/bin`, adds narrow USB
permissions for both supported product IDs, and installs a systemd **system**
service running as that user with the `video` supplementary group. Device units
belong to the system manager; the upstream user-service arrangement is migrated.
The udev rule starts the service on attachment; `BindsTo` stops it on removal.

```sh
systemctl status vft-stream.service
journalctl -u vft-stream.service -n 80 --no-pager
sudo systemctl stop vft-stream.service
```

Service installation and plug/unplug behavior still need validation on Steam Frame.
The installer handles one selected tracker at a time.

## Build without Nix

Requires a Linux build host with Python 3/pip, a C compiler, make, pkg-config,
curl, tar and sha256sum. Tools and dependencies install into the ignored `.build`
directory; no system packages or root access are needed.

```sh
sh scripts/build.sh native
sh scripts/build.sh steam-frame
sh scripts/package-steam-frame.sh
```

Outputs:

- `dist/vft-stream-native`: native development binary.
- `dist/vft-stream-steam-frame`: static ARM64/musl binary.
- `dist/vft-stream-focus3-steam-frame.tar.gz`: binary, diagnostics, installer,
  full application source, build scripts, tests and license notices.

The build pins Zig 0.13.0, CMake 3.31.6, libusb 1.0.27, libuvc 0.0.7 and an stb
commit. Library source downloads are checksum verified; valid build outputs are
reused. Set `LIPTRACKER_BUILD_ROOT` and `LIPTRACKER_TOOLS` to use external caches,
and `LIPTRACKER_JOBS` to change the default four build jobs.

## Existing Nix build

```sh
NIXPKGS_ALLOW_UNFREE=1 nix-build
NIXPKGS_ALLOW_UNFREE=1 nix-build --arg cross false
```

The default produces a static aarch64 binary for Steam Frame; `cross false` builds
for the host. The declaration includes all new C modules. Nix itself was unavailable
in the cloud validation environment, so the standalone build was used and verified.

## GitHub releases

Version tags beginning with `v` run `.github/workflows/release.yml`. The workflow
builds native and static ARM64 binaries, runs the regression/sanitizer/integration
checks, and publishes a hardware-test prerelease with the ARM64 executable,
complete Steam Frame package and checksums. The current release is
`v0.3.0-focus3.2`. The same workflow can be started manually with an existing
version tag if needed. Physical hardware validation remains required.

## Tests and validation status

```sh
sh tests/run.sh
SANITIZE=1 sh tests/run.sh
sh scripts/build.sh native
sh tests/integration.sh
```

Integration tests install Pillow 11.3.0 into the tools cache, simulate USB delivery,
and exercise the real capture callback, JPEG encoder, HTTP server and cleanup.
They verify decoded JPEG dimensions/pixels, reconnection, diagnostics without
activation, device rejection, commit-before-activation ordering, startup failures,
busy ports, delayed first frames, rejected frames and stalled capture.
Protocol tests check exact payloads, padding, delays, acknowledgement behavior and
shutdown after a failed write. Image tests cover all rotations, raw frames, padded
stride, invalid buffers, area scaling and the reported odd-height mode. A fixture
from the original VFT pipeline confirms its output is byte-for-byte unchanged.

Cloud builds and tests cannot establish successful USB transfers, IR operation,
image orientation or sustained performance on the physical tracker. Those checks,
Baballonia tracking quality and service lifecycle remain hardware validation steps.

## License

The Baballonia-derived work remains under the **Babble Software Distribution
License 1.0**: non-commercial use, derivatives under the same license with source.
The Focus 3 reference's MIT notice and linked-library notices are preserved in
`THIRD_PARTY_NOTICES.md` and `licenses/`.
