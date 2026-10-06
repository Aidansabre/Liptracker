// vft-stream — switch on a Vive Facial Tracker and serve it as an MJPEG stream.
//
// Licensed under the Babble Software Distribution License 1.0 (see LICENSE):
// non-commercial use only, derivatives under the same license with source.
//
// This is a modified work of Project Babble's Baballonia
// (https://github.com/Project-Babble/Baballonia, commit 5dbd332): a C
// translation of the VFT activation and image pipeline in
//   src/Baballonia.VFTCapture/Linux/LinuxUsbCommunicator.cs
//   src/Baballonia.VFTCapture/Linux/LinuxVFTCapture.cs
//   src/Baballonia.VFTCapture/VFTCommon.cs
// Changes 2026: Focus 3 profile, device/XU selection, diagnostics, and cleanup.
// Changes: ported from C# to C, OpenCV operations reimplemented by hand, the
// kernel V4L2/UVC ioctls replaced by libuvc, and an MJPEG HTTP server added.
// The VFT protocol was reverse engineered by DragonLord for Project Babble:
// https://docs.babble.diy/blog/reverse-engineering-the-vive-facial-tracker
//
// Runs on whatever the tracker is plugged into (the Steam Frame) so Baballonia
// can consume it elsewhere as a "Wireless/IP Camera" (http://host:port/).
//
// The tracker is driven from userspace through libuvc/libusb. A kernel
// uvcvideo driver and /dev/video node are not required. Capture needs usbfs
// access — root, or the udev rule install-service.sh writes.
//
// Baballonia applies the image pipeline only on its own VFT capture path, not
// to IP cameras, so it has to happen here or the model sees a raw stereo
// frame it was never trained on.

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <libuvc/libuvc.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include "tracker.h"
#include "image.h"
#include "bulk_capture.h"

static volatile sig_atomic_t stop;
static void on_signal(int s) { (void)s; stop = 1; }

static void uvc_die(const char *what, int err) {
  fprintf(stderr, "vft-stream: %s: %s\n", what, uvc_strerror(err));
  exit(1);
}

static void msleep(int ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

struct usb_control { uvc_device_handle_t *handle; uint8_t unit; int trace; };
static enum tracker_profile profile = TRACKER_AUTO;
static int rotation = 90;

static void control_sleep(void *context, unsigned milliseconds) {
  (void)context;
  msleep(milliseconds);
}

static int control_send(void *context, const uint8_t *command, size_t length, int ack) {
  struct usb_control *usb = context;
  int r = uvc_set_ctrl(usb->handle, usb->unit, 2, (void *)command, length);
  if (r < 0 || (size_t)r != length) {
    fprintf(stderr, "vft-stream: XU %u SET_CUR: %s (%d bytes)\n", usb->unit,
            r < 0 ? uvc_strerror(r) : "short write", r);
    return r < 0 ? r : UVC_ERROR_IO;
  }
  if (usb->trace) {
    flockfile(stderr);
    fprintf(stderr, "vft-stream: XU %u SET_CUR sent %d bytes prefix=", usb->unit, r);
    for (size_t i = 0; i < length && i < 17; i++) fprintf(stderr, "%02x", command[i]);
    fputc('\n', stderr);
    funlockfile(stderr);
  }
  if (!ack) return 0;
  uint8_t response[TRACKER_MAX_CONTROL];
  for (int waited = 0; waited < 1000; waited++) {
    r = uvc_get_ctrl(usb->handle, usb->unit, 2, response, length, UVC_GET_CUR);
    if (r < 0 || (size_t)r != length) {
      fprintf(stderr, "vft-stream: XU GET_CUR: %s\n", r < 0 ? uvc_strerror(r) : "short read");
      return r < 0 ? r : UVC_ERROR_IO;
    }
    if (response[0] == 0x56 && memcmp(command, response + 1, 16) == 0) return 0;
    if (response[0] != 0x55) {
      fprintf(stderr, "vft-stream: unexpected XU echo 0x%02x\n", response[0]);
      return UVC_ERROR_IO;
    }
    msleep(1);
  }
  fprintf(stderr, "vft-stream: XU echo timed out\n");
  return UVC_ERROR_TIMEOUT;
}

static void print_extensions(uvc_device_handle_t *handle) {
  for (const uvc_extension_unit_t *unit = uvc_get_extension_units(handle); unit; unit = unit->next) {
    fprintf(stderr, "vft-stream: XU unit=%u GUID(bytes)=", unit->bUnitID);
    for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", unit->guidExtensionCode[i]);
    fprintf(stderr, " controls=0x%llx", (unsigned long long)unit->bmControls);
    if (unit->bmControls & 2) fprintf(stderr, " selector2-length=%d", uvc_get_ctrl_len(handle, unit->bUnitID, 2));
    fputc('\n', stderr);
  }
}

static int select_control(uvc_device_handle_t *handle, int requested, struct usb_control *usb) {
  /* UVC GUID bytes for {2ccb0bda-6331-4fdb-850e-79054dbd5671}. */
  static const uint8_t guid[] = {0xda, 0x0b, 0xcb, 0x2c, 0x31, 0x63, 0xdb, 0x4f,
                                0x85, 0x0e, 0x79, 0x05, 0x4d, 0xbd, 0x56, 0x71};
  int unit_id = requested, count = 0;
  if (!unit_id) {
    for (const uvc_extension_unit_t *unit = uvc_get_extension_units(handle); unit; unit = unit->next)
      if (!memcmp(unit->guidExtensionCode, guid, sizeof guid) && (unit->bmControls & 2)) {
        unit_id = unit->bUnitID;
        count++;
      }
    /* Retain the existing VFT's known unit ID. Focus 3 requires discovery
     * or an explicit --xu-unit, rather than guessing an unrelated unit. */
    if (!count && profile == TRACKER_VFT) unit_id = 4;
    else if (count != 1) {
      fprintf(stderr, "vft-stream: cannot uniquely select HTC XU; run --diagnose and specify --xu-unit\n");
      return -1;
    }
  }
  int length = uvc_get_ctrl_len(handle, unit_id, 2);
  if (length < 17 || length > TRACKER_MAX_CONTROL ||
      (profile == TRACKER_VFT && length != 64 && length != 384)) {
    fprintf(stderr, "vft-stream: unsupported selector-2 length %d for XU %d\n", length, unit_id);
    return -1;
  }
  usb->handle = handle;
  usb->unit = unit_id;
  fprintf(stderr, "vft-stream: using XU %d selector 2, %d-byte payload\n", unit_id, length);
  return length;
}

/* -------------------------------------------------------------- http ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint8_t *frame;
static size_t frame_len, frame_cap;
static unsigned long frame_seq;

static void jpg_sink(void *ctx, void *data, int size) {
  (void)ctx;
  if (frame_len + size > frame_cap) {
    frame_cap = (frame_len + size) * 2;
    frame = realloc(frame, frame_cap);
  }
  memcpy(frame + frame_len, data, size);
  frame_len += size;
}

static int send_all(int fd, const void *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return -1;
    p = (const char *)p + w, n -= w;
  }
  return 0;
}

static void *client(void *arg) {
  int fd = (int)(intptr_t)arg;
  char req[1024];
  (void)!recv(fd, req, sizeof req, 0); // any path serves the stream

  static const char hdr[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: close\r\n\r\n";
  uint8_t *buf = NULL;
  size_t cap = 0;
  unsigned long seen = 0;

  if (send_all(fd, hdr, sizeof hdr - 1) == 0)
    for (;;) {
      pthread_mutex_lock(&mu);
      while (frame_seq == seen && !stop) pthread_cond_wait(&cv, &mu);
      if (stop) { pthread_mutex_unlock(&mu); break; }
      seen = frame_seq;
      size_t n = frame_len;
      if (n > cap) buf = realloc(buf, cap = n);
      memcpy(buf, frame, n);
      pthread_mutex_unlock(&mu);

      char part[128];
      int pl = snprintf(part, sizeof part,
                        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", n);
      if (send_all(fd, part, pl) || send_all(fd, buf, n) || send_all(fd, "\r\n", 2)) break;
    }

  free(buf);
  close(fd);
  return NULL;
}

static int open_server(int port) {
  int s = socket(AF_INET6, SOCK_STREAM, 0), one = 1, zero = 0;
  struct sockaddr_in6 a6 = {.sin6_family = AF_INET6, .sin6_port = htons(port), .sin6_addr = in6addr_any};
  struct sockaddr_in a4 = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
  struct sockaddr *a = (struct sockaddr *)&a6;
  socklen_t size = sizeof a6;
  if (s < 0 && errno == EAFNOSUPPORT) { /* Kernel without IPv6. */
    s = socket(AF_INET, SOCK_STREAM, 0);
    a = (struct sockaddr *)&a4;
    size = sizeof a4;
  }
  if (s < 0) return -1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (a->sa_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
  if (bind(s, a, size) < 0 || listen(s, 8) < 0) {
    int saved = errno;
    close(s);
    errno = saved;
    return -1;
  }
  fprintf(stderr, "vft-stream: serving on http://0.0.0.0:%d/\n", port);
  return s;
}

static void *server(void *arg) {
  int s = (int)(intptr_t)arg, one = 1;

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) { if (errno == EINTR && !stop) continue; break; }
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    pthread_t t;
    if (pthread_create(&t, NULL, client, (void *)(intptr_t)c) == 0) pthread_detach(t);
    else close(c);
  }
  return NULL;
}

/* ------------------------------------------------------------ stream ---- */

static int raw, quality = 90;
static pthread_mutex_t timing_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec last_frame;
static unsigned long received_frames, rejected_frames, encoded_frames;

// Runs on libuvc's transfer thread, once per complete frame.
static void on_frame(uvc_frame_t *f, void *user) {
  (void)user;
  static uint8_t img[IMAGE_MAX_PIXELS];
  static unsigned long n;
  static time_t last_log;

  pthread_mutex_lock(&timing_mu);
  unsigned long received = ++received_frames;
  pthread_mutex_unlock(&timing_mu);
  if (received == 1)
    fprintf(stderr, "vft-stream: first callback: %ux%u format=%d stride=%zu bytes=%zu expected=%zu\n",
            f->width, f->height, f->frame_format, f->step, f->data_bytes,
            (size_t)f->width * f->height * 2);

  int ow, oh;
  if (f->frame_format != UVC_FRAME_FORMAT_YUYV ||
      image_process(profile, rotation, raw, f->data, f->data_bytes, f->width,
                    f->height, f->step, img, sizeof img, &ow, &oh)) {
    pthread_mutex_lock(&timing_mu);
    unsigned long rejected = ++rejected_frames;
    pthread_mutex_unlock(&timing_mu);
    if (rejected <= 3)
      fprintf(stderr, "vft-stream: rejected callback: %ux%u format=%d stride=%zu bytes=%zu\n",
              f->width, f->height, f->frame_format, f->step, f->data_bytes);
    return;
  }

  pthread_mutex_lock(&mu);
  frame_len = 0;
  stbi_write_jpg_to_func(jpg_sink, NULL, ow, oh, 1, img, quality);
  frame_seq++;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);

  time_t now = time(NULL);
  pthread_mutex_lock(&timing_mu);
  clock_gettime(CLOCK_MONOTONIC, &last_frame);
  encoded_frames++;
  pthread_mutex_unlock(&timing_mu);
  if (!last_log) last_log = now;
  n++;
  if (now - last_log >= 10) {
    fprintf(stderr, "vft-stream: %.1f fps\n", n / (double)(now - last_log));
    n = 0, last_log = now;
  }
}

// The tracker advertises its modes in its descriptors; take its first YUYV
// frame size at its default interval instead of hardcoding 400x400@60.
static int pick_mode(uvc_device_handle_t *h, uvc_stream_ctrl_t *ctrl, unsigned *width, unsigned *height) {
  for (const uvc_format_desc_t *fmt = uvc_get_format_descs(h); fmt; fmt = fmt->next) {
    if (fmt->bDescriptorSubtype != UVC_VS_FORMAT_UNCOMPRESSED ||
        memcmp(fmt->fourccFormat, "YUY2", 4) != 0)
      continue;
    for (const uvc_frame_desc_t *fr = fmt->frame_descs; fr; fr = fr->next) {
      if (!fr->wWidth || !fr->wHeight || (fr->wWidth & 1) ||
          (size_t)fr->wWidth * fr->wHeight > IMAGE_MAX_PIXELS) continue;
      int fps = fr->dwDefaultFrameInterval ? 10000000 / fr->dwDefaultFrameInterval : 0;
      int r = uvc_get_stream_ctrl_format_size(h, ctrl, UVC_FRAME_FORMAT_YUYV, fr->wWidth,
                                              fr->wHeight, fps);
      if (r == 0) {
        *width = fr->wWidth; *height = fr->wHeight;
        fprintf(stderr, "vft-stream: %ux%u YUYV @ %d fps\n", fr->wWidth, fr->wHeight, fps);
        return 0;
      }
    }
  }
  fprintf(stderr, "vft-stream: no usable YUYV mode; descriptors follow\n");
  uvc_print_diag(h, stderr);
  return -1;
}

/* Read the negotiated bulk endpoint directly, without libuvc's frame parser.
 * The stream is committed and activated, but no competing capture transfers
 * may be queued while this diagnostic runs. */
static int probe_bulk(uvc_device_handle_t *h, const uvc_stream_ctrl_t *ctrl, int seconds) {
  libusb_device_handle *usb = uvc_get_libusb_handle(h);
  uint8_t endpoint = 0;
  int r = bulk_endpoint(h, ctrl, &endpoint);
  if (r) return 1;
  int length = ctrl->dwMaxPayloadTransferSize;
  uint8_t *data = malloc(length);
  if (!data) { fprintf(stderr, "vft-stream: bulk probe allocation failed\n"); return 1; }
  fprintf(stderr, "vft-stream: bulk probe endpoint=0x%02x read-size=%d deadline=%ds (no frame parser)\n",
          endpoint, length, seconds);
  unsigned reads = 0, nonempty = 0, timeouts = 0;
  size_t bytes = 0;
  int fatal = 0;
  struct timespec begin, now;
  clock_gettime(CLOCK_MONOTONIC, &begin);
  while (!stop && nonempty < 3) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    double remaining = seconds - (now.tv_sec - begin.tv_sec + (now.tv_nsec - begin.tv_nsec) / 1e9);
    if (remaining <= 0) break;
    unsigned timeout = remaining >= 1 ? 1000 : (unsigned)(remaining * 1000) + 1;
    int got = 0;
    r = libusb_bulk_transfer(usb, endpoint, data, length, &got, timeout);
    reads++;
    if (r == LIBUSB_ERROR_TIMEOUT) timeouts++;
    int report = reads <= 3 || got > 0 || (r && r != LIBUSB_ERROR_TIMEOUT);
    if (report)
      fprintf(stderr, "vft-stream: bulk read %u: %s (%d) bytes=%d", reads, libusb_error_name(r), r, got);
    int uvc_error = 0;
    if (got > 0 && got <= length) {
      bytes += got;
      nonempty++;
      uvc_error = got >= 2 && data[0] >= 2 && (data[1] & 0x40);
      if (report) {
        fprintf(stderr, " prefix=");
        for (int i = 0; i < got && i < 16; i++) fprintf(stderr, "%02x", data[i]);
      }
    }
    if (report) fputc('\n', stderr);
    if (uvc_error) bulk_report_stream_error(h, ctrl->bInterfaceNumber);
    if (r && r != LIBUSB_ERROR_TIMEOUT) { fatal = 1; break; }
    if (!r && !got) msleep(20); /* Avoid spinning on repeated empty packets. */
  }
  free(data);
  fprintf(stderr, "vft-stream: bulk probe summary: reads=%u bytes=%zu timeouts=%u\n", reads, bytes, timeouts);
  return fatal || !bytes;
}

/* -------------------------------------------------------------- main ---- */

static void usage(int status) {
  fprintf(stderr,
          "usage: vft-stream [-p port] [-q jpeg-quality] [-r] [options]\n"
          "  -p  HTTP port (default 8085)\n"
          "  -q  JPEG quality 1-100 (default 90)\n"
          "  -r  raw: full Y frame without processing or rotation\n"
          "  --tracker auto|vft|focus3  (default auto)\n"
          "  --pid hex                select a specific HTC product ID\n"
          "  --xu-unit 1-255          override the vendor extension unit\n"
          "  --rotation 0|90|180|270   Focus 3 degrees CCW (default 90)\n"
          "  --startup-timeout 1-120   first usable frame deadline (default 10s)\n"
          "  --probe-bulk              Focus 3 raw USB transfer diagnostic, then exit\n"
          "  --capture-first           Focus 3: commit and queue capture before vendor activation\n"
          "  --capture-backend auto|bulk|libuvc  Focus 3 defaults to direct bulk reads\n"
          "  --allow-uvc-errors        inspect full Focus 3 frames carrying UVC ERR\n"
          "  --recommit-after-activation  Focus 3 bulk: commit before and again after activation\n"
          "  --bulk-timeout 50-5000    bulk read timeout in ms (default 250)\n"
          "  --no-clear-halt           Focus 3 bulk/libuvc: skip CLEAR_FEATURE(HALT)\n"
          "  --diagnose               print USB/UVC descriptors without activation\n");
  exit(status);
}

// Wait for Ctrl+C/SIGTERM; 1 if frames stop arriving.
static int watch_frames(int startup_timeout) {
  while (!stop) {
    msleep(200);
    // A stalled tracker (unplugged, or the XU state reset) never recovers by
    // waiting; exit and let the service manager reinitialise it.
    struct timespec now, previous;
    unsigned long received, rejected, encoded;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&timing_mu);
    previous = last_frame;
    received = received_frames;
    rejected = rejected_frames;
    encoded = encoded_frames;
    pthread_mutex_unlock(&timing_mu);
    int deadline = encoded ? 3 : startup_timeout;
    if (now.tv_sec - previous.tv_sec + (now.tv_nsec - previous.tv_nsec) / 1e9 > deadline) {
      fprintf(stderr, "vft-stream: no %sframe in %ds (received=%lu rejected=%lu encoded=%lu)\n",
              encoded ? "" : "usable startup ", deadline, received, rejected, encoded);
      return 1;
    }
  }
  return 0;
}

/* VS_COMMIT. Focus 3 commits after vendor activation by default, as the
 * reference's DirectShow graph does when it starts running. */
static int commit_stream(uvc_device_handle_t *h, uvc_stream_ctrl_t *ctrl,
                         uvc_stream_handle_t **capture, const char *when) {
  int r = uvc_stream_open_ctrl(h, capture, ctrl);
  if (r < 0) {
    fprintf(stderr, "vft-stream: commit capture stream: %s\n", uvc_strerror(r));
    return r;
  }
  fprintf(stderr, "vft-stream: UVC stream committed %s Focus 3 activation\n", when);
  return 0;
}

static int parse_number(const char *text, int base, int low, int high) {
  char *end;
  errno = 0;
  long value = strtol(text, &end, base);
  if (errno || end == text || *end || value < low || value > high) usage(2);
  return value;
}

int main(int argc, char **argv) {
  int port = 8085, opt, r, pid = 0, xu_unit = 0, diagnose = 0, startup_timeout = 10;
  int bulk_diagnostic = 0, capture_first = 0;
  int backend = 0, allow_uvc_errors = 0;
  int recommit = 0, bulk_timeout = 250, custom_bulk_timeout = 0, clear_halt = 1;
  enum { OPT_RECOMMIT = 1000, OPT_BULK_TIMEOUT, OPT_NO_CLEAR_HALT };
  enum tracker_profile requested = TRACKER_AUTO;
  static const struct option options[] = {
    {"tracker", required_argument, NULL, 't'}, {"pid", required_argument, NULL, 'i'},
    {"xu-unit", required_argument, NULL, 'u'}, {"rotation", required_argument, NULL, 'R'},
    {"startup-timeout", required_argument, NULL, 'T'},
    {"probe-bulk", no_argument, NULL, 'B'},
    {"capture-first", no_argument, NULL, 'C'},
    {"capture-backend", required_argument, NULL, 'K'},
    {"allow-uvc-errors", no_argument, NULL, 'E'},
    {"recommit-after-activation", no_argument, NULL, OPT_RECOMMIT},
    {"bulk-timeout", required_argument, NULL, OPT_BULK_TIMEOUT},
    {"no-clear-halt", no_argument, NULL, OPT_NO_CLEAR_HALT},
    {"diagnose", no_argument, NULL, 'd'}, {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
  };
  while ((opt = getopt_long(argc, argv, "p:q:rh", options, NULL)) != -1) switch (opt) {
      case 'p': port = parse_number(optarg, 10, 1, 65535); break;
      case 'q': quality = parse_number(optarg, 10, 1, 100); break;
      case 'r': raw = 1; break;
      case 't':
        if (!strcmp(optarg, "auto")) requested = TRACKER_AUTO;
        else if (!strcmp(optarg, "vft")) requested = TRACKER_VFT;
        else if (!strcmp(optarg, "focus3")) requested = TRACKER_FOCUS3;
        else usage(2);
        break;
      case 'i': pid = parse_number(optarg, 16, 1, 65535); break;
      case 'u': xu_unit = parse_number(optarg, 10, 1, 255); break;
      case 'R': rotation = parse_number(optarg, 10, 0, 270); if (rotation % 90) usage(2); break;
      case 'T': startup_timeout = parse_number(optarg, 10, 1, 120); break;
      case 'B': bulk_diagnostic = 1; break;
      case 'C': capture_first = 1; break;
      case 'K':
        if (!strcmp(optarg, "auto")) backend = 0;
        else if (!strcmp(optarg, "bulk")) backend = 1;
        else if (!strcmp(optarg, "libuvc")) backend = 2;
        else usage(2);
        break;
      case 'E': allow_uvc_errors = 1; break;
      case OPT_RECOMMIT: recommit = 1; break;
      case OPT_BULK_TIMEOUT: bulk_timeout = parse_number(optarg, 10, 50, 5000); custom_bulk_timeout = 1; break;
      case OPT_NO_CLEAR_HALT: clear_halt = 0; break;
      case 'd': diagnose = 1; break;
      case 'h': usage(0); break;
      default: usage(2);
    }
  if (optind != argc) usage(2);
  if ((bulk_diagnostic || capture_first) && (diagnose || requested == TRACKER_VFT)) usage(2);
  if (bulk_diagnostic && capture_first) usage(2);
  if (allow_uvc_errors && (diagnose || bulk_diagnostic || backend == 2 || requested == TRACKER_VFT)) usage(2);
  if ((recommit || custom_bulk_timeout) &&
      (diagnose || bulk_diagnostic || backend == 2 || requested == TRACKER_VFT)) usage(2);
  if (!clear_halt && (diagnose || requested == TRACKER_VFT)) usage(2);

  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  image_init();

  uvc_context_t *ctx = NULL;
  uvc_device_t **devices = NULL, *dev = NULL;
  uvc_device_handle_t *h = NULL;
  uvc_stream_handle_t *capture = NULL;
  struct bulk_capture *bulk = NULL;
  struct usb_control usb = {0};
  struct tracker_io io = {.context = &usb, .send = control_send, .sleep_ms = control_sleep};
  int rc = 1, activation_attempted = 0, streaming = 0, server_fd = -1, have_endpoint = 0;
  uint8_t endpoint = 0;
  if ((r = uvc_init(&ctx, NULL)) < 0) uvc_die("uvc_init", r);
  if ((r = uvc_get_device_list(ctx, &devices)) < 0) {
    fprintf(stderr, "vft-stream: enumerate cameras: %s\n", uvc_strerror(r));
    goto cleanup;
  }
  int matches = 0;
  for (size_t i = 0; devices[i]; i++) {
    uvc_device_descriptor_t *descriptor;
    if (uvc_get_device_descriptor(devices[i], &descriptor) < 0) continue;
    enum tracker_profile detected = tracker_identify(descriptor->idVendor, descriptor->idProduct);
    if (descriptor->idVendor == HTC_VID)
      fprintf(stderr, "vft-stream: HTC %04x:%04x %s (profile %s)\n", descriptor->idVendor,
              descriptor->idProduct, descriptor->product ? descriptor->product : "(no product string)", tracker_name(detected));
    int eligible = descriptor->idVendor == HTC_VID &&
                   (!pid || descriptor->idProduct == pid) &&
                   (requested == TRACKER_AUTO ? detected != TRACKER_AUTO :
                    detected == requested || (pid && detected == TRACKER_AUTO));
    if (diagnose && pid && descriptor->idVendor == HTC_VID && descriptor->idProduct == pid) eligible = 1;
    if (eligible) {
      matches++;
      dev = devices[i];
      profile = requested == TRACKER_AUTO ? detected : requested;
    }
    uvc_free_device_descriptor(descriptor);
  }
  if (matches != 1) {
    fprintf(stderr, "vft-stream: %d matching cameras; use --tracker and --pid to select exactly one\n", matches);
    goto cleanup;
  }
  if ((bulk_diagnostic || capture_first || backend == 1 || allow_uvc_errors || recommit ||
       custom_bulk_timeout || !clear_halt) && profile != TRACKER_FOCUS3) {
    fprintf(stderr, "vft-stream: capture diagnostics require a Focus 3 tracker\n");
    goto cleanup;
  }
  if ((r = uvc_open(dev, &h)) < 0) {
    // ACCESS here is usbfs permissions, not a missing tracker.
    fprintf(stderr, "vft-stream: open tracker: %s%s\n", uvc_strerror(r),
            r == UVC_ERROR_ACCESS ? " (run as root, or install-service.sh for the udev rule)" : "");
    goto cleanup;
  }
  fprintf(stderr, "vft-stream: opened %s tracker\n", tracker_name(profile));
  print_extensions(h);
  if (diagnose) { uvc_print_diag(h, stderr); rc = 0; goto cleanup; }
  r = select_control(h, xu_unit, &usb);
  if (r < 0) goto cleanup;
  io.length = r;
  int use_bulk = profile == TRACKER_FOCUS3 && backend != 2 && !bulk_diagnostic;
  usb.trace = bulk_diagnostic || capture_first || use_bulk;
  if (profile == TRACKER_FOCUS3 && bulk_select_alt0(h)) goto cleanup;

  uvc_stream_ctrl_t ctrl = {0};
  unsigned width = 0, height = 0;
  if (pick_mode(h, &ctrl, &width, &height)) goto cleanup;
  fprintf(stderr, "vft-stream: UVC interface=%u format=%u frame=%u interval=%u max-frame=%u max-payload=%u\n",
          ctrl.bInterfaceNumber, ctrl.bFormatIndex, ctrl.bFrameIndex, ctrl.dwFrameInterval,
          ctrl.dwMaxVideoFrameSize, ctrl.dwMaxPayloadTransferSize);
  if (!bulk_diagnostic) {
    server_fd = open_server(port);
    if (server_fd < 0) {
      fprintf(stderr, "vft-stream: HTTP listen: %s\n", strerror(errno));
      goto cleanup;
    }
  }
  if (profile == TRACKER_FOCUS3) {
    /* Report the link and end any stream an earlier run left active, as
     * Windows does when it stops a bulk camera. The probe claimed the interface. */
    if (!bulk_endpoint(h, &ctrl, &endpoint)) {
      have_endpoint = 1;
      bulk_report_link(h, &ctrl, endpoint);
      if (clear_halt) bulk_clear_halt(h, endpoint, "before commit");
    }
    /* Diagnostic orderings that commit before the vendor enable. */
    if ((capture_first || recommit) && commit_stream(h, &ctrl, &capture, "before")) goto cleanup;
  }
  if (use_bulk) {
    if ((r = bulk_capture_open(h, &ctrl, width, height, allow_uvc_errors, bulk_timeout, &bulk)) < 0) {
      fprintf(stderr, "vft-stream: prepare bulk capture: %s\n", uvc_strerror(r));
      goto cleanup;
    }
    if (allow_uvc_errors)
      fprintf(stderr, "vft-stream: allowing UVC ERR on full-size frames for inspection\n");
  }
  if (capture_first) {
    clock_gettime(CLOCK_MONOTONIC, &last_frame);
    r = bulk ? bulk_capture_start(bulk, on_frame, NULL) : uvc_stream_start(capture, on_frame, NULL, 0);
    if (r < 0) {
      fprintf(stderr, "vft-stream: start streaming: %s\n", uvc_strerror(r));
      goto cleanup;
    }
    streaming = 1;
    fprintf(stderr, "vft-stream: USB capture queued before Focus 3 activation\n");
  }
  activation_attempted = 1;
  if (tracker_set_state(profile, 1, &io)) goto cleanup;
  if (recommit) {
    if ((r = uvc_stream_ctrl(capture, &ctrl)) < 0) {
      fprintf(stderr, "vft-stream: recommit after activation: %s\n", uvc_strerror(r));
      goto cleanup;
    }
    fprintf(stderr, "vft-stream: UVC stream recommitted after Focus 3 activation\n");
  }
  if (bulk_diagnostic) {
    if (commit_stream(h, &ctrl, &capture, "after")) goto cleanup;
    rc = probe_bulk(h, &ctrl, startup_timeout);
    goto cleanup;
  }

  pthread_t srv;
  if ((r = pthread_create(&srv, NULL, server, (void *)(intptr_t)server_fd))) {
    fprintf(stderr, "vft-stream: create HTTP server: %s\n", strerror(r));
    goto cleanup;
  }
  pthread_detach(srv);

  if (!streaming) {
    /* Reference order: activate, then commit and read straight away. */
    if (profile == TRACKER_FOCUS3 && !capture && commit_stream(h, &ctrl, &capture, "after"))
      goto cleanup;
    clock_gettime(CLOCK_MONOTONIC, &last_frame);
    r = bulk ? bulk_capture_start(bulk, on_frame, NULL) :
        capture ? uvc_stream_start(capture, on_frame, NULL, 0) :
                  uvc_start_streaming(h, &ctrl, on_frame, NULL, 0);
    if (r < 0) {
      fprintf(stderr, "vft-stream: start streaming: %s\n", uvc_strerror(r));
      goto cleanup;
    }
    streaming = 1;
  }

  rc = watch_frames(startup_timeout);

cleanup:
  if (streaming) {
    if (bulk) bulk_capture_stop(bulk);
    else if (capture) uvc_stream_stop(capture);
    else uvc_stop_streaming(h);
  }
  if (clear_halt && have_endpoint && capture) bulk_clear_halt(h, endpoint, "stop stream");
  if (activation_attempted && tracker_set_state(profile, 0, &io)) {
    fprintf(stderr, "vft-stream: tracker shutdown failed (device may be disconnected)\n");
    rc = 1;
  }
  pthread_mutex_lock(&mu);
  stop = 1;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
  if (server_fd >= 0) { shutdown(server_fd, SHUT_RDWR); close(server_fd); }
  if (bulk) bulk_capture_close(bulk);
  if (capture) uvc_stream_close(capture);
  if (h) uvc_close(h);
  if (devices) uvc_free_device_list(devices, 1);
  if (ctx) uvc_exit(ctx);
  return rc;
}
