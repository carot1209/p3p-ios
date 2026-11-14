// Replays a recorded GPU trace (trace.hpp) on a back end and writes every
// picture as a BMP; with two back ends, also how much their pictures differ.
//
//   gpu_replay <trace> <out dir> soft|metal [soft|metal]
//
// On a macOS runner this is the test of the Metal back end: the trace holds
// frames of the real game recorded on the development machine, and the soft
// back end (the same code that was checked against the game there) gives the
// expected pictures.
#include "soft_backend.hpp"
#include "trace.hpp"
#ifdef P3P_HAVE_METAL
#include "metal_backend.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace p3pios::gpu;

void write_bmp(const std::string &path, const std::uint32_t *rgba) {
    const std::uint32_t w = kViewWidth, h = kViewHeight, row = w * 3u, size = 54u + row * h;
    std::vector<std::uint8_t> out(size, 0);
    auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    out[0] = 'B'; out[1] = 'M'; put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    out[26] = 1; out[28] = 24; put32(34, row * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t c = rgba[(h - 1u - y) * w + x];
            auto *q = &out[54u + y * row + x * 3u];
            q[0] = static_cast<std::uint8_t>(c >> 16); q[1] = static_cast<std::uint8_t>(c >> 8); q[2] = static_cast<std::uint8_t>(c);
        }
    if (FILE *f = std::fopen(path.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}

std::unique_ptr<Backend> make(const std::string &name) {
    if (name == "soft") return make_soft_backend();
#ifdef P3P_HAVE_METAL
    if (name == "metal") {
        std::string error;
        auto b = make_metal_backend(error);
        if (!b) std::fprintf(stderr, "metal: %s\n", error.c_str());
        return b;
    }
#endif
    std::fprintf(stderr, "unknown back end %s\n", name.c_str());
    return nullptr;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 4) { std::fprintf(stderr, "gpu_replay <trace> <out dir> <backend> [<backend>]\n"); return 2; }
    const std::string trace = argv[1], out = argv[2];
    using Frames = std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>>;
    std::vector<Frames> results;
    std::vector<std::string> names;
    for (int i = 3; i < argc && i < 5; ++i) {
        auto backend = make(argv[i]);
        if (!backend) return 1;
        Frames frames;
        const auto start = std::chrono::steady_clock::now();
        const int n = replay_trace(trace, *backend, [&](std::uint32_t window, std::uint32_t index, const std::uint32_t *rgba) {
            frames[{window, index}].assign(rgba, rgba + kViewWidth * kViewHeight);
            char name[96];
            std::snprintf(name, sizeof name, "/w%02u_f%02u_%s.bmp", window, index, argv[i]);
            write_bmp(out + name, rgba);
        });
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("%s (%s): %d pictures in %.0f ms\n", argv[i], backend->name(), n, ms);
        if (n <= 0) return 1;
        results.push_back(std::move(frames));
        names.push_back(argv[i]);
    }
    if (results.size() == 2u) {
        // Per picture: mean absolute difference of r, g, b (0..255) and the share of pixels with a channel off by more than 16.
        std::printf("picture     mean diff   pixels off by > 16\n");
        bool bad = false;
        for (const auto &[key, a] : results[0]) {
            const auto it = results[1].find(key);
            if (it == results[1].end()) { std::printf("w%02u f%02u    missing in %s\n", key.first, key.second, names[1].c_str()); bad = true; continue; }
            std::uint64_t sum = 0, off = 0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                bool far = false;
                for (int ch = 0; ch < 3; ++ch) {
                    const int d = std::abs(static_cast<int>((a[i] >> (8 * ch)) & 0xFFu) - static_cast<int>((it->second[i] >> (8 * ch)) & 0xFFu));
                    sum += static_cast<std::uint64_t>(d);
                    far = far || d > 16;
                }
                off += far;
            }
            const double mean = static_cast<double>(sum) / (static_cast<double>(a.size()) * 3.0);
            const double share = 100.0 * static_cast<double>(off) / static_cast<double>(a.size());
            std::printf("w%02u f%02u    %8.3f    %6.2f%%\n", key.first, key.second, mean, share);
        }
        return bad ? 1 : 0;
    }
    return 0;
}
