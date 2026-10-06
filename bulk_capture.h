/* Direct Focus 3 USB bulk capture. See LICENSE. */
#ifndef BULK_CAPTURE_H
#define BULK_CAPTURE_H
#include <libuvc/libuvc.h>
struct bulk_capture;
int bulk_endpoint(uvc_device_handle_t *, const uvc_stream_ctrl_t *, uint8_t *);
int bulk_capture_open(uvc_device_handle_t *, const uvc_stream_ctrl_t *,
                      unsigned, unsigned, int, unsigned, struct bulk_capture **);
int bulk_capture_start(struct bulk_capture *, uvc_frame_callback_t *, void *);
void bulk_capture_stop(struct bulk_capture *);
void bulk_capture_close(struct bulk_capture *);
#endif
