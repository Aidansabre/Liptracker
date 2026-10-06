/* Test-only USB backend. Exercises the real application and HTTP server. */
#define _DEFAULT_SOURCE
#include <libusb.h>
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
struct uvc_stream_handle { int open, started; };
struct libusb_device { int unused; };
struct libusb_device_handle { int unused; };
static struct libusb_device usb_device;
static struct libusb_device_handle usb_handle;
static struct uvc_context context;
static struct uvc_device devices[2];
static struct uvc_device_handle handle;
static struct uvc_stream_handle stream;
static atomic_int sensor_on, ir_on;
static atomic_int running;
static atomic_int bulk_read_started;
static pthread_t thread;
static pthread_t main_thread;
static atomic_int recommitted;
static uvc_frame_callback_t *callback;
static void *callback_context;
static int mode(const char *name) {
  const char *value = getenv("VFT_TEST_MODE");
  return value && !strcmp(value, name);
}
static FILE *log_open(void) { const char *path = getenv("VFT_TEST_LOG"); return path ? fopen(path, "a") : NULL; }
static void event(const char *name) {
  const char *path = getenv("VFT_TEST_EVENTS");
  FILE *log = path ? fopen(path, "a") : NULL;
  if (log) { fprintf(log, "%s\n", name); fclose(log); }
}
uvc_error_t uvc_init(uvc_context_t **ctx, struct libusb_context *usb) {
  (void)usb; main_thread = pthread_self(); *ctx = &context; return UVC_SUCCESS;
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
void uvc_close(uvc_device_handle_t *dev) {
  assert(dev == &handle && !stream.open && !stream.started);
}
const char *uvc_strerror(uvc_error_t error) { (void)error; return "fake USB error"; }
libusb_device_handle *uvc_get_libusb_handle(uvc_device_handle_t *dev) {
  assert(dev == &handle); return &usb_handle;
}
libusb_device *libusb_get_device(libusb_device_handle *dev) {
  assert(dev == &usb_handle); return &usb_device;
}
int libusb_get_active_config_descriptor(libusb_device *dev, struct libusb_config_descriptor **out) {
  assert(dev == &usb_device);
  if (mode("probe-descriptor-failure")) return LIBUSB_ERROR_IO;
  static struct libusb_endpoint_descriptor ep;
  static struct libusb_interface_descriptor alt;
  static struct libusb_interface interface;
  ep = (struct libusb_endpoint_descriptor){.bEndpointAddress = 0x81,
          .bmAttributes = mode("probe-invalid-endpoint") ? LIBUSB_TRANSFER_TYPE_INTERRUPT : LIBUSB_TRANSFER_TYPE_BULK};
  alt = (struct libusb_interface_descriptor){.bInterfaceNumber = 1, .bNumEndpoints = 1, .endpoint = &ep};
  interface = (struct libusb_interface){.altsetting = &alt, .num_altsetting = 1};
  *out = calloc(1, sizeof **out); assert(*out);
  (*out)->bNumInterfaces = 1;
  (*out)->interface = &interface;
  return LIBUSB_SUCCESS;
}
void libusb_free_config_descriptor(struct libusb_config_descriptor *config) { free(config); }
const char *libusb_error_name(int error) {
  if (!error) return "LIBUSB_SUCCESS";
  if (error == LIBUSB_ERROR_TIMEOUT) return "LIBUSB_ERROR_TIMEOUT";
  if (error == LIBUSB_ERROR_PIPE) return "LIBUSB_ERROR_PIPE";
  return "LIBUSB_ERROR_IO";
}
static int capture_transfer(unsigned char *data, int length, int *got, unsigned timeout) {
  static uint8_t payload[16384];
  static size_t payload_size, payload_at, frame_at;
  static uint32_t frame_number;
  static int partial_injected;
  atomic_store(&bulk_read_started, 1);
  *got = 0;
  for (unsigned waited = 0; (!sensor_on || !ir_on) && waited < timeout; waited += 5) usleep(5000);
  if (!sensor_on || !ir_on) return LIBUSB_ERROR_TIMEOUT;
  if (mode("bulk-empty") || (mode("bulk-stall") && frame_number) ||
      (mode("bulk-recommit-required") && !recommitted)) {
    usleep(timeout * 1000); return LIBUSB_ERROR_TIMEOUT;
  }
  if (mode("bulk-read-error")) return LIBUSB_ERROR_IO;
  if (payload_at == payload_size) {
    size_t frame_size = mode("bulk-incomplete") ? 640*480*2 : 640*481*2;
    if (mode("bulk-first-short") && !frame_number) frame_size = 131024;
    size_t count = frame_size - frame_at;
    if (count > sizeof payload - 12) count = sizeof payload - 12;
    memset(payload, 0, 12);
    payload[0] = 12;
    payload[1] = 0x0c | (frame_number & 1) | (frame_at+count == frame_size ? 2 : 0);
    if (mode("bulk-error") || (mode("bulk-error-recovery") && !frame_number)) payload[1] |= 0x40;
    for (int i = 0; i < 4; i++) payload[2+i] = (frame_number + 1) >> (8*i);
    for (size_t i = 0; i < count; i++) payload[12+i] = ((frame_at+i) & 1) ? 255 : 64;
    payload_size = count+12; payload_at = 0;
    frame_at += count;
    if (frame_at == frame_size) { frame_at = 0; frame_number++; }
  }
  size_t count = payload_size-payload_at;
  if (count > (size_t)length) count = length;
  int partial = mode("bulk-partial-timeout") && !partial_injected;
  if (partial) { count = 1000; partial_injected = 1; }
  memcpy(data, payload+payload_at, count);
  payload_at += count;
  *got = count;
  usleep(500);
  return partial ? LIBUSB_ERROR_TIMEOUT : LIBUSB_SUCCESS;
}
int libusb_bulk_transfer(libusb_device_handle *dev, unsigned char endpoint,
                        unsigned char *data, int length, int *got, unsigned int timeout) {
  assert(dev == &usb_handle && endpoint == 0x81 && length > 0 && length <= 16384 && timeout && timeout <= 5000);
  assert(stream.open && !stream.started);
  if (!pthread_equal(pthread_self(), main_thread)) return capture_transfer(data, length, got, timeout);
  assert(sensor_on && ir_on);
  event("bulk-read");
  *got = 0;
  if (mode("probe-pipe")) return LIBUSB_ERROR_PIPE;
  if (mode("probe-empty")) { usleep(timeout * 1000); return LIBUSB_ERROR_TIMEOUT; }
  memset(data, 0, length);
  data[0] = 12; data[1] = 0x82;
  *got = mode("probe-partial") ? 512 : length;
  return mode("probe-partial") ? LIBUSB_ERROR_TIMEOUT : LIBUSB_SUCCESS;
}
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
  assert(stream.open); /* Vendor writes must follow the stream commit. */
  if (bytes[1] == 0x14) {
    if (bytes[3] == 1 && mode("capture-first-required") && !stream.started && !bulk_read_started) return UVC_ERROR_IO;
    sensor_on = bytes[3] == 1;
    event(sensor_on ? "stream-on" : "stream-off");
  } else if (bytes[1] == 0xa2) {
    event(bytes[16] == 0x11 ? "ir-on" : "ir-off");
  }
  if (mode("control-failure") && bytes[1] == 0xa2 && bytes[16] == 0x11) return UVC_ERROR_PIPE;
  if (bytes[1] == 0xa2) ir_on = bytes[16] == 0x11;
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
  (void)dev;
  assert(format == UVC_FRAME_FORMAT_YUYV && width == 640 && height == 481 && fps == 30);
  assert(!ctrl->bmHint && !ctrl->dwFrameInterval && !ctrl->dwMaxVideoFrameSize &&
         !ctrl->dwMaxPayloadTransferSize);
  ctrl->bInterfaceNumber = 1;
  ctrl->bFormatIndex = ctrl->bFrameIndex = 1;
  ctrl->dwFrameInterval = 333333;
  ctrl->dwMaxVideoFrameSize = 640 * 481 * 2;
  ctrl->dwMaxPayloadTransferSize = 16384;
  return UVC_SUCCESS;
}
void uvc_print_diag(uvc_device_handle_t *dev, FILE *stream) { (void)dev; fprintf(stream, "fake descriptors: 640x481 YUY2 bulk 30 FPS\n"); }
static void *frames(void *arg) {
  (void)arg;
  uint8_t *data = malloc(640 * 481 * 2); assert(data);
  for (int i = 0; i < 640 * 481; i++) { data[i * 2] = 64; data[i * 2 + 1] = 255; }
  uvc_frame_t frame = {.data = data, .data_bytes = 640 * 481 * 2,
    .width = 640, .height = 481, .frame_format = UVC_FRAME_FORMAT_YUYV, .step = 1280};
  if (mode("rejected-frames")) frame.data_bytes -= 1280;
  int count = 0;
  while (atomic_load(&running)) {
    if (sensor_on && ir_on && !mode("no-callbacks") && (!mode("delayed-start") || count >= 105) &&
        (!mode("stall") || !count)) callback(&frame, callback_context);
    count++;
    usleep(33333);
  }
  free(data); return NULL;
}
uvc_error_t uvc_stream_open_ctrl(uvc_device_handle_t *dev, uvc_stream_handle_t **out,
                                uvc_stream_ctrl_t *ctrl) {
  assert(dev == &handle && !stream.open && ctrl->bInterfaceNumber == 1);
  event("commit");
  if (mode("commit-failure")) return UVC_ERROR_IO;
  /* Simulate a camera whose UVC configuration resets vendor activation. */
  sensor_on = ir_on = 0;
  stream.open = 1;
  *out = &stream;
  return UVC_SUCCESS;
}
uvc_error_t uvc_stream_ctrl(uvc_stream_handle_t *capture, uvc_stream_ctrl_t *ctrl) {
  assert(capture == &stream && stream.open && !stream.started && ctrl->bInterfaceNumber == 1);
  assert(sensor_on && ir_on);
  event("recommit");
  if (mode("recommit-failure")) return UVC_ERROR_IO;
  atomic_store(&recommitted, 1);
  return UVC_SUCCESS;
}
uvc_error_t uvc_stream_start(uvc_stream_handle_t *capture, uvc_frame_callback_t *cb,
                             void *user, uint8_t flags) {
  (void)flags;
  assert(capture == &stream && stream.open && !stream.started);
  event("start");
  if (mode("start-failure")) return UVC_ERROR_IO;
  callback = cb; callback_context = user; atomic_store(&running, 1);
  if (pthread_create(&thread, NULL, frames, NULL)) {
    atomic_store(&running, 0);
    return UVC_ERROR_IO;
  }
  stream.started = 1;
  return UVC_SUCCESS;
}
uvc_error_t uvc_stream_stop(uvc_stream_handle_t *capture) {
  assert(capture == &stream && stream.started);
  event("stop");
  atomic_store(&running, 0); pthread_join(thread, NULL);
  stream.started = 0;
  return UVC_SUCCESS;
}
void uvc_stream_close(uvc_stream_handle_t *capture) {
  assert(capture == &stream && stream.open && !stream.started);
  event("close");
  stream.open = 0;
}
uvc_error_t uvc_start_streaming(uvc_device_handle_t *dev, uvc_stream_ctrl_t *ctrl,
                              uvc_frame_callback_t *cb, void *user, uint8_t flags) {
  uvc_stream_handle_t *capture = NULL;
  uvc_error_t result = uvc_stream_open_ctrl(dev, &capture, ctrl);
  if (result < 0) return result;
  result = uvc_stream_start(capture, cb, user, flags);
  if (result < 0) uvc_stream_close(capture);
  return result;
}
void uvc_stop_streaming(uvc_device_handle_t *dev) {
  assert(dev == &handle);
  uvc_stream_stop(&stream);
  uvc_stream_close(&stream);
}
