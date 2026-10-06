/* Modified 2026: separate Focus 3 image path. Original VFT processing
 * derives from vft-stream/Baballonia; see vft-stream.c and LICENSE. */
#include "image.h"
#include <math.h>
#include <string.h>
#define OUT_W 400
#define OUT_H 400

static uint8_t lut[256];
static float gk[5];

void image_init(void) {
  // Baballonia's gamma LUT, verbatim — including the /2048, which maps 0..255
  // onto roughly 0..110. The model was trained on that, so keep it.
  for (int i = 0; i < 256; i++)
    lut[i] = (uint8_t)(pow(i / 2048.0, 1.0 / 2.5) * 255.0);
  // OpenCV GaussianBlur(5x5, sigma 0) derives sigma = 1.1.
  double s = 0.3 * ((5 - 1) * 0.5 - 1) + 0.8, sum = 0;
  for (int i = 0; i < 5; i++) sum += gk[i] = exp(-((i - 2) * (i - 2)) / (2 * s * s));
  for (int i = 0; i < 5; i++) gk[i] /= sum;
}

static int reflect101(int i, int n) {
  if (i < 0) return -i;
  if (i >= n) return 2 * n - i - 2;
  return i;
}

// YUYV -> Y -> left half -> bilinear to 400x400 -> gaussian 5x5 -> LUT.
// With raw set, only the Y extraction happens (for eyeballing the full frame).
static void process_vft(const uint8_t *yuyv, int w, int h, size_t stride, int raw,
                    uint8_t *out, int *ow, int *oh) {
  static uint8_t gray[1920 * 1080];
  static float tmp[OUT_W * OUT_H], rs[OUT_W * OUT_H];

  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) gray[y * w + x] = yuyv[y * stride + 2 * x];

  if (raw) {
    memcpy(out, gray, (size_t)w * h);
    *ow = w, *oh = h;
    return;
  }

  // cv::resize INTER_LINEAR, half-pixel centres, over columns [0, w/2).
  int cw = w / 2;
  float sx = (float)cw / OUT_W, sy = (float)h / OUT_H;
  for (int y = 0; y < OUT_H; y++) {
    float fy = (y + 0.5f) * sy - 0.5f;
    if (fy < 0) fy = 0;
    int y0 = (int)fy, y1 = y0 + 1 < h ? y0 + 1 : h - 1;
    float ay = fy - y0;
    for (int x = 0; x < OUT_W; x++) {
      float fx = (x + 0.5f) * sx - 0.5f;
      if (fx < 0) fx = 0;
      int x0 = (int)fx, x1 = x0 + 1 < cw ? x0 + 1 : cw - 1;
      float ax = fx - x0;
      float top = gray[y0 * w + x0] * (1 - ax) + gray[y0 * w + x1] * ax;
      float bot = gray[y1 * w + x0] * (1 - ax) + gray[y1 * w + x1] * ax;
      rs[y * OUT_W + x] = top * (1 - ay) + bot * ay;
    }
  }

  for (int y = 0; y < OUT_H; y++)
    for (int x = 0; x < OUT_W; x++) {
      float a = 0;
      for (int k = 0; k < 5; k++) a += gk[k] * rs[y * OUT_W + reflect101(x + k - 2, OUT_W)];
      tmp[y * OUT_W + x] = a;
    }
  for (int y = 0; y < OUT_H; y++)
    for (int x = 0; x < OUT_W; x++) {
      float a = 0;
      for (int k = 0; k < 5; k++) a += gk[k] * tmp[reflect101(y + k - 2, OUT_H) * OUT_W + x];
      int v = (int)(a + 0.5f);
      out[y * OUT_W + x] = lut[v < 0 ? 0 : v > 255 ? 255 : v];
    }
  *ow = OUT_W, *oh = OUT_H;
}

int image_process(enum tracker_profile profile, int rotation, int raw,
                  const uint8_t *data, size_t bytes, unsigned w, unsigned h,
                  size_t stride, uint8_t *out, size_t capacity, int *ow, int *oh) {
  if (!data || !out || !ow || !oh || !w || !h || w > IMAGE_MAX_PIXELS ||
      h > IMAGE_MAX_PIXELS || (size_t)w * h > IMAGE_MAX_PIXELS ||
      (w & 1) || stride < (size_t)w * 2 || stride > bytes / h ||
      (profile != TRACKER_VFT && profile != TRACKER_FOCUS3) ||
      (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270)) return -1;
  size_t required = raw ? (size_t)w * h : profile == TRACKER_VFT ? 400u * 400u : 320u * 480u;
  if (capacity < required) return -1;
  if (profile == TRACKER_VFT || raw) {
    process_vft(data, w, h, stride, raw, out, ow, oh);
    return 0;
  }

  /* Focus 3 is one camera: retain all Y samples without the VFT crop or LUT.
   * The reference rotates 90 degrees CCW and scales to a 320x480 portrait. */
  static uint8_t rotated[IMAGE_MAX_PIXELS];
  unsigned rw = rotation == 90 || rotation == 270 ? h : w;
  unsigned rh = rotation == 90 || rotation == 270 ? w : h;
  for (unsigned y = 0; y < h; y++)
    for (unsigned x = 0; x < w; x++) {
      unsigned dx = x, dy = y;
      if (rotation == 90) { dx = y; dy = w - 1 - x; }
      if (rotation == 180) { dx = w - 1 - x; dy = h - 1 - y; }
      if (rotation == 270) { dx = h - 1 - y; dy = x; }
      rotated[(size_t)dy * rw + dx] = data[(size_t)y * stride + x * 2];
    }

  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 320; x++) {
      double value;
      if (rw >= 320 && rh >= 480) {
        /* Area-weighted downsampling, as in the reference's INTER_AREA. */
        double left = x * (double)rw / 320, right = (x + 1) * (double)rw / 320;
        double top = y * (double)rh / 480, bottom = (y + 1) * (double)rh / 480;
        double sum = 0;
        for (unsigned sy = (unsigned)top; sy < rh && sy < (unsigned)ceil(bottom); sy++)
          for (unsigned sx = (unsigned)left; sx < rw && sx < (unsigned)ceil(right); sx++) {
            double weight = (fmin(right, sx + 1) - fmax(left, sx)) *
                            (fmin(bottom, sy + 1) - fmax(top, sy));
            sum += rotated[(size_t)sy * rw + sx] * weight;
          }
        value = sum / ((right - left) * (bottom - top));
      } else {
        /* Bilinear interpolation for modes smaller than the output. */
        double fx = fmax(0, (x + 0.5) * rw / 320 - 0.5);
        double fy = fmax(0, (y + 0.5) * rh / 480 - 0.5);
        unsigned x0 = (unsigned)fx, y0 = (unsigned)fy;
        unsigned x1 = x0 + 1 < rw ? x0 + 1 : rw - 1;
        unsigned y1 = y0 + 1 < rh ? y0 + 1 : rh - 1;
        double ax = fx - x0, ay = fy - y0;
        value = (rotated[(size_t)y0 * rw + x0] * (1 - ax) + rotated[(size_t)y0 * rw + x1] * ax) * (1 - ay) +
                (rotated[(size_t)y1 * rw + x0] * (1 - ax) + rotated[(size_t)y1 * rw + x1] * ax) * ay;
      }
      out[y * 320 + x] = (uint8_t)fmin(255, fmax(0, floor(value + 0.5)));
    }
  *ow = 320;
  *oh = 480;
  return 0;
}
