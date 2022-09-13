// libav.hpp -*- c++ -*-

/*
 * MIT License
 *
 * Copyright (c) 2022 Walker Griggs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <iostream>
#include <string>
#include <chrono>
#include <memory>

#ifdef __cplusplus
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavutil/frame.h>
}
#endif

using FormatContextPtr = std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
using PacketPtr = std::unique_ptr<AVPacket, void(*)(AVPacket*)>;

/** A smart pointer wrapper for AVPacket
 *
 * Primarily used for packet allocation. Smart pointers will handle destruction
 * and garbage collection.
 */
class Packet : public PacketPtr {
public:
  using PacketPtr::PacketPtr;

  /** Allocates an empty packet.
   *
   * @return An allocated Packet on success, a null Packet on error.
   */
  static Packet alloc() {
    return Packet(av_packet_alloc(), [](AVPacket* packet) {
      av_packet_unref(packet);
      av_packet_free(&packet);
    });
  }
};

/** A smart pointer wrapper for AVFrame
 *
 * Primarily used for frame allocation. Smart pointers will handle destruction
 * and garbage collection.
 */
class Frame : public FramePtr {
public:
  using FramePtr::FramePtr;

  /** Allocates an empty frame.
   *
   * @return An allocated Frame on success, a null frame on error.
   */
  static Frame alloc() {
    return Frame(av_frame_alloc(),  [](AVFrame* frame) {
      av_frame_free(&frame);
    });
  }

  /** Allocates an empty frames given dimensions and format.
   *
   * Allocates an empty frame with buffer to populate the ~data~ and ~buf~
   * fields.
   *
   * @param w       frame width
   * @param h       frame height
   * @param pix_fmt picture format
   *
   * @return An allocated Frame on success, a null frame on error.
   */
  static Frame alloc(int w, int h, enum AVPixelFormat pix_fmt) {
    auto frame = alloc();
    frame->width = w;
    frame->height = h;
    frame->format = pix_fmt;

    if (av_frame_get_buffer(frame.get(), 0) < 0) {
      return Frame(NULL, [](AVFrame*) {});
    }

    return frame;
  }

  /** Allocates and scales a new frame, preserving data and extended data.
   *
   * Allocates and scales a new frame target dimensions and picture format,
   * preserving data and extended data. This is not an in-place operation, and
   * the new frame must be managed independently.
   *
   * @param w       scaled frame width
   * @param h       scaled frame height
   * @param pix_fmt scaled picture format
   *
   * @return An allocated Frame on success, a null frame on error.
   */
  Frame scale(int w, int h, enum AVPixelFormat pix_fmt) {
    auto frame = alloc(w,  h, pix_fmt);
    auto iframe = get();
    auto sframe = frame.get();

    struct SwsContext *sws_ctx;
    sws_ctx = sws_getContext(
                             iframe->width, iframe->height, (AVPixelFormat)iframe->format,
                             sframe->width, sframe->height, (AVPixelFormat)sframe->format,
                             SWS_BILINEAR | SWS_ACCURATE_RND,
                             NULL, NULL, NULL
                             );
    if (!sws_ctx) {
      return Frame(NULL, [](AVFrame*) {});
    }

    if (sws_init_context(sws_ctx, NULL, NULL) < 0) {
      return Frame(NULL, [](AVFrame*) {});
    }

    if (sws_scale(sws_ctx, iframe->data, iframe->linesize, 0, iframe->height,
                  sframe->extended_data, sframe->linesize) != sframe->height) {
      return Frame(NULL, [](AVFrame*) {});
    }

    sws_freeContext(sws_ctx);
    return frame;
  }
};

/** A smart pointer wrapper for AVCodecContext with methods for easy encoding.
 *
 * As an abstraction for encoding, `alloc_context` and `alloc_context_by_name`
 * will handle context allocation and garbage collection for you.
 *
 * Similarly, `send_frame` will do the heavy lifting of sending frames to the
 * encoder and running the given callback.
 */
class EncoderContext : public CodecContextPtr {
public:
  using CodecContextPtr::CodecContextPtr;

  /** Allocates a new EncoderContext given a codecs name
   *
   * @param codec name of code
   *
   * @return An allocated EncoderContext on success, a null context on error.
   */
  static EncoderContext alloc_context_by_name(std::string codec) {
    AVCodec* avc = (AVCodec*)avcodec_find_encoder_by_name(codec.c_str());
    if (!avc) {
      return EncoderContext(NULL, [](AVCodecContext*) {});
    }
    return alloc_context(avc);
  }

  /** Allocates a new EncoderContext given a codec.
   *
   * @param codec a pointer to the given AVCodec
   *
   * @return An allocated EncoderContext on success, a null context on error.
   */
  static EncoderContext alloc_context(AVCodec* codec) {
    AVCodecContext* avcc = avcodec_alloc_context3(codec);
    if (!avcc) {
      return EncoderContext(NULL, [](AVCodecContext*) {});
    }

    return EncoderContext(avcc, [](AVCodecContext* avcc) {
      avcodec_free_context(&avcc);
    });
  }

  /** Opens the codec.
   *
   * Opens a previously allocated codec. Open is called separately from alloc
   * because some codecs need flags set before opening.
   *
   * Always call this function before using encoding routines like
   * `avcodec_receive_packet`.
   *
   * @return Zero on success, a negative value on error.
   */
  int open() {
    return avcodec_open2(get(), NULL, NULL);
  }

  /** Pass a raw frame through the encoder, and run the given callback.
   *
   * @param frame the raw video or audio frame
   * @param fn    the callback function to run after successfully receiving a
   *              packet from the encoder
   *
   * @return Zero on success, negative AVERROR on error.
   */
  int send_frame(Frame& frame, std::function<int(Packet)> fn) {
    if (frame) {
      frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    int res = avcodec_send_frame(get(), frame.get());

    while (res >= 0) {
      auto packet = Packet::alloc();
      res = avcodec_receive_packet(get(), packet.get());
      if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
        break;
      } else if (res < 0) {
        return -1;
      }

      res = fn(std::move(packet));
      if (res < 0) {
        return res;
      }
    }

    return 0;
  }
};

/** A smart pointer wrapper for AVCodecContext with methods for easy decoding.
 *
 * As an abstraction for encoding, `open_context` will handle context allocation
 * and garbage collection for you.
 *
 * Similarly, `send_packet` will do the heavy lifting of sending frames to the
 * encoder and running the given callback.
 */
class DecoderContext : public CodecContextPtr {
public:
  using CodecContextPtr::CodecContextPtr;

  /** Allocates a new DecoderContext given codec parameters.
   *
   * Finds, allocates, and opens a registered decoder with the matching codec
   * ID specified in the given parameters.
   *
   * The codec's parameters are also filled with the given parameters struct.
   * Fields in the parameters which do not have matching fields in the codec are
   * ignored.
   *
   * @param codecpar the codec parameters, specifically the codec id.
   *
   * @return An allocated DecoderContext on success, a null context on error.
   */
  static DecoderContext open_context(AVCodecParameters *codecpar) {
    AVCodec* avc = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
    if (!avc) {
      return DecoderContext(NULL, [](AVCodecContext*) {});
    }

    AVCodecContext* avcc = avcodec_alloc_context3(avc);
    if (!avcc) {
      return DecoderContext(NULL, [](AVCodecContext*) {});
    }

    auto ctx = DecoderContext(avcc, [](AVCodecContext* avcc) {
      avcodec_free_context(&avcc);
    });

    if (avcodec_parameters_to_context(ctx.get(), codecpar) < 0) {
      return DecoderContext(NULL, [](AVCodecContext*) {});
    }

    if (avcodec_open2(ctx.get(), avc, NULL) < 0) {
      return DecoderContext(NULL, [](AVCodecContext*) {});
    }
    return ctx;
  }

  /** Pass raw packet data through the decoder, and run the given callback.
   *
   * @param packet the input packet. Ownership of this packet remains with the
   *               caller.
   * @param fn    the callback function to run after successfully receiving a
   *              frame from the decoder.
   *
   * @return Zero on success, negative AVERROR on error.
   */
  int send_packet(Packet& packet, std::function<int(Frame)> fn) {
    int res = avcodec_send_packet(get(), packet.get());

    while(res >= 0) {
      auto frame = Frame::alloc();
      res = avcodec_receive_frame(get(), frame.get());
      if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
        break;
      } else if (res < 0) {
        return res;
      }

      res = fn(std::move(frame));
      if (res < 0) {
        return res;
      }
    }

    return 0;
  }
};

/** A smart pointer wrapper for AVInputFormat
 *
 * FormatContext is used for both input and output
 */
class FormatContext : public FormatContextPtr {
public:
  using FormatContextPtr::FormatContextPtr;

  /** Allocate and open a new input FormatContext
   *
   * Allocate and open a new input FormatContext, and read header packets to get
   * stream information. The logical file position is not changed; examined
   * packets may be buffered for later processing.
   *
   * @param input_format A pointer to the populated AVInputFormat
   *
   * @return An allocated FormatContext on success, an empty context on error.
   */
  static FormatContext open_input_format(const AVInputFormat *input_format) {
    AVFormatContext* avfc = avformat_alloc_context();
    if (!avfc) {
      return FormatContext(nullptr, [](AVFormatContext*) {});
    }

    if (avformat_open_input(&avfc, NULL, input_format, NULL) < 0) {
      return FormatContext(NULL, [](AVFormatContext*) {});
    }

    auto ctx = FormatContext(avfc, [](AVFormatContext* avfc) {
      avformat_close_input(&avfc);
    });

    if (avformat_find_stream_info(ctx.get(), NULL) < 0) {
      return FormatContext(NULL, [](AVFormatContext*) {});
    }
    return ctx;
  }

  /** Allocate and open a new output FormatContext
   *
   * Allocate and open a new output FormatContext. The format context's target
   * resource can only be written to.
   *
   * @param url location of the target output resource
   *
   * @return An allocated FormatContext on success, an empty context on error.
   */
  static FormatContext open_output(const std::string url) {
    AVFormatContext* avfc = NULL;
    if (avformat_alloc_output_context2(&avfc, NULL, NULL, url.c_str()) < 0) {
      return FormatContext(nullptr, [](AVFormatContext*) {});
    }

    auto ctx = FormatContext(avfc, [](AVFormatContext* avfc) {
      avio_close(avfc->pb);
      avformat_free_context(avfc);
    });

    if (int ret = avio_open(&ctx->pb, url.c_str(), AVIO_FLAG_WRITE); ret < 0) {
      return FormatContext(nullptr, [](AVFormatContext*) {});
    }
    return ctx;
  }

  /** Create a new stream and set the appropriate header flags
   *
   * Some formats may require you set flags before you open the codec, and copy
   * parameters after. If so, this function will not work for you.
   *
   * @param encoder the allocated and configured EncodeContext.
   *
   * @return The stream's index on success, an negative AVERROR on error
   */
  int create_stream(EncoderContext& encoder) {
    auto stream = avformat_new_stream(get(), NULL);
    if (!stream) {
      return -1;
    }

    stream->time_base = encoder->time_base;
    if (int res = avcodec_parameters_from_context(stream->codecpar, encoder.get()); res < 0) {
      return res;
    }

    av_dump_format(get(), 0, NULL, 1);

    if (get()->oformat->flags & AVFMT_GLOBALHEADER) {
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    return stream->index;
  }

  /** Find the best stream in the format context.
   *
   * The best stream is determed according to various heuristics as the most
   * likely to be what the user expects.
   *
   * @param wanted_stream_nb the user requested stream number, -1 for automatic
   *                         selection
   * @param type             stream type: audio, video, subtitles, etc.
   *
   * @return an AVStream on success, null on error.
   */
  AVStream* find_best_stream(AVMediaType type, int wanted_stream_nb) {
    int idx = find_best_stream_idx(type, wanted_stream_nb);
    if (idx < 0) {
      return NULL;
    }

    return get()->streams[idx];
  }

  /** Find the best stream in the format context.
   *
   * The best stream is determed according to various heuristics as the most
   * likely to be what the user expects.
   *
   * @param wanted_stream_nb the user requested stream number, -1 for automatic
   *                         selection
   * @param type             stream type: audio, video, subtitles, etc.
   *
   * @return a non-negative stream index on success, a negative AVERROR on error
   */
  int find_best_stream_idx(AVMediaType type, int wanted_stream_nb) {
    return av_find_best_stream(get(), type, wanted_stream_nb, -1, NULL, 0);
  }
};
