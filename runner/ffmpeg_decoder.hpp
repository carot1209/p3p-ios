#pragma once
// Real decoding for the upstream sceMpeg HLE (core/include/p3p3ds/hle/mpeg.hpp):
// H.264 pictures and ATRAC3plus audio of PSMF movies through a minimal FFmpeg
// (scripts/build_ffmpeg.sh: only the h264 and atrac3p decoders).
#include "p3p3ds/hle/mpeg.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace p3pios {

struct MovieStats {
    std::uint64_t video_aus{}, pictures{}, video_errors{}, audio_aus{}, audio_errors{};
    std::uint32_t width{}, height{};
};

// `on_picture(address, bytes)`: guest memory the decoder wrote a picture to
// (the renderer must know: the GE may draw from it or display it).
std::unique_ptr<p3p3ds::hle::MpegDecoder> make_ffmpeg_decoder(std::function<void(std::uint32_t, std::uint32_t)> on_picture,
                                                             std::shared_ptr<MovieStats> stats);

} // namespace p3pios
