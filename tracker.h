/* Focus 3 support, 2026. See LICENSE and THIRD_PARTY_NOTICES.md. */
#ifndef TRACKER_H
#define TRACKER_H
#include <stddef.h>
#include <stdint.h>

#define HTC_VID 0x0bb4
#define VFT_PID 0x0321
#define FOCUS3_PID 0x06a1
#define TRACKER_MAX_CONTROL 4096

enum tracker_profile { TRACKER_AUTO, TRACKER_VFT, TRACKER_FOCUS3 };
struct tracker_io {
  void *context;
  size_t length;
  int (*send)(void *, const uint8_t *, size_t, int acknowledge);
  void (*sleep_ms)(void *, unsigned);
};
enum tracker_profile tracker_identify(uint16_t vid, uint16_t pid);
const char *tracker_name(enum tracker_profile profile);
int tracker_set_state(enum tracker_profile profile, int on, const struct tracker_io *io);
#endif
