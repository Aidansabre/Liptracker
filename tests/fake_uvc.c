/* Test-only USB backend. Exercises the real application and HTTP server. */
#define _DEFAULT_SOURCE
#include <libuvc/libuvc.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct uvc_context { int unused; };
struct uvc_device { int unused; };
struct uvc_device_handle { int unused; };
static struct uvc_context context;
static struct uvc_device devices[2];
static struct uvc_device_handle handle;
static atomic_int running;
static pthread_t thread;
static uvc_frame_callback_t *callback;
static void *callback_context;
static int mode(const char *name) {
  const char *value = getenv("VFT_TEST_MODE");
  return value && !strcmp(value, name);
}
static FILE *log_open(void) { const char *path = getenv("VFT_TEST_LOG"); return path ? fopen(path, "a") : NULL; }
uvc_error_t uvc_init(uvc_context_t **ctx, struct libusb_context *usb) {
  (void)usb; *ctx = &context; return UVC_SUCCESS;
}
void uvc_exit(uvc_context_t *ctx) { assert(ctx == &context); }
uvc_error_t uvc_get_device_list(uvc_context_t *ctx, uvc_device_t ***list) {
  (void)ctx;
  *list = calloc(3, sizeof **list); assert(*list);
  (*list)[0] = &devices[0];
  if (mode("ambiguous")) (*list)[1] = &devices[1];
  return UVC_SUCCESS;
}
void uvc_free_device_list(uvc_device_t **list, uint8_t unref) { assert(unref); free(list); }
uvc_error_t uvc_get_device_descriptor(uvc_device_t *dev, uvc_device_descriptor_t **desc) {
  (void)dev; *desc = calloc(1, sizeof **desc); assert(*desc);
  (*desc)->idVendor = 0x0bb4; (*desc)->idProduct = mode("unknown") ? 0x9999 : 0x06a1;
  (*desc)->product = "HTC Lip Camera";
  return UVC_SUCCESS;
}
void uvc_free_device_descriptor(uvc_device_descriptor_t *desc) { free(desc); }
uvc_error_t uvc_open(uvc_device_t *dev, uvc_device_handle_t **out) { (void)dev; *out = &handle; return UVC_SUCCESS; }
void uvc_close(uvc_device_handle_t *dev) { assert(dev == &handle); }
const char *uvc_strerror(uvc_error_t error) { (void)error; return "fake USB error"; }
const uvc_extension_unit_t *uvc_get_extension_units(uvc_device_handle_t *dev) {
  (void)dev;
  static uvc_extension_unit_t unit = {.bUnitID = 4, .bmControls = 3,
    .guidExtensionCode = {0xda, 0x0b, 0xcb, 0x2c, 0x31, 0x63, 0xdb, 0x4f, 0x85, 0x0e, 0x79, 0x05, 0x4d, 0xbd, 0x56, 0x71}};
  return mode("missing-xu") ? NULL : &unit;
}
int uvc_get_ctrl_len(uvc_device_handle_t *dev, uint8_t unit, uint8_t selector) {
  (void)dev; assert(unit == 4 && selector == 2);
  return mode("invalid-length") ? 8 : 64;
}
int uvc_set_ctrl(uvc_device_handle_t *dev, uint8_t unit, uint8_t selector, void *data, int length) {
  (void)dev; assert(unit == 4 && selector == 2 && length == 64);
  uint8_t *bytes = data;
  FILE *log = log_open();
  if (log) { for (int i = 0; i < length; i++) fprintf(log, "%02x", bytes[i]); fputc('\n', log); fclose(log); }
  if (mode("control-failure") && bytes[1] == 0xa2 && bytes[16] == 0x11) return UVC_ERROR_PIPE;
  return length;
}
int uvc_get_ctrl(uvc_device_handle_t *dev, uint8_t unit, uint8_t selector, void *data, int len, enum uvc_req_code req) {
  (void)dev; (void)unit; (void)selector; (void)data; (void)len; (void)req;
  assert(!"Focus 3 must never poll GET_CUR"); return UVC_ERROR_IO;
}
const uvc_format_desc_t *uvc_get_format_descs(uvc_device_handle_t *dev) {
  (void)dev;
  static uvc_frame_desc_t frame = {.wWidth = 640, .wHeight = 481, .dwDefaultFrameInterval = 333333};
  static uvc_format_desc_t format = {.bDescriptorSubtype = UVC_VS_FORMAT_UNCOMPRESSED,
                                   .fourccFormat = {'Y', 'U', 'Y', '2'}, .frame_descs = &frame};
  return &format;
}
uvc_error_t uvc_get_stream_ctrl_format_size(uvc_device_handle_t *dev, uvc_stream_ctrl_t *ctrl,
                                          enum uvc_frame_format format, int width, int height, int fps) {
  (void)dev; (void)ctrl;
  assert(format == UVC_FRAME_FORMAT_YUYV && width == 640 && height == 481 && fps == 30);
  return UVC_SUCCESS;
}
void uvc_print_diag(uvc_device_handle_t *dev, FILE *stream) { (void)dev; fprintf(stream, "fake descriptors: 640x481 YUY2 bulk 30 FPS\n"); }
static void *frames(void *arg) {
  (void)arg;
  uint8_t *data = malloc(640 * 481 * 2); assert(data);
  for (int i = 0; i < 640 * 481; i++) { data[i * 2] = 64; data[i * 2 + 1] = 255; }
  uvc_frame_t frame = {.data = data, .data_bytes = 640 * 481 * 2,
    .width = 640, .height = 481, .frame_format = UVC_FRAME_FORMAT_YUYV, .step = 1280};
  int count = 0;
  while (atomic_load(&running)) {
    if (!mode("stall") || !count) callback(&frame, callback_context);
    count++;
    usleep(33333);
  }
  free(data); return NULL;
}
uvc_error_t uvc_start_streaming(uvc_device_handle_t *dev, uvc_stream_ctrl_t *ctrl,
                              uvc_frame_callback_t *cb, void *user, uint8_t flags) {
  (void)dev; (void)ctrl; (void)flags;
  if (mode("start-failure")) return UVC_ERROR_IO;
  callback = cb; callback_context = user; atomic_store(&running, 1);
  return pthread_create(&thread, NULL, frames, NULL) ? UVC_ERROR_IO : UVC_SUCCESS;
}
void uvc_stop_streaming(uvc_device_handle_t *dev) {
  (void)dev; atomic_store(&running, 0); pthread_join(thread, NULL);
}
