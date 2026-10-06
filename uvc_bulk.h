/* UVC payload assembly for the Focus 3 bulk reader. See LICENSE. */
#ifndef UVC_BULK_H
#define UVC_BULK_H
#include <stddef.h>
#include <stdint.h>

struct uvc_bulk_stats {
  uint64_t payloads, invalid, error_payloads, frames, incomplete, error_frames, delivered;
  uint64_t min_frame_bytes, max_frame_bytes;
};
struct uvc_bulk_decoder {
  uint8_t *frame;
  size_t capacity, used;
  int active, fid, have_pts, bad, overflow, ended, allow_errors;
  uint32_t pts;
  void (*ready)(void *, const uint8_t *, size_t);
  void (*observe)(void *, size_t, int, int);
  void *user;
  struct uvc_bulk_stats stats;
};
int uvc_bulk_init(struct uvc_bulk_decoder *, size_t,
                  int, void (*)(void *, const uint8_t *, size_t), void *);
void uvc_bulk_feed(struct uvc_bulk_decoder *, const uint8_t *, size_t);
void uvc_bulk_discard(struct uvc_bulk_decoder *);
void uvc_bulk_destroy(struct uvc_bulk_decoder *);
#endif
