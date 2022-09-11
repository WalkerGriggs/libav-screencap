#include <iostream>
#include <chrono>

#include "libav.hpp"

int frames = 0;

int encode_video(DecoderContext& input_avcc, EncoderContext& output_avcc, FormatContext& output_avfc, Frame& frame)
{
  if(frame) {
    frame->pict_type = AV_PICTURE_TYPE_NONE;
  }

  auto scale_frame = frame.scale(frame->width, frame->height, AV_PIX_FMT_YUV420P);
  scale_frame->pts = frames++ * output_avcc->time_base.num;
  scale_frame->pkt_dts = scale_frame->pts;

  Packet output_packet = Packet::alloc();
  if (!output_packet) {
    return -1;
  }

  int res = avcodec_send_frame(output_avcc.get(), scale_frame.get());

  while (res >= 0) {
    res = avcodec_receive_packet(output_avcc.get(), output_packet.get());
    if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
      break;
    } else if (res < 0) {
      return -1;
    }

    output_packet->stream_index = 0; // TODO

    if(res = av_write_frame(output_avfc.get(), output_packet.get()); res < 0) {
      return res;
    }
  }

  return 0;
}

int transcode_video(DecoderContext& input_avcc, EncoderContext& output_avcc, FormatContext& output_avfc, Packet& packet, Frame& frame)
{
  int res = avcodec_send_packet(input_avcc.get(), packet.get());
  if(res > 0) {
    return res;
  }

  while(res >= 0) {
    res = avcodec_receive_frame(input_avcc.get(), frame.get());
    if (res == AVERROR(EAGAIN) || res == AVERROR_EOF) {
      break;
    } else if (res < 0) {
      return res;
    }

    if(encode_video(input_avcc, output_avcc, output_avfc, frame)) {
      return -1;
    }

    av_frame_unref(frame.get());
  }

  return 0;
}

int setup_encoder_context(DecoderContext& decoder, EncoderContext& encoder, AVRational timebase)
{
  av_opt_set(encoder->priv_data, "preset", "fast", 0);

  encoder->pix_fmt             = AV_PIX_FMT_YUV420P;
  encoder->height              = decoder->height;
  encoder->width               = decoder->width;
  encoder->sample_aspect_ratio = decoder->sample_aspect_ratio;
  encoder->bit_rate            = 2 * 1000 * 1000;
  encoder->rc_buffer_size      = 4 * 1000 * 1000;
  encoder->rc_max_rate         = 2 * 1000 * 1000;
  encoder->rc_min_rate         = 2.5 * 1000 * 1000;
  encoder->time_base           = timebase;

  return 0;
}

int main(int argc, char **argv) {
  avdevice_register_all();

  const AVInputFormat *input_format = av_find_input_format("x11grab");
  if (!input_format) {
    throw std::runtime_error("Failed to find input format");
  }

  Frame frame = Frame::alloc();
  if (!frame) {
    throw std::runtime_error("Failed to allocate a decoder frame");
  }

  Packet packet = Packet::alloc();
  if (!packet) {
    throw std::runtime_error("Failed to allocate a decoder packet");
  }


  /*
   * Setup decoder
   */

  auto input_avfc = FormatContext::open_input_format(input_format);
  if(!input_avfc.get()) {
    throw std::runtime_error("Failed to open the input format");
  }

  auto input_avs = input_avfc.find_best_stream(AVMEDIA_TYPE_VIDEO, -1);

  auto input_avcc = DecoderContext::open_context(input_avs->codecpar);
  if (!input_avcc.get()) {
    throw std::runtime_error("Failed to allocate the input codec context");
  }

  /*
   * Setup encoder
   */

  auto output_avfc = FormatContext::open_output("out.mp4");
  if(!output_avfc.get()) {
    throw std::runtime_error("Failed to open the output format");
  }

  auto output_avcc = EncoderContext::alloc_context_by_name("libx264");
  if(!output_avcc.get()) {
    throw std::runtime_error("Failed to allocate the output codec context");
  }

  auto framerate = av_guess_frame_rate(input_avfc.get(), input_avs, NULL);
  auto timebase = av_inv_q(framerate);
  setup_encoder_context(input_avcc, output_avcc, timebase);

  if (output_avcc.open() < 0) {
    throw std::runtime_error("Failed to open the output codec context");
  }

  if (output_avfc.create_stream(output_avcc) < 0) {
    throw std::runtime_error("Failed to create new output stream");
  }

  if(avformat_write_header(output_avfc.get(), NULL) < 0) {
    throw std::runtime_error("Failed to write output headers");
  }

  /*
   * Run it!
   */

  using ClockType = std::chrono::system_clock;
  auto time_start = ClockType::now();

  while(av_read_frame(input_avfc.get(), packet.get()) >= 0) {
    if (std::chrono::duration_cast<std::chrono::seconds>(ClockType::now() - time_start).count() >= 7) {
      break;
    }
    transcode_video(input_avcc, output_avcc, output_avfc, packet, frame);
  }

  av_write_trailer(output_avfc.get());
  return 0;
}
