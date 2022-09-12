// main.cpp -*- c++ -*-

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

/**
 * @file main.cpp
 *
 * @brief Transcodes frames from X11 server to X264, written to an MP4
 *        container.
 *
 * @author Walker Griggs (walker@walkergriggs.com)
 */

#include <iostream>
#include <chrono>
#include <functional>
#include <signal.h>

#include "libav.hpp"

volatile sig_atomic_t stop;

void signal_handler(int n)
{
  std::cout << "Gracefully stopping" << std::endl;
  stop = 1;
}

int main(int argc, char **argv) {
  avdevice_register_all();

  const AVInputFormat *input_format = av_find_input_format("x11grab");
  if (!input_format) {
    throw std::runtime_error("Failed to find input format");
  }

  Packet packet = Packet::alloc();
  if (!packet) {
    throw std::runtime_error("Failed to allocate a decoder packet");
  }

  int frames = 0;


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

  auto framerate = av_guess_frame_rate(input_avfc.get(), input_avs, NULL);
  auto timebase = av_inv_q(framerate);

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

  av_opt_set(output_avcc->priv_data, "preset", "fast", 0);
  output_avcc->pix_fmt             = AV_PIX_FMT_YUV420P;
  output_avcc->height              = input_avcc->height;
  output_avcc->width               = input_avcc->width;
  output_avcc->sample_aspect_ratio = input_avcc->sample_aspect_ratio;
  output_avcc->bit_rate            = 2 * 1000 * 1000;
  output_avcc->rc_buffer_size      = 4 * 1000 * 1000;
  output_avcc->rc_max_rate         = 2 * 1000 * 1000;
  output_avcc->rc_min_rate         = 2.5 * 1000 * 1000;
  output_avcc->time_base           = timebase;

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
   * Define codec callbacks
   */

  std::function<int(Packet packet)> encode_callback = [&](Packet packet) {
    return av_write_frame(output_avfc.get(), packet.get());
  };

  std::function<int(Frame frame)> decode_callback = [&](Frame frame) {
    auto scale_frame = frame.scale(frame->width, frame->height, AV_PIX_FMT_YUV420P);
    scale_frame->pts = frames++ * output_avcc->time_base.num;
    scale_frame->pkt_dts = scale_frame->pts;

    return output_avcc.send_frame(scale_frame, encode_callback);
  };

  /*
   * Run it!
   */

  signal(SIGINT, &signal_handler);

  while(!stop) {
    if (av_read_frame(input_avfc.get(), packet.get()) < 0) {
      break;
    }
    input_avcc.send_packet(packet, decode_callback);
  }

  av_write_trailer(output_avfc.get());
  return 0;
}
