/* Focus 3 capture through the kernel uvcvideo driver. See LICENSE. */
#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H
#include <libuvc/libuvc.h>
#include <stddef.h>
#include <stdint.h>
struct v4l2_capture;
/* Find the uvcvideo capture node for a USB bus/address and its HTC XU. */
int v4l2_capture_open(unsigned bus, unsigned address, int xu_unit, struct v4l2_capture **);
int v4l2_capture_xu_length(struct v4l2_capture *);
/* tracker_io send: SET_CUR on XU selector 2; acknowledgements unsupported. */
int v4l2_capture_send(void *, const uint8_t *, size_t, int);
/* STREAMON (uvcvideo commits here), then deliver frames from a thread. */
int v4l2_capture_start(struct v4l2_capture *, uvc_frame_callback_t *, void *);
void v4l2_capture_stop(struct v4l2_capture *);
void v4l2_capture_close(struct v4l2_capture *);
#endif
