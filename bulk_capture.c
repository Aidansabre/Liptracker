/* One direct USB read at a time, as validated by --probe-bulk. See LICENSE. */
#define _POSIX_C_SOURCE 200809L
#include "bulk_capture.h"
#include "uvc_bulk.h"
#include <inttypes.h>
#include <libusb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct bulk_capture {
  libusb_device_handle *usb;
  uint8_t endpoint, *payload;
  size_t length;
  unsigned width, height;
  struct uvc_bulk_decoder decoder;
  uvc_frame_callback_t *callback;
  void *user;
  pthread_t thread;
  atomic_int running;
  int started;
  uint64_t reads, bytes, timeouts;
};
int bulk_endpoint(uvc_device_handle_t *h, const uvc_stream_ctrl_t *ctrl, uint8_t *endpoint) {
  libusb_device_handle *usb = uvc_get_libusb_handle(h);
  struct libusb_config_descriptor *config = NULL;
  int r = libusb_get_active_config_descriptor(libusb_get_device(usb), &config);
  if (r < 0) {
    fprintf(stderr, "vft-stream: bulk descriptors: %s\n", libusb_error_name(r));
    return r;
  }
  int matches = 0;
  for (int i = 0; i < config->bNumInterfaces; i++) {
    const struct libusb_interface *interface = &config->interface[i];
    for (int j = 0; j < interface->num_altsetting; j++) {
      const struct libusb_interface_descriptor *alt = &interface->altsetting[j];
      if (alt->bInterfaceNumber != ctrl->bInterfaceNumber || alt->bAlternateSetting != 0) continue;
      for (int k = 0; k < alt->bNumEndpoints; k++) {
        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN &&
            (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
          *endpoint = ep->bEndpointAddress;
          matches++;
        }
      }
    }
  }
  libusb_free_config_descriptor(config);
  if (matches != 1 || !ctrl->dwMaxPayloadTransferSize || ctrl->dwMaxPayloadTransferSize > 1024 * 1024) {
    fprintf(stderr, "vft-stream: bulk capture requires one bulk IN endpoint and a 1-1048576 byte payload\n");
    return UVC_ERROR_INVALID_MODE;
  }
  return 0;
}
static void ready(void *user, const uint8_t *data, size_t size) {
  struct bulk_capture *b = user;
  uvc_frame_t frame = {.data = (void *)data, .data_bytes = size, .width = b->width,
    .height = b->height, .step = b->width * 2, .frame_format = UVC_FRAME_FORMAT_YUYV};
  b->callback(&frame, b->user);
}
static void observe(void *user, size_t size, int bad, int incomplete) {
  struct bulk_capture *b = user;
  if (b->decoder.stats.frames <= 3)
    fprintf(stderr, "vft-stream: bulk frame: bytes=%zu expected=%zu error=%d incomplete=%d\n",
            size, b->decoder.capacity, bad, incomplete);
}
int bulk_capture_open(uvc_device_handle_t *h, const uvc_stream_ctrl_t *ctrl,
                      unsigned width, unsigned height, int allow_errors, struct bulk_capture **out) {
  *out = NULL;
  uint8_t endpoint = 0;
  int r = bulk_endpoint(h, ctrl, &endpoint);
  if (r) return r;
  if (!width || !height || (width & 1) || width > 1920 || height > 1080) return UVC_ERROR_INVALID_MODE;
  struct bulk_capture *b = calloc(1, sizeof *b);
  if (!b) return UVC_ERROR_NO_MEM;
  b->usb = uvc_get_libusb_handle(h);
  b->endpoint = endpoint;
  b->length = ctrl->dwMaxPayloadTransferSize;
  b->width = width; b->height = height;
  atomic_init(&b->running, 0);
  b->payload = malloc(b->length);
  if (!b->payload || uvc_bulk_init(&b->decoder, (size_t)width * height * 2, allow_errors, ready, b)) {
    free(b->payload); free(b); return UVC_ERROR_NO_MEM;
  }
  b->decoder.observe = observe;
  *out = b;
  return 0;
}
static void *read_loop(void *user) {
  struct bulk_capture *b = user;
  size_t used = 0;
  while (atomic_load(&b->running)) {
    int got = 0;
    int requested = b->length - used;
    int r = libusb_bulk_transfer(b->usb, b->endpoint, b->payload + used, requested, &got, 250);
    b->reads++;
    if (r == LIBUSB_ERROR_TIMEOUT) b->timeouts++;
    if (got < 0 || got > requested) { uvc_bulk_discard(&b->decoder); break; }
    b->bytes += got;
    used += got;
    if (!r || (r == LIBUSB_ERROR_TIMEOUT && used == b->length)) {
      if (b->decoder.stats.payloads < 3 && used >= 2)
        fprintf(stderr, "vft-stream: bulk payload: bytes=%zu header=%u flags=0x%02x\n", used, b->payload[0], b->payload[1]);
      uvc_bulk_feed(&b->decoder, b->payload, used);
      used = 0;
    } else if (r != LIBUSB_ERROR_TIMEOUT) {
      fprintf(stderr, "vft-stream: bulk capture read: %s (%d) bytes=%d\n", libusb_error_name(r), r, got);
      uvc_bulk_discard(&b->decoder);
      break;
    }
    if (!r && !got) {
      struct timespec pause = {0, 20000000L};
      nanosleep(&pause, NULL);
    }
  }
  uvc_bulk_discard(&b->decoder); /* Never publish an unfinished frame on shutdown. */
  return NULL;
}
int bulk_capture_start(struct bulk_capture *b, uvc_frame_callback_t *callback, void *user) {
  if (!b || b->started || !callback) return UVC_ERROR_INVALID_PARAM;
  b->callback = callback; b->user = user;
  atomic_store(&b->running, 1);
  int r = pthread_create(&b->thread, NULL, read_loop, b);
  if (r) { atomic_store(&b->running, 0); return UVC_ERROR_OTHER; }
  b->started = 1;
  fprintf(stderr, "vft-stream: direct bulk capture endpoint=0x%02x payload=%zu\n", b->endpoint, b->length);
  return 0;
}
void bulk_capture_stop(struct bulk_capture *b) {
  if (!b || !b->started) return;
  atomic_store(&b->running, 0);
  pthread_join(b->thread, NULL);
  b->started = 0;
  struct uvc_bulk_stats *s = &b->decoder.stats;
  fprintf(stderr, "vft-stream: bulk totals: reads=%" PRIu64 " bytes=%" PRIu64 " timeouts=%" PRIu64
          " payloads=%" PRIu64 " invalid=%" PRIu64 " error-payloads=%" PRIu64 " frames=%" PRIu64
          " incomplete=%" PRIu64 " error-frames=%" PRIu64 " delivered=%" PRIu64
          " min-frame=%" PRIu64 " max-frame=%" PRIu64 " expected=%zu\n",
          b->reads, b->bytes, b->timeouts, s->payloads, s->invalid, s->error_payloads,
          s->frames, s->incomplete, s->error_frames, s->delivered,
          s->min_frame_bytes, s->max_frame_bytes, b->decoder.capacity);
}
void bulk_capture_close(struct bulk_capture *b) {
  if (!b) return;
  bulk_capture_stop(b);
  uvc_bulk_destroy(&b->decoder);
  free(b->payload);
  free(b);
}
