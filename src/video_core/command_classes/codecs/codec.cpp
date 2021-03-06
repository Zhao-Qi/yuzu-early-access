// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <fstream>
#include "codec.h"
#include "common/assert.h"
#include "video_core/command_classes/codecs/h264.h"
#include "video_core/command_classes/codecs/vp9.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace Tegra {

Codec::Codec(GPU& gpu)
    : gpu(gpu), h264_decoder(std::make_unique<Decoder::H264>(gpu)),
      vp9_decoder(std::make_unique<Decoder::VP9>(gpu)) {}

Codec::~Codec() {
    if (initialized) {
        // Free libav memory
        avcodec_send_packet(av_codec_ctx, nullptr);
        avcodec_receive_frame(av_codec_ctx, av_frame);
        LOG_DEBUG(Service_NVDRV, "Flushed avcontext");

        avcodec_flush_buffers(av_codec_ctx);
        av_frame_unref(av_frame);
        av_free(av_frame);
        avcodec_close(av_codec_ctx);
    }

    // destroy intermediary data.
    h264_decoder.reset();
    vp9_decoder.reset();
}

void Codec::SetTargetCodec(NvdecCommon::VideoCodec codec) {
    if (initialized) {
        if (current_codec != codec) {
            codec_swap = true;
            LOG_INFO(Service_NVDRV, "Codec swap from {} to {}", static_cast<u32>(current_codec),
                     static_cast<u32>(codec));
        }
    } else {
        LOG_INFO(Service_NVDRV, "Codec initialized to {}", static_cast<u32>(codec));
    }
    current_codec = codec;
}

void Codec::StateWrite(u32 offset, u64 arguments) {
    u8* state_offset = reinterpret_cast<u8*>(&state) + offset * sizeof(u64);
    std::memcpy(state_offset, &arguments, sizeof(u64));
}

void Codec::Decode() {
    bool is_first_frame = false;

    if (initialized && codec_swap) {
        // Free allocated memory in preparation for new codec.
        av_frame_unref(av_frame);
        av_free(av_frame);
        avcodec_close(av_codec_ctx);
        avcodec_free_context(&av_codec_ctx);
        initialized = false;
        codec_swap = false;
    }

    if (!initialized) {
        if (current_codec == NvdecCommon::VideoCodec::H264) {
            av_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        } else if (current_codec == NvdecCommon::VideoCodec::Vp9) {
            av_codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
        }

        av_codec_ctx = avcodec_alloc_context3(av_codec);
        av_frame = av_frame_alloc();
        av_opt_set(av_codec_ctx->priv_data, "tune", "zerolatency", 0);

        // TODO(ameerj): libavcodec gpu hw acceleration

        if (avcodec_open2(av_codec_ctx, av_codec, nullptr) < 0) {
            LOG_ERROR(Service_NVDRV, "avcodec_open2() Failed.");
        }
        initialized = true;
        is_first_frame = true;
    }
    bool vp9_hidden_frame = false;

    AVPacket packet{};
    av_init_packet(&packet);
    auto frame_data = std::vector<u8>();
    auto test_frame_data = std::vector<u8>();

    if (current_codec == NvdecCommon::VideoCodec::H264) {
        frame_data = h264_decoder->ComposeFrameHeader(state, is_first_frame);
    } else if (current_codec == NvdecCommon::VideoCodec::Vp9) {
        frame_data = vp9_decoder->ComposeFrameHeader(state);
        vp9_hidden_frame = vp9_decoder->WasFrameHidden();
    } else {
        LOG_ERROR(Service_NVDRV, "Unknown video codec {}", static_cast<u32>(current_codec));
    }

    packet.data = frame_data.data();
    packet.size = static_cast<int>(frame_data.size());

    avcodec_send_packet(av_codec_ctx, &packet);

    if (!vp9_hidden_frame) {
        // Only receive/store visible frames
        avcodec_receive_frame(av_codec_ctx, av_frame);
    }
}

AVFrame* Codec::GetCurrentFrame() {
    return av_frame;
}

const AVFrame* Codec::GetCurrentFrame() const {
    return av_frame;
}

NvdecCommon::VideoCodec Codec::GetCurrentCodec() const {
    return current_codec;
}

} // namespace Tegra
