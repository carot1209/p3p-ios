#ifdef P3P_HAVE_FFMPEG
#include "movie_player.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

#include <algorithm>

namespace p3pios {
namespace {

std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

class FfmpegMovie final : public MoviePlayer {
public:
    ~FfmpegMovie() override {
        avcodec_free_context(&video_);
        avcodec_free_context(&audio_);
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        if (format_ != nullptr) avformat_close_input(&format_);
    }

    bool init(const std::string &path, std::string &error) {
        // Sofdec streams carry no codec tag: the video is MPEG-1/2 (one decoder reads both).
        format_ = avformat_alloc_context();
        if (format_ == nullptr) { error = "out of memory"; return false; }
        format_->video_codec_id = AV_CODEC_ID_MPEG2VIDEO;
        if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0) { error = "cannot open " + path; return false; }
        if (avformat_find_stream_info(format_, nullptr) < 0) { error = "no streams in " + path; return false; }
        for (unsigned i = 0; i < format_->nb_streams; ++i) {
            const AVCodecParameters *par = format_->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_ == nullptr) {
                if (open_codec(par, video_)) video_index_ = static_cast<int>(i);
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && audio_ == nullptr) {
                if (open_codec(par, audio_)) audio_index_ = static_cast<int>(i);
            }
        }
        if (video_ == nullptr) { error = "no video decoder for " + path; return false; }
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        const AVRational sar = format_->streams[video_index_]->sample_aspect_ratio.num > 0 ? format_->streams[video_index_]->sample_aspect_ratio
                                                                                          : video_->sample_aspect_ratio;
        aspect_ = static_cast<double>(video_->width) / std::max(video_->height, 1);
        if (sar.num > 0 && sar.den > 0) aspect_ *= static_cast<double>(sar.num) / sar.den;
        return true;
    }

    int width() const override { return video_->width; }
    int height() const override { return video_->height; }
    double aspect() const override { return aspect_; }

    bool next(std::vector<std::uint32_t> &rgba, std::vector<std::int16_t> &pcm, double &seconds) override {
        for (;;) {
            if (avcodec_receive_frame(video_, frame_) == 0) {
                convert(*frame_, rgba);
                const AVRational tb = format_->streams[video_index_]->time_base;
                const std::int64_t pts = frame_->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    if (first_pts_ == AV_NOPTS_VALUE) first_pts_ = pts;
                    seconds = static_cast<double>(pts - first_pts_) * tb.num / tb.den;
                } else {
                    const AVRational fr = video_->framerate.num > 0 ? video_->framerate : AVRational{30000, 1001};
                    seconds = static_cast<double>(pictures_) * fr.den / fr.num;
                }
                ++pictures_;
                return true;
            }
            if (draining_) return false;
            if (av_read_frame(format_, packet_) < 0) {
                avcodec_send_packet(video_, nullptr);
                draining_ = true;
                continue;
            }
            if (packet_->stream_index == video_index_) {
                avcodec_send_packet(video_, packet_);
            } else if (packet_->stream_index == audio_index_ && audio_ != nullptr) {
                if (avcodec_send_packet(audio_, packet_) == 0)
                    while (avcodec_receive_frame(audio_, frame_) == 0) append_audio(*frame_, pcm);
            }
            av_packet_unref(packet_);
        }
    }

private:
    static bool open_codec(const AVCodecParameters *par, AVCodecContext *&ctx) {
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (codec == nullptr) return false;
        ctx = avcodec_alloc_context3(codec);
        if (ctx == nullptr) return false;
        if (avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        return true;
    }

    static void convert(const AVFrame &f, std::vector<std::uint32_t> &rgba) {
        const int w = f.width, h = f.height;
        rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        if (f.format != AV_PIX_FMT_YUV420P && f.format != AV_PIX_FMT_YUVJ420P) { std::fill(rgba.begin(), rgba.end(), 0xFF000000u); return; }
        for (int y = 0; y < h; ++y) {
            const std::uint8_t *yr = f.data[0] + static_cast<std::ptrdiff_t>(y) * f.linesize[0];
            const std::uint8_t *ur = f.data[1] + static_cast<std::ptrdiff_t>(y / 2) * f.linesize[1];
            const std::uint8_t *vr = f.data[2] + static_cast<std::ptrdiff_t>(y / 2) * f.linesize[2];
            std::uint32_t *out = &rgba[static_cast<std::size_t>(y) * static_cast<std::size_t>(w)];
            for (int x = 0; x < w; ++x) {
                const int c = 298 * (yr[x] - 16), u = ur[x / 2] - 128, v = vr[x / 2] - 128;
                out[x] = clamp8((c + 409 * v + 128) >> 8) | (static_cast<std::uint32_t>(clamp8((c - 100 * u - 208 * v + 128) >> 8)) << 8) |
                         (static_cast<std::uint32_t>(clamp8((c + 516 * u + 128) >> 8)) << 16) | 0xFF000000u;
            }
        }
    }

    // Any sample format/rate/channel count -> 44.1 kHz stereo S16 (linear interpolation).
    void append_audio(const AVFrame &f, std::vector<std::int16_t> &pcm) {
        const int channels = std::max(f.ch_layout.nb_channels, 1), rate = f.sample_rate > 0 ? f.sample_rate : 44100;
        auto sample = [&](int i, int c) -> float {
            c = std::min(c, channels - 1);
            switch (f.format) {
            case AV_SAMPLE_FMT_S16: return reinterpret_cast<const std::int16_t *>(f.data[0])[i * channels + c] / 32768.0f;
            case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const std::int16_t *>(f.extended_data[c])[i] / 32768.0f;
            case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float *>(f.data[0])[i * channels + c];
            case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float *>(f.extended_data[c])[i];
            default: return 0.0f;
            }
        };
        const double step = static_cast<double>(rate) / 44100.0;
        while (position_ < static_cast<double>(f.nb_samples)) {
            const int i = static_cast<int>(position_);
            const float t = static_cast<float>(position_ - i);
            for (int c = 0; c < 2; ++c) {
                const float a = i == 0 && t == 0.0f ? sample(0, c) : sample(i, c);
                const float b = i + 1 < f.nb_samples ? sample(i + 1, c) : a;
                pcm.push_back(static_cast<std::int16_t>(std::clamp(a + (b - a) * t, -1.0f, 1.0f) * 32767.0f));
            }
            position_ += step;
        }
        position_ -= static_cast<double>(f.nb_samples);
    }

    AVFormatContext *format_{};
    AVCodecContext *video_{}, *audio_{};
    AVPacket *packet_{};
    AVFrame *frame_{};
    int video_index_{-1}, audio_index_{-1};
    double aspect_{4.0 / 3.0}, position_{0.0};
    std::int64_t first_pts_{AV_NOPTS_VALUE};
    std::uint64_t pictures_{};
    bool draining_{};
};

} // namespace

std::unique_ptr<MoviePlayer> MoviePlayer::open(const std::string &path, std::string &error) {
    auto movie = std::make_unique<FfmpegMovie>();
    if (!movie->init(path, error)) return nullptr;
    return movie;
}

} // namespace p3pios
#endif // P3P_HAVE_FFMPEG
