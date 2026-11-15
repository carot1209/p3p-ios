#ifdef P3P_HAVE_FFMPEG
// FFmpeg-backed MpegDecoder (see ffmpeg_decoder.hpp).
#include "ffmpeg_decoder.hpp"

#include "psprecomp/guest_memory.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

namespace p3pios {
namespace {

std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

class FfmpegDecoder final : public p3p3ds::hle::MpegDecoder {
public:
    FfmpegDecoder(std::function<void(std::uint32_t, std::uint32_t)> on_picture, std::shared_ptr<MovieStats> stats)
        : on_picture_(std::move(on_picture)), stats_(std::move(stats)) {
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
    }
    ~FfmpegDecoder() override {
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
    }

    bool decode_video(const p3p3ds::hle::MpegAccessUnit &au, psprecomp::GuestMemory &memory, std::uint32_t dest,
                      std::uint32_t stride, std::uint32_t format) override {
        ++stats_->video_aus;
        // A new movie (its timestamps start over) or a decoder that stopped
        // giving pictures gets a fresh decoder: one that is stuck would show
        // its last picture for the rest of the session.
        const bool restarted = au.pts >= 0 && last_pts_ >= 0 && au.pts + 90000 < last_pts_;
        if (au.pts >= 0) last_pts_ = au.pts;
        if (video_ != nullptr && (restarted || barren_ >= 45u)) {
            avcodec_free_context(&video_);
            ++stats_->video_resets;
            barren_ = 0;
        }
        if (video_ == nullptr && !open_video()) return false;
        if (au.data.empty()) ++stats_->empty_aus;
        bool got = false;
        if (!au.data.empty() && send(video_, au.data.data(), au.data.size())) {
            while (avcodec_receive_frame(video_, frame_) == 0) {
                convert(*frame_);
                got = true;
            }
        } else if (!au.data.empty()) {
            ++stats_->video_errors;
        }
        barren_ = got ? 0u : barren_ + 1u;
        if (rgba_.empty()) return false;
        // A picture each call: the newest one (an AU that gave none repeats it).
        write(memory, dest, stride, format);
        if (got) ++stats_->pictures;
        return true;
    }

    void decode_audio(const p3p3ds::hle::MpegAccessUnit &au, psprecomp::GuestMemory &memory, std::uint32_t dest) override {
        ++stats_->audio_aus;
        std::uint8_t *out = memory.raw_pointer(dest, 2048u * 4u);
        if (out == nullptr) return;
        std::memset(out, 0, 2048u * 4u);
        // PSMF ATRAC3plus AU: 8-byte header (0F D0 ..), then one frame of
        // ((b2 & 3) << 8 | b3) * 8 + 8 bytes (upstream mpeg.hpp).
        if (au.data.size() < 8u || au.data[0] != 0x0Fu || au.data[1] != 0xD0u) { ++stats_->audio_errors; return; }
        const std::size_t frame = (static_cast<std::size_t>(au.data[2] & 3u) << 8 | au.data[3]) * 8u + 8u;
        if (au.data.size() < 8u + frame) { ++stats_->audio_errors; return; }
        if (audio_ == nullptr || audio_block_ != frame) {
            avcodec_free_context(&audio_);
            if (!open_audio(static_cast<int>(frame))) { ++stats_->audio_errors; return; }
        }
        if (!send(audio_, au.data.data() + 8, frame)) { ++stats_->audio_errors; return; }
        std::size_t written = 0;
        auto *pcm = reinterpret_cast<std::int16_t *>(out);
        while (avcodec_receive_frame(audio_, frame_) == 0) {
            const int channels = frame_->ch_layout.nb_channels;
            const int n = std::min<int>(frame_->nb_samples, 2048 - static_cast<int>(written));
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < 2; ++c) {
                    const int src = std::min(c, channels - 1);
                    float v = 0.0f;
                    if (frame_->format == AV_SAMPLE_FMT_FLTP) v = reinterpret_cast<const float *>(frame_->extended_data[src])[i];
                    else if (frame_->format == AV_SAMPLE_FMT_FLT) v = reinterpret_cast<const float *>(frame_->data[0])[i * channels + src];
                    const float s = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
                    pcm[(written + static_cast<std::size_t>(i)) * 2u + static_cast<std::size_t>(c)] = static_cast<std::int16_t>(s);
                }
            written += static_cast<std::size_t>(n);
        }
    }

private:
    bool open_video() {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) return false;
        video_ = avcodec_alloc_context3(codec);
        if (video_ == nullptr) return false;
        video_->thread_count = 1;
        video_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        video_->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        if (avcodec_open2(video_, codec, nullptr) < 0) { avcodec_free_context(&video_); return false; }
        return true;
    }

    bool open_audio(int block) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        if (codec == nullptr) return false;
        audio_ = avcodec_alloc_context3(codec);
        if (audio_ == nullptr) return false;
        av_channel_layout_default(&audio_->ch_layout, 2);
        audio_->sample_rate = 44100;
        audio_->block_align = block;
        if (avcodec_open2(audio_, codec, nullptr) < 0) { avcodec_free_context(&audio_); return false; }
        audio_block_ = static_cast<std::size_t>(block);
        return true;
    }

    bool send(AVCodecContext *ctx, const std::uint8_t *data, std::size_t size) {
        // FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end.
        scratch_.assign(data, data + size);
        scratch_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        packet_->data = scratch_.data();
        packet_->size = static_cast<int>(size);
        const int rc = avcodec_send_packet(ctx, packet_);
        if (rc != 0 && rc != AVERROR(EAGAIN) && ctx == video_) stats_->last_video_error = rc;
        packet_->data = nullptr;
        packet_->size = 0;
        return rc == 0 || rc == AVERROR(EAGAIN);
    }

    // YUV 4:2:0 (BT.601, limited range) -> RGBA, at most 512 x 272.
    void convert(const AVFrame &f) {
        if (f.format != AV_PIX_FMT_YUV420P && f.format != AV_PIX_FMT_YUVJ420P) return;
        width_ = static_cast<std::uint32_t>(std::min(f.width, 512));
        height_ = static_cast<std::uint32_t>(std::min(f.height, 272));
        stats_->width = static_cast<std::uint32_t>(f.width);
        stats_->height = static_cast<std::uint32_t>(f.height);
        rgba_.resize(static_cast<std::size_t>(width_) * height_);
        const bool full = f.format == AV_PIX_FMT_YUVJ420P || f.color_range == AVCOL_RANGE_JPEG;
        for (std::uint32_t y = 0; y < height_; ++y) {
            const std::uint8_t *yr = f.data[0] + static_cast<std::ptrdiff_t>(y) * f.linesize[0];
            const std::uint8_t *ur = f.data[1] + static_cast<std::ptrdiff_t>(y / 2u) * f.linesize[1];
            const std::uint8_t *vr = f.data[2] + static_cast<std::ptrdiff_t>(y / 2u) * f.linesize[2];
            std::uint32_t *out = &rgba_[static_cast<std::size_t>(y) * width_];
            for (std::uint32_t x = 0; x < width_; ++x) {
                const int u = ur[x / 2u] - 128, v = vr[x / 2u] - 128;
                int r, g, b;
                if (full) {
                    const int c = yr[x] << 8;
                    r = (c + 359 * v) >> 8; g = (c - 88 * u - 183 * v) >> 8; b = (c + 454 * u) >> 8;
                } else {
                    const int c = 298 * (yr[x] - 16);
                    r = (c + 409 * v + 128) >> 8; g = (c - 100 * u - 208 * v + 128) >> 8; b = (c + 516 * u + 128) >> 8;
                }
                out[x] = clamp8(r) | (static_cast<std::uint32_t>(clamp8(g)) << 8) | (static_cast<std::uint32_t>(clamp8(b)) << 16) | 0xFF000000u;
            }
        }
    }

    void write(psprecomp::GuestMemory &memory, std::uint32_t dest, std::uint32_t stride, std::uint32_t format) {
        const std::uint32_t bpp = format == 3u ? 4u : 2u;
        if (stride == 0u) stride = 512u;
        const std::uint32_t w = std::min(width_, stride);
        const std::uint32_t bytes = stride * height_ * bpp;
        std::uint8_t *base = memory.raw_pointer(dest, bytes);
        if (base == nullptr) return;
        for (std::uint32_t y = 0; y < height_; ++y) {
            std::uint8_t *row = base + static_cast<std::size_t>(y) * stride * bpp;
            const std::uint32_t *src = &rgba_[static_cast<std::size_t>(y) * width_];
            if (bpp == 4u) { std::memcpy(row, src, w * 4u); continue; }
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint32_t c = src[x], r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu;
                std::uint16_t v;
                if (format == 0u) v = static_cast<std::uint16_t>((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
                else if (format == 1u) v = static_cast<std::uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000u);
                else v = static_cast<std::uint16_t>((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000u);
                std::memcpy(row + x * 2u, &v, 2);
            }
        }
        if (on_picture_) on_picture_(dest, bytes);
    }

    std::function<void(std::uint32_t, std::uint32_t)> on_picture_;
    std::shared_ptr<MovieStats> stats_;
    AVCodecContext *video_{}, *audio_{};
    AVPacket *packet_{};
    AVFrame *frame_{};
    std::size_t audio_block_{};
    std::vector<std::uint8_t> scratch_;
    std::vector<std::uint32_t> rgba_;
    std::int64_t last_pts_{-1};
    std::uint32_t barren_{}; // consecutive AUs that gave no picture
    std::uint32_t width_{}, height_{};
};

} // namespace

std::unique_ptr<p3p3ds::hle::MpegDecoder> make_ffmpeg_decoder(std::function<void(std::uint32_t, std::uint32_t)> on_picture,
                                                             std::shared_ptr<MovieStats> stats) {
    return std::make_unique<FfmpegDecoder>(std::move(on_picture), std::move(stats));
}

} // namespace p3pios

#endif // P3P_HAVE_FFMPEG
