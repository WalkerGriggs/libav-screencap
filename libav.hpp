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

class Packet : public PacketPtr
{
public:
  using PacketPtr::PacketPtr;

  static Packet alloc()
  {
    return Packet(av_packet_alloc(), [](AVPacket* packet) {
      av_packet_unref(packet);
      av_packet_free(&packet);
    });
  }
};

class Frame : public FramePtr {
public:
  using FramePtr::FramePtr;

  static Frame alloc()
  {
    return Frame(av_frame_alloc(),  [](AVFrame* frame) {
      av_frame_free(&frame);
    });
  }

  static Frame alloc(int w, int h, enum AVPixelFormat pix_fmt)
  {
    auto frame = alloc();
    frame->width = w;
    frame->height = h;
    frame->format = pix_fmt;

    if(av_frame_get_buffer(frame.get(), 0) < 0){
      return Frame(NULL, [](AVFrame*) {});
    }

    return frame;
  }

  Frame scale(int w, int h, enum AVPixelFormat pix_fmt)
  {
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

    if (sws_init_context(sws_ctx, NULL, NULL) < 0) {
      return Frame(NULL, [](AVFrame*) {});
    }

    if(sws_scale(sws_ctx, iframe->data, iframe->linesize, 0, iframe->height,
                 sframe->extended_data, sframe->linesize) != sframe->height) {
      return Frame(NULL, [](AVFrame*) {});
    }

    return frame;
  }
};

class EncoderContext : public CodecContextPtr {
public:
  using CodecContextPtr::CodecContextPtr;

  static EncoderContext alloc_context_by_name(std::string codec)
  {
    AVCodec* avc = (AVCodec*)avcodec_find_encoder_by_name(codec.c_str());
    if (!avc) {
      return EncoderContext(NULL, [](AVCodecContext*) {});
    }
    return alloc_context(avc);
  }

  static EncoderContext alloc_context(AVCodec* codec)
  {
    AVCodecContext* avcc = avcodec_alloc_context3(codec);
    if (!avcc) {
      return EncoderContext(NULL, [](AVCodecContext*) {});
    }

    return EncoderContext(avcc, [](AVCodecContext* avcc) {
      avcodec_free_context(&avcc);
    });
  }

  int open()
  {
    return avcodec_open2(get(), NULL, NULL);
  }

  int send_frame(Frame& frame, std::function<int(Packet)> fn)
  {
    if(frame) {
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

      packet->stream_index = 0; // TODO

      res = fn(std::move(packet));
      if (res < 0) {
        return res;
      }
    }

    return 0;
  }
};

class DecoderContext : public CodecContextPtr {
public:
  using CodecContextPtr::CodecContextPtr;

  static DecoderContext open_context(AVCodecParameters *codecpar)
  {
    AVCodec* avc = (AVCodec*)avcodec_find_decoder(codecpar->codec_id);
    if(!avc) {
      return DecoderContext(NULL, [](AVCodecContext*) {});
    }

    AVCodecContext* avcc = avcodec_alloc_context3(avc);
    if(!avcc) {
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

  int send_packet(Packet& packet, std::function<int(Frame)> fn)
  {
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

class FormatContext : public FormatContextPtr {
public:
  using FormatContextPtr::FormatContextPtr;

  static FormatContext open_input_format(const AVInputFormat *input_format)
  {
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

  static FormatContext open_output(const std::string url)
  {
    AVFormatContext* avfc = NULL;
    if(avformat_alloc_output_context2(&avfc, NULL, NULL, url.c_str()) < 0) {
      return FormatContext(nullptr, [](AVFormatContext*) {});
    }

    auto ctx = FormatContext(avfc, [](AVFormatContext* avfc) {
      avio_close(avfc->pb);
      avformat_free_context(avfc);
    });

    if(int ret = avio_open(&ctx->pb, url.c_str(), AVIO_FLAG_WRITE); ret < 0) {
      return FormatContext(nullptr, [](AVFormatContext*) {});
    }
    return ctx;
  }

  int create_stream(EncoderContext& encoder)
  {
    auto stream = avformat_new_stream(get(), NULL);
    if (!stream) {
      return -1;
    }

    stream->time_base = encoder->time_base;
    if (int res = avcodec_parameters_from_context(stream->codecpar, encoder.get()); res < 0) {
      return res;
    }

    av_dump_format(get(), 0, NULL, 1);

    if(get()->oformat->flags & AVFMT_GLOBALHEADER) {
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    return stream->index;
  }

  AVStream* find_best_stream(AVMediaType type, int wanted_stream_nb)
  {
    return get()->streams[find_best_stream_idx(type, wanted_stream_nb)];
  }

  int find_best_stream_idx(AVMediaType type, int wanted_stream_nb)
  {
    return av_find_best_stream(get(), type, wanted_stream_nb, -1, NULL, 0);
  }
};
