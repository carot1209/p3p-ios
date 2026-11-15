#pragma once
// Plays a movie file from outside the game: the PS2 version's Sofdec movies
// (.SFD: MPEG program stream, MPEG-1/2 video, ADX audio) through FFmpeg.
// Used to put Persona 3 FES's anime cutscenes back where P3P shows stills.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace p3pios {

class MoviePlayer {
public:
    virtual ~MoviePlayer() = default;
    static std::unique_ptr<MoviePlayer> open(const std::string &path, std::string &error);
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
    // Width / height the picture is meant to be shown at (4:3 for the PS2 movies).
    [[nodiscard]] virtual double aspect() const = 0;
    // The next picture (width x height, bytes R,G,B,A) and its time in seconds.
    // Audio decoded on the way is appended to `pcm`: 44.1 kHz stereo S16.
    // False at the end of the movie.
    virtual bool next(std::vector<std::uint32_t> &rgba, std::vector<std::int16_t> &pcm, double &seconds) = 0;
};

} // namespace p3pios
