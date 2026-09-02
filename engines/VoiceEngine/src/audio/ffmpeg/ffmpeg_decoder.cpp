#include "core/internal.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <string>
#include <vector>

namespace voiceengine {

bool decode_ffmpeg_file(const wchar_t* path_utf16, Pcm16k* out, std::string* err) {
    if (!path_utf16 || !out) {
        if (err) {
            *err = "null ffmpeg path";
        }
        return false;
    }
    const std::string path = wide_to_utf8(path_utf16);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        if (err) {
            *err = "FFmpeg format/decode error: cannot open " + path;
        }
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        if (err) {
            *err = "FFmpeg format/decode error: no stream info in " + path;
        }
        return false;
    }
    const int stream_i = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream_i < 0) {
        avformat_close_input(&fmt);
        if (err) {
            *err = "FFmpeg format/decode error: no audio stream in " + path;
        }
        return false;
    }
    AVStream* st = fmt->streams[stream_i];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        if (err) {
            *err = "FFmpeg format/decode error: unsupported codec in " + path;
        }
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 ||
        avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        if (err) {
            *err = "FFmpeg format/decode error: cannot open decoder for " + path;
        }
        return false;
    }

    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, 1);
    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(&swr,
                            &out_ch,
                            AV_SAMPLE_FMT_FLT,
                            16000,
                            &ctx->ch_layout,
                            ctx->sample_fmt,
                            ctx->sample_rate,
                            0,
                            nullptr) < 0 ||
        swr_init(swr) < 0) {
        av_channel_layout_uninit(&out_ch);
        swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        if (err) {
            *err = "FFmpeg format/decode error: resampler init failed";
        }
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> pcm;
    int skipped_packets = 0;
    auto fail = [&](const char* msg) {
        if (err) {
            *err = msg;
        }
        av_packet_free(&pkt);
        av_frame_free(&frame);
        av_channel_layout_uninit(&out_ch);
        swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return false;
    };
    if (!pkt || !frame) {
        return fail("FFmpeg format/decode error: alloc failed");
    }

    auto convert_frame = [&](AVFrame* fr) -> bool {
        const int out_count = swr_get_out_samples(swr, fr->nb_samples);
        if (out_count < 0) {
            return false;
        }
        std::vector<float> tmp(static_cast<size_t>(out_count) + 16);
        uint8_t* dst[] = {reinterpret_cast<uint8_t*>(tmp.data())};
        const int got = swr_convert(swr, dst, out_count,
                                    const_cast<const uint8_t**>(fr->extended_data),
                                    fr->nb_samples);
        if (got < 0) {
            return false;
        }
        pcm.insert(pcm.end(), tmp.begin(), tmp.begin() + got);
        return true;
    };

    int read_err = 0;
    while ((read_err = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index != stream_i) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(ctx, pkt) < 0) {
            av_packet_unref(pkt);
            // Real-world MP3 recordings can contain an isolated damaged
            // frame (for example after an interrupted recorder write).
            // FFmpeg's command-line decoder skips such packets and keeps the
            // usable audio. Match that behavior instead of rejecting an
            // otherwise valid long recording.
            ++skipped_packets;
            avcodec_flush_buffers(ctx);
            continue;
        }
        av_packet_unref(pkt);
        while (true) {
            const int r = avcodec_receive_frame(ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                break;
            }
            if (r < 0) {
                ++skipped_packets;
                avcodec_flush_buffers(ctx);
                break;
            }
            if (!convert_frame(frame)) {
                return fail("FFmpeg format/decode error: resampling failed");
            }
        }
    }
    avcodec_send_packet(ctx, nullptr);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        if (!convert_frame(frame)) {
            return fail("FFmpeg format/decode error: flush failed");
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_channel_layout_uninit(&out_ch);
    swr_free(&swr);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    if (pcm.empty()) {
        if (err) {
            *err = "FFmpeg format/decode error: empty PCM from " + path;
        }
        return false;
    }
    (void)skipped_packets;
    out->samples = std::move(pcm);
    out->sample_rate = 16000;
    return true;
}

}  // namespace voiceengine
