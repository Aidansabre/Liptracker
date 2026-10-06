# Source and dependency notices

This project extends [jaerven-in-vr/vft-stream](https://github.com/jaerven-in-vr/vft-stream)
at commit `6678300f48c43caea65cc71b214879434b6b680c`. Its Baballonia-derived
code remains under the Babble Software Distribution License 1.0 in LICENSE.
Modified files identify the additions made in 2026.

Focus 3 command handling is adapted from `src/tracker_control.py` in
[Kirisame-Nanoha/Lip_Camera_IP_Server](https://github.com/Kirisame-Nanoha/Lip_Camera_IP_Server)
at commit `ec070f191f80d1325b6faa8d08d70bc4eb1cb126`. Its source uses the MIT
license below. The image profile follows its full-luminance, portrait pipeline;
this project implements that processing in C without OpenCV.

## MIT notice for tracker-control source

Copyright DragonDreams GmbH 2024

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Linked libraries

The static Steam Frame build contains:

- libusb 1.0.27, LGPL-2.1-or-later: `licenses/libusb-LGPL-2.1.txt`.
- libuvc 0.0.7 (`68d07a0`), BSD: `licenses/libuvc-BSD.txt`.
- stb_image_write (`2c980bb`), public domain or MIT: `licenses/stb.txt`.
- musl libc supplied by Zig 0.13.0, MIT: `licenses/musl.txt`.
- Zig runtime helpers, MIT: `licenses/zig-MIT.txt`.

The package includes this application's source and build scripts. The scripts
download the exact upstream library sources with checksum verification and
allow rebuilding/relinking the binary with modified libraries. Source archives
are retained in the configured build root. Keep these notices with distributed
binaries and preserve the applicable source/relinking requirements.
