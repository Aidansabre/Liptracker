/* Focus 3 capture through the kernel uvcvideo driver. The Focus 3 headset
 * itself runs a Linux kernel, so this is the host stack the tracker is known
 * to work with: uvcvideo commits the mode at STREAMON and keeps several bulk
 * requests queued. See LICENSE. */
#define _GNU_SOURCE
#include "v4l2_capture.h"
#include "tracker.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BUFFERS 4

struct v4l2_capture {
  int fd, unit, length, started;
  char node[300];
  unsigned width, height, stride, size;
  struct { void *data; size_t length; } buffers[BUFFERS];
  unsigned count;
  uvc_frame_callback_t *callback;
  void *user;
  pthread_t thread;
  atomic_int running;
  unsigned long frames, errors, timeouts;
};

static int xioctl(int fd, unsigned long request, void *arg) {
  int r;
  do r = ioctl(fd, request, arg); while (r < 0 && errno == EINTR);
  return r;
}

static int read_number(const char *dir, const char *name, unsigned *value) {
  char path[PATH_MAX];
  if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return -1;
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  int ok = fscanf(f, "%u", value) == 1;
  fclose(f);
  return ok ? 0 : -1;
}

/* UVC GUID bytes for {2ccb0bda-6331-4fdb-850e-79054dbd5671}. */
static const uint8_t htc_guid[] = {0xda, 0x0b, 0xcb, 0x2c, 0x31, 0x63, 0xdb, 0x4f,
                                   0x85, 0x0e, 0x79, 0x05, 0x4d, 0xbd, 0x56, 0x71};

/* Find the HTC extension unit in the raw descriptors sysfs exposes. */
static int find_unit(const char *usb_dir) {
  char path[PATH_MAX];
  uint8_t data[8192];
  if (snprintf(path, sizeof path, "%s/descriptors", usb_dir) >= (int)sizeof path) return -1;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  size_t size = fread(data, 1, sizeof data, f);
  fclose(f);
  int unit = -1, matches = 0, video_control = 0;
  for (size_t at = 0; at + 2 <= size && data[at] >= 2 && at + data[at] <= size; at += data[at]) {
    const uint8_t *d = data + at;
    if (d[1] == 0x04 && d[0] >= 9) video_control = d[5] == 0x0e && d[6] == 0x01;
    if (video_control && d[1] == 0x24 && d[0] >= 20 && d[2] == 0x06 /* VC_EXTENSION_UNIT */ &&
        !memcmp(d + 4, htc_guid, sizeof htc_guid)) {
      unit = d[3];
      matches++;
    }
  }
  return matches == 1 ? unit : -1;
}

static int xu_query(struct v4l2_capture *c, uint8_t query, void *data, uint16_t size) {
  struct uvc_xu_control_query q = {.unit = c->unit, .selector = 2, .query = query,
                                   .size = size, .data = data};
  return xioctl(c->fd, UVCIOC_CTRL_QUERY, &q);
}

/* Return an open fd for the capture node on bus/address, or -1. */
static int find_node(unsigned bus, unsigned address, char *node, size_t node_size,
                     char *usb_dir, size_t usb_dir_size) {
  DIR *dir = opendir("/sys/class/video4linux");
  if (!dir) return -1;
  int fd = -1;
  for (struct dirent *e; fd < 0 && (e = readdir(dir));) {
    if (strncmp(e->d_name, "video", 5)) continue;
    char link[PATH_MAX], resolved[PATH_MAX];
    if (snprintf(link, sizeof link, "/sys/class/video4linux/%s/device", e->d_name) >= (int)sizeof link) continue;
    if (!realpath(link, resolved)) continue;
    char *slash = strrchr(resolved, '/'); /* USB interface -> USB device */
    if (!slash) continue;
    *slash = 0;
    unsigned b, a;
    if (read_number(resolved, "busnum", &b) || read_number(resolved, "devnum", &a) ||
        b != bus || a != address) continue;
    if (snprintf(node, node_size, "/dev/%s", e->d_name) >= (int)node_size) continue;
    int candidate = open(node, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (candidate < 0) {
      fprintf(stderr, "vft-stream: open %s: %s\n", node, strerror(errno));
      continue;
    }
    struct v4l2_capability cap = {0};
    uint32_t caps = 0;
    if (!xioctl(candidate, VIDIOC_QUERYCAP, &cap))
      caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING)) {
      fprintf(stderr, "vft-stream: V4L2 node %s driver=%s card=%s\n", node, cap.driver, cap.card);
      if (snprintf(usb_dir, usb_dir_size, "%s", resolved) >= (int)usb_dir_size) { close(candidate); continue; }
      fd = candidate;
    } else {
      close(candidate);
    }
  }
  closedir(dir);
  return fd;
}

static int pick_format(struct v4l2_capture *c) {
  struct v4l2_frmsizeenum size = {.pixel_format = V4L2_PIX_FMT_YUYV};
  if (xioctl(c->fd, VIDIOC_ENUM_FRAMESIZES, &size) || size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
    fprintf(stderr, "vft-stream: V4L2 node offers no discrete YUYV frame size\n");
    return -1;
  }
  struct v4l2_format f = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
  f.fmt.pix.width = size.discrete.width;
  f.fmt.pix.height = size.discrete.height;
  f.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(c->fd, VIDIOC_S_FMT, &f) || f.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
    fprintf(stderr, "vft-stream: V4L2 set YUYV format: %s\n", strerror(errno));
    return -1;
  }
  struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
  unsigned fps = 0;
  if (!xioctl(c->fd, VIDIOC_G_PARM, &parm) && parm.parm.capture.timeperframe.numerator)
    fps = parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator;
  c->width = f.fmt.pix.width;
  c->height = f.fmt.pix.height;
  c->stride = f.fmt.pix.bytesperline;
  c->size = f.fmt.pix.sizeimage;
  fprintf(stderr, "vft-stream: V4L2 format %ux%u YUYV stride=%u size=%u fps=%u\n",
          c->width, c->height, c->stride, c->size, fps);
  return 0;
}

int v4l2_capture_open(unsigned bus, unsigned address, int xu_unit, struct v4l2_capture **out) {
  *out = NULL;
  struct v4l2_capture *c = calloc(1, sizeof *c);
  if (!c) return -1;
  c->fd = -1;
  atomic_init(&c->running, 0);
  char usb_dir[PATH_MAX];
  c->fd = find_node(bus, address, c->node, sizeof c->node, usb_dir, sizeof usb_dir);
  if (c->fd < 0) {
    fprintf(stderr, "vft-stream: no uvcvideo capture node for USB %03u:%03u "
            "(is the uvcvideo kernel module loaded? see experiments.sh)\n", bus, address);
    free(c);
    return -1;
  }
  c->unit = xu_unit ? xu_unit : find_unit(usb_dir);
  if (c->unit <= 0) {
    fprintf(stderr, "vft-stream: cannot find the HTC XU in %s/descriptors; specify --xu-unit\n", usb_dir);
    v4l2_capture_close(c);
    return -1;
  }
  uint8_t length[2] = {0};
  if (xu_query(c, UVC_GET_LEN, length, sizeof length)) {
    fprintf(stderr, "vft-stream: V4L2 XU %d GET_LEN: %s\n", c->unit, strerror(errno));
    v4l2_capture_close(c);
    return -1;
  }
  c->length = length[0] | length[1] << 8;
  if (c->length < 17 || c->length > TRACKER_MAX_CONTROL) {
    fprintf(stderr, "vft-stream: unsupported selector-2 length %d for XU %d\n", c->length, c->unit);
    v4l2_capture_close(c);
    return -1;
  }
  fprintf(stderr, "vft-stream: using XU %d selector 2, %d-byte payload (uvcvideo)\n", c->unit, c->length);
  if (pick_format(c)) {
    v4l2_capture_close(c);
    return -1;
  }
  *out = c;
  return 0;
}

int v4l2_capture_xu_length(struct v4l2_capture *c) { return c->length; }

int v4l2_capture_send(void *context, const uint8_t *command, size_t length, int ack) {
  struct v4l2_capture *c = context;
  if (ack || length != (size_t)c->length) return -1;
  uint8_t data[TRACKER_MAX_CONTROL];
  memcpy(data, command, length);
  if (xu_query(c, UVC_SET_CUR, data, length)) {
    fprintf(stderr, "vft-stream: XU %d SET_CUR (uvcvideo): %s\n", c->unit, strerror(errno));
    return -1;
  }
  flockfile(stderr);
  fprintf(stderr, "vft-stream: XU %d SET_CUR sent %zu bytes prefix=", c->unit, length);
  for (size_t i = 0; i < length && i < 17; i++) fprintf(stderr, "%02x", command[i]);
  fputc('\n', stderr);
  funlockfile(stderr);
  return 0;
}

static void *read_loop(void *user) {
  struct v4l2_capture *c = user;
  while (atomic_load(&c->running)) {
    struct pollfd p = {.fd = c->fd, .events = POLLIN};
    int r = poll(&p, 1, 250);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) { fprintf(stderr, "vft-stream: V4L2 poll: %s\n", strerror(errno)); break; }
    if (!r) {
      if (c->timeouts++ < 4)
        fprintf(stderr, "vft-stream: V4L2 no buffer within 250 ms (frames=%lu)\n", c->frames);
      continue;
    }
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (xioctl(c->fd, VIDIOC_DQBUF, &b)) {
      if (errno == EAGAIN) continue;
      fprintf(stderr, "vft-stream: V4L2 DQBUF: %s\n", strerror(errno));
      break;
    }
    int error = !!(b.flags & V4L2_BUF_FLAG_ERROR);
    c->frames++;
    if (error) c->errors++;
    if (c->frames <= 3 || (error && c->errors <= 3))
      fprintf(stderr, "vft-stream: V4L2 buffer %lu: bytes=%u expected=%u error=%d\n",
              c->frames, b.bytesused, c->size, error);
    if (!error && b.index < c->count) {
      uvc_frame_t frame = {.data = c->buffers[b.index].data, .data_bytes = b.bytesused,
        .width = c->width, .height = c->height, .step = c->stride,
        .frame_format = UVC_FRAME_FORMAT_YUYV};
      c->callback(&frame, c->user);
    }
    if (xioctl(c->fd, VIDIOC_QBUF, &b)) {
      fprintf(stderr, "vft-stream: V4L2 QBUF: %s\n", strerror(errno));
      break;
    }
  }
  return NULL;
}

int v4l2_capture_start(struct v4l2_capture *c, uvc_frame_callback_t *callback, void *user) {
  if (!c || c->started || !callback) return -1;
  struct v4l2_requestbuffers req = {.count = BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                    .memory = V4L2_MEMORY_MMAP};
  if (xioctl(c->fd, VIDIOC_REQBUFS, &req) || !req.count) {
    fprintf(stderr, "vft-stream: V4L2 REQBUFS: %s\n", strerror(errno));
    return -1;
  }
  c->count = req.count > BUFFERS ? BUFFERS : req.count;
  for (unsigned i = 0; i < c->count; i++) {
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
    if (xioctl(c->fd, VIDIOC_QUERYBUF, &b)) return -1;
    c->buffers[i].length = b.length;
    c->buffers[i].data = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
    if (c->buffers[i].data == MAP_FAILED) { c->buffers[i].data = NULL; return -1; }
    if (xioctl(c->fd, VIDIOC_QBUF, &b)) return -1;
  }
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(c->fd, VIDIOC_STREAMON, &type)) {
    fprintf(stderr, "vft-stream: V4L2 STREAMON: %s\n", strerror(errno));
    return -1;
  }
  fprintf(stderr, "vft-stream: V4L2 streaming on %s (uvcvideo committed after activation)\n", c->node);
  c->callback = callback;
  c->user = user;
  atomic_store(&c->running, 1);
  if (pthread_create(&c->thread, NULL, read_loop, c)) {
    atomic_store(&c->running, 0);
    xioctl(c->fd, VIDIOC_STREAMOFF, &type);
    return -1;
  }
  c->started = 1;
  return 0;
}

void v4l2_capture_stop(struct v4l2_capture *c) {
  if (!c || !c->started) return;
  atomic_store(&c->running, 0);
  pthread_join(c->thread, NULL);
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  xioctl(c->fd, VIDIOC_STREAMOFF, &type); /* uvcvideo clears the bulk halt. */
  c->started = 0;
  fprintf(stderr, "vft-stream: V4L2 totals: buffers=%lu errors=%lu poll-timeouts=%lu\n",
          c->frames, c->errors, c->timeouts);
}

void v4l2_capture_close(struct v4l2_capture *c) {
  if (!c) return;
  v4l2_capture_stop(c);
  for (unsigned i = 0; i < c->count; i++)
    if (c->buffers[i].data) munmap(c->buffers[i].data, c->buffers[i].length);
  if (c->fd >= 0) close(c->fd);
  free(c);
}
