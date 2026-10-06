#include "image.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t input[480 * (480 * 2 + 8)], output[IMAGE_MAX_PIXELS];
static uint8_t sample(unsigned x, unsigned y) { return (x * 13 + y * 17) & 255; }
int main(int argc, char **argv) {
  int ow, oh;
  image_init();
  for (int rotation = 0; rotation < 360; rotation += 90) {
    unsigned w = rotation == 90 || rotation == 270 ? 480 : 320;
    unsigned h = rotation == 90 || rotation == 270 ? 320 : 480;
    size_t stride = w * 2 + 8;
    memset(input, 0xee, sizeof input);
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) input[y * stride + x * 2] = sample(x, y);
    assert(!image_process(TRACKER_FOCUS3, rotation, 0, input, h * stride, w, h,
                          stride, output, sizeof output, &ow, &oh));
    assert(ow == 320 && oh == 480);
    for (unsigned y = 0; y < h; y++)
      for (unsigned x = 0; x < w; x++) {
        unsigned dx = x, dy = y;
        if (rotation == 90) { dx = y; dy = w - x - 1; }
        if (rotation == 180) { dx = w - x - 1; dy = h - y - 1; }
        if (rotation == 270) { dx = h - y - 1; dy = x; }
        assert(output[dy * 320 + dx] == sample(x, y));
      }
    assert(!image_process(TRACKER_FOCUS3, rotation, 1, input, h * stride, w, h,
                          stride, output, sizeof output, &ow, &oh));
    assert(ow == (int)w && oh == (int)h);
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++)
      assert(output[y * w + x] == sample(x, y));
    assert(image_process(TRACKER_FOCUS3, rotation, 0, input, h * stride - 1, w, h,
                          stride, output, sizeof output, &ow, &oh));
  }
  /* 2x area reduction: fixture has an average luminance of 75 in every block. */
  uint8_t *large = malloc(640 * 960 * 2);
  assert(large);
  for (unsigned y = 0; y < 960; y++) for (unsigned x = 0; x < 640; x++) {
    large[(y * 640 + x) * 2] = (x % 2) * 50 + (y % 2) * 100;
    large[(y * 640 + x) * 2 + 1] = 255;
  }
  assert(!image_process(TRACKER_FOCUS3, 0, 0, large, 640 * 960 * 2, 640, 960,
                        1280, output, sizeof output, &ow, &oh));
  for (int i = 0; i < 320 * 480; i++) assert(output[i] == 75);
  free(large);
  /* Actual Focus 3 descriptors: 640x481, including the odd final row. */
  large = malloc(640 * 481 * 2); assert(large);
  for (int i = 0; i < 640 * 481; i++) { large[i * 2] = 203; large[i * 2 + 1] = 17; }
  assert(!image_process(TRACKER_FOCUS3, 90, 0, large, 640 * 481 * 2, 640, 481,
                        1280, output, sizeof output, &ow, &oh));
  assert(ow == 320 && oh == 480);
  for (int i = 0; i < 320 * 480; i++) assert(output[i] == 203);
  free(large);
  assert(image_process(TRACKER_FOCUS3, 0, 0, input, sizeof input, 0, 4, 16, output, sizeof output, &ow, &oh));
  assert(image_process(TRACKER_FOCUS3, 0, 0, input, sizeof input, 3, 4, 16, output, sizeof output, &ow, &oh));
  assert(image_process(TRACKER_FOCUS3, 45, 0, input, sizeof input, 4, 4, 8, output, sizeof output, &ow, &oh));
  assert(image_process(TRACKER_FOCUS3, 0, 0, input, sizeof input, 4, 4, 4, output, sizeof output, &ow, &oh));
  assert(image_process(TRACKER_FOCUS3, 0, 0, input, sizeof input, 4, 4, 8, output, 10, &ow, &oh));
  /* Deterministic original-VFT fixture, checked against the unmodified upstream. */
  for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 8; x++) {
    input[y * 16 + x * 2] = sample(x, y); input[y * 16 + x * 2 + 1] = 255;
  }
  assert(!image_process(TRACKER_VFT, 90, 0, input, 64, 8, 4, 16, output, sizeof output, &ow, &oh));
  assert(ow == 400 && oh == 400);
  if (argc == 2) {
    FILE *f = fopen(argv[1], "wb"); assert(f);
    assert(fwrite(output, 1, 400 * 400, f) == 400 * 400); assert(!fclose(f));
  }
  puts("image: all rotations, full frame, padded stride, raw output, area scaling, and invalid frames passed");
}
