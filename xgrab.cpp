#include <iostream>
#include <string>

#ifdef __cplusplus
extern "C"
{
  #include "xcb/xcb.h"

  #include "libavutil/mathematics.h"
  #include "libavutil/opt.h"
  #include "libavutil/parseutils.h"
  #include "libavutil/time.h"
  #include "libavutil/buffer.h"

  #include "libavformat/avformat.h"
}
#endif

typedef struct XGrabContext {
  // XCB first class objects
  xcb_connection_t *conn;
  xcb_screen_t *screen;
  xcb_window_t window;

  // Frame and time info
  int64_t time_frame;
  int64_t frame_duration;
  AVRational time_base;
  const char *framerate;

  // XCB window/frame positioning info
  xcb_window_t window_id;
  int x, y;
  int width, height;
  int frame_size;
  int bpp;
} XGrabContext;

static void xgrab_image_reply_free(void *opaque, uint8_t *data) {
    free(opaque);
}

static int xgrab_frame(XGrabContext *c, AVPacket *pkt) {
  xcb_get_image_cookie_t iq;
  xcb_get_image_reply_t *img;
  xcb_drawable_t drawable = c->window_id;
  xcb_generic_error_t *e = NULL;
  uint8_t *data;
  int length;

  iq = xcb_get_image(c->conn, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                     c->x, c->y, c->width, c->height, ~0);

  img = xcb_get_image_reply(c->conn, iq, &e);

    if (e) {
        // av_log(s, AV_LOG_ERROR,
        //        "Cannot get the image data "
        //        "event_error: response_type:%u error_code:%u "
        //        "sequence:%u resource_id:%u minor_code:%u major_code:%u.\n",
        //        e->response_type, e->error_code,
        //        e->sequence, e->resource_id, e->minor_code, e->major_code);
        free(e);
        return AVERROR(EACCES);
    }

    if (!img) {
        return AVERROR(EAGAIN);
    }

    data = xcb_get_image_data(img);

    length = xcb_get_image_data_length(img);

    pkt->buf = av_buffer_create(data, length, xgrab_image_reply_free, img, 0);
    if (!pkt->buf) {
        free(img);
        return AVERROR(ENOMEM);
    }

    pkt->data = data;
    pkt->size = length;
    return 0;
}
