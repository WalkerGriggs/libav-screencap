#include <iostream>
#include <string>
#include <chrono>

#ifdef __cplusplus
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>
#include <libavutil/timestamp.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}
#endif

typedef struct Context {
  AVFormatContext *avfc;
  AVCodec *avc;
  AVStream *avs;
  AVCodecContext *avcc;
  int frames;
} Context;

int encode_video(Context *decoder, Context *encoder, AVFrame *input_frame) {
  if(input_frame) {
    input_frame->pict_type = AV_PICTURE_TYPE_NONE;
  }

  AVPacket *output_packet = av_packet_alloc();
  if (!output_packet) {
    std::cout << "Failed to allocate a new packet" << std::endl;
    return -1;
  }

  AVFrame *scale_frame = av_frame_alloc();
  scale_frame->width = 2560;
  scale_frame->height = 1440;
  scale_frame->format = AV_PIX_FMT_YUV420P;

  if(av_frame_get_buffer(scale_frame, 1) < 0){
    std::cout << "Failed to get frame buffer" << std::endl;
    return -1;
  }
  
  struct SwsContext *sws_ctx;
  sws_ctx = sws_getContext(
                           input_frame->width, input_frame->height, (AVPixelFormat)input_frame->format,
                           scale_frame->width, scale_frame->height, (AVPixelFormat)scale_frame->format,
                           SWS_BILINEAR | SWS_ACCURATE_RND,
                           NULL, NULL, NULL
                           );

  if (sws_init_context(sws_ctx, NULL, NULL) < 0) {
    std::cout << "Failed to initialize context" << std::endl;
    return -1;
  }

  if(sws_scale(sws_ctx, input_frame->data, input_frame->linesize, 0, input_frame->height, scale_frame->extended_data, scale_frame->linesize) != scale_frame->height) {
    std::cout << "Failed to scale frame" << std::endl;
    return -1;
  }

  scale_frame->pkt_dts = input_frame->pkt_dts;
  scale_frame->pts = input_frame->pts;

  scale_frame->pts = scale_frame->pkt_dts = encoder->frames++ * 1001;
  
  int res = avcodec_send_frame(encoder->avcc, scale_frame);

  while (res >= 0) {
    res = avcodec_receive_packet(encoder->avcc, output_packet);
    if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
      break;
    } else if (res < 0) {
      std::cout << "Failed to receive frame from encoder" << std::endl;
      return -1;
    }

    // Set the packet stream
    output_packet->stream_index = 0;
    
    // Write interleaved frames, ordered by dts, to the output file.
    res = av_write_frame(encoder->avfc, output_packet);
    if(res != 0) {
      std::cout << "Failed to write interleaved frames" << std::endl;
      return res;
    }
  }
  if (scale_frame != NULL) {
    av_frame_free(&scale_frame);
    scale_frame = NULL;
  }

  av_packet_unref(output_packet);
  av_packet_free(&output_packet);
  return 0;
}

int transcode_video(Context *decoder, Context *encoder, AVPacket *input_packet, AVFrame *input_frame) {
  int res = avcodec_send_packet(decoder->avcc, input_packet);
  if(res > 0) {
    std::cout << "Error sending packet to the decoder" << std::endl;
    return res;
  }

  while(res >= 0) {
    res = avcodec_receive_frame(decoder->avcc, input_frame);
    if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
      break;
    } else if (res < 0) {
      std::cout << "Failed to receive frame from decoder" << std::endl;
      return res;
    }

    if(encode_video(decoder, encoder, input_frame)) {
      return -1;
    }
    av_frame_unref(input_frame);
  }

  return 0;
}

int main(int argc, char **argv) {
  avdevice_register_all();

  Context *decoder = (Context*) calloc(1, sizeof(Context));
  Context *encoder = (Context*) calloc(1, sizeof(Context));
  encoder->frames = 0;

  AVDictionary* opts = NULL;
  int64_t ret;
  int stream_index;

  const AVInputFormat *input_format = av_find_input_format("x11grab");
  if (!input_format) {
    AVDictionary* opts = NULL;
    std::cout << "Failed to grab the format" << std::endl;
    return 1;
  }

  /*
   * Video Decoder Prep
   */

  decoder->avfc = avformat_alloc_context();
  if (!decoder->avfc) {
    std::cout << "Could not allocate the output context" << std::endl;
    return 1;
  }

  if((ret = avformat_open_input(&decoder->avfc, NULL, input_format, NULL)) < 0) {
    std::cout << "Could not open input format" << std::endl;
    return 1;
  }

  if ((ret = avformat_find_stream_info(decoder->avfc, NULL)) < 0) {
    std::cout << "Could not find stream info" << std::endl;
    return 1;
  }

  for (uint i = 0; i < decoder->avfc->nb_streams; i++) {
    if(decoder->avfc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      decoder->avs = decoder->avfc->streams[i];
      stream_index = i;
    }
  }

  decoder->avc = (AVCodec*)avcodec_find_decoder(decoder->avs->codecpar->codec_id);
  if(!decoder->avc) {
    std::cout << "Failed to find the decoder" << std::endl;
    return 1;
  }

  decoder->avcc = avcodec_alloc_context3(decoder->avc);
  if(!decoder->avcc) {
    std::cout << "Failed to allocate the codec context" << std::endl;
    return 1;
  }
  
  if (avcodec_parameters_to_context(decoder->avcc, decoder->avs->codecpar) < 0) {
    std::cout << "Failed to fill the codec context" << std::endl;
    return 1;
  }
  
  if (avcodec_open2(decoder->avcc, decoder->avc, NULL) < 0) {
    std::cout << "Failed to open the codec" << std::endl;
    return 1;
  }
  
  /*
   * Video Encoder Prep
   */

  AVRational input_framerate = av_guess_frame_rate(decoder->avfc, decoder->avs, NULL);
  std::cout << input_framerate.num << "/" << input_framerate.den << std::endl;
  
  avformat_alloc_output_context2(&encoder->avfc, NULL, NULL, "out.mp4");
  if (!encoder->avfc) {
    std::cout << "Failed to allocate output context" << std::endl;
    return 1;
  }

  encoder->avs = avformat_new_stream(encoder->avfc, NULL);
  if (!encoder->avs) {
    std::cout << "Failed to create new output stream" << std::endl;
    return 1;
  }

  encoder->avc = (AVCodec*)avcodec_find_encoder_by_name("libx264");
  if (!encoder->avc) {
    std::cout << "Failed to find encoder" << std::endl;
    return 1;
  }

  encoder->avcc = avcodec_alloc_context3(encoder->avc);
  if (!encoder->avcc) {
    std::cout << "Failed to allocate for the codec context" << std::endl;
    return 1;
  }

  av_opt_set(encoder->avcc->priv_data, "preset", "fast", 0);

  // See `ffmpeg -h encoder=libx264 | grep -i "supported pixel formats"` for the
  // full list of supported formats.
  encoder->avcc->pix_fmt             = AV_PIX_FMT_YUV420P;
  encoder->avcc->height              = decoder->avcc->height;
  encoder->avcc->width               = decoder->avcc->width;
  encoder->avcc->sample_aspect_ratio = decoder->avcc->sample_aspect_ratio;
  encoder->avcc->bit_rate            = 2 * 1000 * 1000;
  encoder->avcc->rc_buffer_size      = 4 * 1000 * 1000;
  encoder->avcc->rc_max_rate         = 2 * 1000 * 1000;
  encoder->avcc->rc_min_rate         = 2.5 * 1000 * 1000;
  encoder->avcc->time_base           = av_inv_q(input_framerate);
  encoder->avs->time_base            = encoder->avcc->time_base;

  if (avcodec_open2(encoder->avcc, encoder->avc, NULL) < 0) {
    std::cout << "Failed to open the output codec" << std::endl;
    return 1;
  }

  avcodec_parameters_from_context(encoder->avs->codecpar, encoder->avcc);
  av_dump_format(encoder->avfc, 0, NULL, 1);

  if(encoder->avfc->oformat->flags & AVFMT_GLOBALHEADER) {
    encoder->avfc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  
  /*
   * Main
   */

  ret = avio_open(&encoder->avfc->pb, "out.mp4", AVIO_FLAG_WRITE);
  if (ret < 0) {
    std::cout << "Failed to copy video output params" << std::endl;
    return 1;
  }
  
  // Write the file header to the output file.
  if(avformat_write_header(encoder->avfc, &opts) < 0) {
    std::cout << "Failed to write the output header" << std::endl;
    return 1;
  }

  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    std::cout << "Failed to allocate a new frame" << std::endl;
    return 1;
  }

  AVPacket *packet = av_packet_alloc();
  if (!packet) {
    std::cout << "Failed to allocate a new packet" << std::endl;
    return 1;
  }

  using ClockType = std::chrono::system_clock;
  auto time_start = ClockType::now();

  while(av_read_frame(decoder->avfc, packet) >= 0) {
    if (std::chrono::duration_cast<std::chrono::seconds>(ClockType::now() - time_start).count() >= 5) {
      break;
    }
    
    if(transcode_video(decoder, encoder, packet, frame)) {
      return -1;
    }
  }
  
  // write the trailer
  av_write_trailer(encoder->avfc);

  if(frame != NULL) {
    av_frame_free(&frame);
    frame = NULL;
  }

  if(packet != NULL) {
    av_packet_free(&packet);
    packet = NULL;
  }

  avformat_close_input(&decoder->avfc);

  avformat_free_context(decoder->avfc);
  decoder->avfc = NULL;

  avformat_free_context(encoder->avfc);
  encoder->avfc = NULL;

  avcodec_free_context(&decoder->avcc);
  decoder->avcc = NULL;

  return 0;
}
