/* Modified 2026. See LICENSE. */
#ifndef IMAGE_H
#define IMAGE_H
#include "tracker.h"
#define IMAGE_MAX_PIXELS (1920u * 1080u)
void image_init(void);
/* rotation is degrees counterclockwise; raw bypasses all profile processing. */
int image_process(enum tracker_profile profile, int rotation, int raw,
                  const uint8_t *data, size_t bytes, unsigned width, unsigned height,
                  size_t stride, uint8_t *out, size_t capacity, int *ow, int *oh);
#endif
