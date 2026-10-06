/* Assemble UVC bulk payloads using FID, PTS and EOF boundaries. See LICENSE. */
#include "uvc_bulk.h"
#include <stdlib.h>
#include <string.h>

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int uvc_bulk_init(struct uvc_bulk_decoder *d, size_t capacity, int allow_errors,
                  void (*ready)(void *, const uint8_t *, size_t), void *user) {
  if (!d || !ready || !capacity || capacity > 4 * 1024 * 1024) return -1;
  memset(d, 0, sizeof *d);
  d->frame = malloc(capacity);
  if (!d->frame) return -1;
  d->capacity = capacity;
  d->allow_errors = allow_errors;
  d->ready = ready;
  d->user = user;
  return 0;
}
static void finish(struct uvc_bulk_decoder *d) {
  if (!d->active) return;
  d->stats.frames++;
  if (d->stats.frames == 1 || d->used < d->stats.min_frame_bytes) d->stats.min_frame_bytes = d->used;
  if (d->used > d->stats.max_frame_bytes) d->stats.max_frame_bytes = d->used;
  int incomplete = d->used != d->capacity || d->overflow;
  if (d->observe) d->observe(d->user, d->used, d->bad, incomplete);
  if (d->bad) d->stats.error_frames++;
  if (incomplete) d->stats.incomplete++;
  else if (!d->bad || d->allow_errors) {
    d->stats.delivered++;
    d->ready(d->user, d->frame, d->used);
  }
  d->active = 0;
  d->used = 0;
  d->bad = d->overflow = 0;
}
void uvc_bulk_discard(struct uvc_bulk_decoder *d) {
  if (d->active) {
    d->overflow = 1;
    finish(d);
  }
  d->ended = 1;
}
void uvc_bulk_feed(struct uvc_bulk_decoder *d, const uint8_t *p, size_t size) {
  if (!size) return; /* Zero-length USB packet is a payload boundary, not a frame. */
  d->stats.payloads++;
  if (!p || size < 2 || p[0] < 2 || p[0] > size) goto invalid;
  uint8_t flags = p[1];
  size_t needed = 2 + ((flags & 4) ? 4 : 0) + ((flags & 8) ? 6 : 0);
  if (p[0] < needed) goto invalid;
  if (flags & 0x40) d->stats.error_payloads++;
  int fid = flags & 1, have_pts = !!(flags & 4);
  uint32_t pts = have_pts ? le32(p + 2) : 0;
  int changed = d->fid != fid || (have_pts && d->have_pts && pts != d->pts);
  if (d->active && changed) finish(d);
  if (d->ended && !changed) return; /* Ignore trailing packets after EOF. */
  d->ended = 0;
  if (!d->active) {
    d->active = 1;
    d->fid = fid;
    d->have_pts = have_pts;
    d->pts = pts;
  } else if (have_pts && !d->have_pts) {
    d->have_pts = 1;
    d->pts = pts;
  }
  if (flags & 0x40) d->bad = 1;
  size_t data = size - p[0];
  if (data > d->capacity - d->used) d->overflow = 1;
  if (!d->overflow) {
    memcpy(d->frame + d->used, p + p[0], data);
    d->used += data;
  }
  if (flags & 2) { finish(d); d->ended = 1; }
  return;
invalid:
  d->stats.invalid++;
  uvc_bulk_discard(d);
}
void uvc_bulk_destroy(struct uvc_bulk_decoder *d) {
  if (!d) return;
  free(d->frame);
  memset(d, 0, sizeof *d);
}
