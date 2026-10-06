#include "uvc_bulk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct check { const uint8_t *expected; size_t size; unsigned count; };
static void ready(void *user, const uint8_t *data, size_t size) {
  struct check *c = user;
  assert(size == c->size && !memcmp(data, c->expected, size));
  c->count++;
}
static void feed(struct uvc_bulk_decoder *d, unsigned flags, uint32_t pts,
                  const uint8_t *data, size_t size) {
  uint8_t *p = calloc(1, size + 12); assert(p);
  p[0] = 12; p[1] = flags;
  for (int i = 0; i < 4; i++) p[2+i] = pts >> (i * 8);
  if (size) memcpy(p+12, data, size);
  uvc_bulk_feed(d, p, size+12);
  free(p);
}
static void full(struct uvc_bulk_decoder *d, const uint8_t *data, size_t size,
                  unsigned fid, unsigned error, uint32_t pts) {
  for (size_t at = 0; at < size;) {
    size_t n = size-at > 32756 ? 32756 : size-at;
    unsigned flags = 0x0c | fid | error | (at+n == size ? 2 : 0);
    feed(d, flags, pts, data+at, n);
    at += n;
  }
}
int main(void) {
  size_t size = 640 * 481 * 2;
  uint8_t *data = malloc(size); assert(data);
  for (size_t i = 0; i < size; i++) data[i] = (uint8_t)(i * 13);
  struct check c = {data, size, 0};
  struct uvc_bulk_decoder d;
  assert(!uvc_bulk_init(&d, size, 0, ready, &c));
  /* Reproduce the 131024-byte initial callback: four full 32768-byte payloads. */
  for (int i = 0; i < 4; i++) feed(&d, 0x0d, 0x005faefc, data + i*32756, 32756);
  full(&d, data, size, 0, 0, 0x005faefd);
  assert(c.count == 1 && d.stats.incomplete == 1 && d.stats.max_frame_bytes == size);
  /* The observed 0x4d ERR flag taints a frame; it is never silently accepted. */
  full(&d, data, size, 1, 0x40, 20);
  assert(c.count == 1 && d.stats.error_frames == 1 && d.stats.error_payloads == 19);
  full(&d, data, size, 0, 0, 21);
  assert(c.count == 2);
  /* Header-only EOF, unlike libuvc's data-only EOF check. */
  feed(&d, 0x0d, 22, data, size);
  feed(&d, 0x0f, 22, NULL, 0);
  assert(c.count == 3);
  /* Missing EOF is recovered at a FID/PTS boundary. */
  feed(&d, 0x0c, 23, data, size);
  feed(&d, 0x0d, 24, NULL, 0);
  assert(c.count == 4);
  feed(&d, 0x0d, 24, data, size);
  feed(&d, 0x0d, 25, NULL, 0); /* Same FID; changed PTS also separates frames. */
  assert(c.count == 5);
  uvc_bulk_discard(&d);
  /* A 640x480 frame is reported as incomplete, not padded into 640x481. */
  full(&d, data, 640*480*2, 0, 0, 26);
  assert(c.count == 5 && d.stats.incomplete >= 2);
  /* Oversize frames and malformed/truncated optional headers are rejected. */
  feed(&d, 0x0d, 27, data, size);
  feed(&d, 0x0f, 27, data, 1);
  assert(c.count == 5);
  const uint8_t short_pts[] = {2, 4};
  const uint8_t short_scr[] = {2, 8};
  const uint8_t bad_size[] = {8, 0};
  uvc_bulk_feed(&d, short_pts, sizeof short_pts);
  uvc_bulk_feed(&d, short_scr, sizeof short_scr);
  uvc_bulk_feed(&d, bad_size, sizeof bad_size);
  uvc_bulk_feed(&d, NULL, 1);
  uvc_bulk_feed(&d, NULL, 0);
  assert(c.count == 5 && d.stats.invalid == 4);
  uvc_bulk_destroy(&d);
  /* Explicit error inspection still requires a complete frame. */
  c.count = 0;
  assert(!uvc_bulk_init(&d, size, 1, ready, &c));
  full(&d, data, 640*480*2, 1, 0x40, 1);
  assert(!c.count);
  full(&d, data, size, 0, 0x40, 2);
  assert(c.count == 1 && d.stats.error_frames == 2);
  uvc_bulk_destroy(&d);
  free(data);
  puts("uvc bulk: observed headers, exact full frames, partial/error recovery, EOF/FID/PTS, and buffer bounds passed");
  return 0;
}
