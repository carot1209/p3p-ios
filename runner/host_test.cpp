// Headless test of the live runner: plays the game in real time for a number
// of seconds the way the iOS shell does (pulling video at 60 Hz and audio in
// device-sized chunks, pushing scripted buttons) and writes what a player
// would have seen and heard.
//
//   p3p_host_test --elf eboot.elf --iso game.iso --data <dir> --seconds 60
//                 [--frames <dir> --frame-every 2] [--wav out.wav]
//                 [--press <second>:<BUTTONS>:<duration s>]... [--save-slot N] [--no-skip-movies]
//                 [--gpu soft|null [--trace file --trace-at <guest second>... --trace-frames N]]
#include "live_runner.hpp"
#include "soft_backend.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {
// Draws nothing: measures the CPU cost of the GPU front end alone.
struct NullBackend final : p3pios::gpu::Backend {
    const char *name() const override { return "null"; }
    void create_texture(std::uint32_t, std::uint32_t, std::uint32_t, const std::uint32_t *) override {}
    void destroy_texture(std::uint32_t) override {}
    void create_target(std::uint32_t) override {}
    void create_depth(std::uint32_t) override {}
    void upload_target(std::uint32_t, const std::uint32_t *) override {}
    void draw(const p3pios::gpu::State &, const p3pios::gpu::Vertex *, std::uint32_t) override {}
    bool read_target(std::uint32_t, std::uint32_t *rgba) override {
        std::memset(rgba, 0x40, p3pios::kScreenWidth * p3pios::kScreenHeight * 4u);
        return true;
    }
    void flush() override {}
};

struct Press { double at, duration; std::uint32_t buttons; };

std::uint32_t parse_buttons(const std::string &text) {
    using namespace p3pios::button;
    static const std::pair<const char *, std::uint32_t> names[] = {
        {"START", Start}, {"SELECT", Select}, {"UP", Up}, {"DOWN", Down}, {"LEFT", Left}, {"RIGHT", Right},
        {"CROSS", Cross}, {"CIRCLE", Circle}, {"SQUARE", Square}, {"TRIANGLE", Triangle}, {"L", L}, {"R", R}};
    std::uint32_t mask = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('+', start);
        const std::string name = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        bool found = false;
        for (const auto &[n, bit] : names)
            if (name == n) { mask |= bit; found = true; }
        if (!found) { std::fprintf(stderr, "unknown button %s\n", name.c_str()); std::exit(2); }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return mask;
}

void write_bmp(const std::string &path, const std::vector<std::uint8_t> &rgbx) {
    const std::uint32_t w = p3pios::kScreenWidth, h = p3pios::kScreenHeight, row = w * 3u, size = 54u + row * h;
    std::vector<std::uint8_t> out(size, 0);
    auto put32 = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) out[at + i] = static_cast<std::uint8_t>(v >> (8 * i)); };
    out[0] = 'B'; out[1] = 'M'; put32(2, size); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    out[26] = 1; out[28] = 24; put32(34, row * h);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto *p = &rgbx[((h - 1u - y) * w + x) * 4u];
            auto *q = &out[54u + y * row + x * 3u];
            q[0] = p[2]; q[1] = p[1]; q[2] = p[0];
        }
    if (FILE *f = std::fopen(path.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); }
}
} // namespace

int main(int argc, char **argv) {
    p3pios::Config config;
    config.build_id = "host-test";
    std::string frames_dir, wav_path;
    double seconds = 30.0;
    int frame_every = 60, save_slot = -2;
    std::vector<Press> presses;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { if (++i >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[i]; };
        if (a == "--elf") config.elf_path = value();
        else if (a == "--iso") config.iso_path = value();
        else if (a == "--data") config.data_dir = value();
        else if (a == "--seconds") seconds = std::stod(value());
        else if (a == "--frames") frames_dir = value();
        else if (a == "--frame-every") frame_every = std::stoi(value());
        else if (a == "--wav") wav_path = value();
        else if (a == "--save-slot") save_slot = std::stoi(value());
        else if (a == "--no-skip-movies") config.skip_movies = false;
        else if (a == "--gpu") {
            // The GPU front end on the CPU reference back end (the Metal one only exists on Apple systems).
            const std::string kind = value();
            if (kind == "soft") config.make_gpu_backend = [](std::string &) { return p3pios::gpu::make_soft_backend(); };
            else if (kind == "null") config.make_gpu_backend = [](std::string &) -> std::unique_ptr<p3pios::gpu::Backend> { return std::make_unique<NullBackend>(); };
            else { std::fprintf(stderr, "--gpu soft|null\n"); return 2; }
        }
        else if (a == "--trace") config.trace_path = value();
        else if (a == "--trace-at") config.trace_at.push_back(std::stod(value()));
        else if (a == "--trace-frames") config.trace_frames = static_cast<std::uint32_t>(std::stoul(value()));
        else if (a == "--press") {
            const std::string v = value();
            const auto c1 = v.find(':'), c2 = v.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) { std::fprintf(stderr, "--press <second>:<BUTTONS>:<duration>\n"); return 2; }
            presses.push_back({std::stod(v.substr(0, c1)), std::stod(v.substr(c2 + 1)), parse_buttons(v.substr(c1 + 1, c2 - c1 - 1))});
        } else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    if (config.elf_path.empty() || config.iso_path.empty() || config.data_dir.empty()) {
        std::fprintf(stderr, "--elf, --iso and --data are required\n");
        return 2;
    }
    std::filesystem::create_directories(config.data_dir);
    if (!frames_dir.empty()) std::filesystem::create_directories(frames_dir);

    p3pios::LiveRunner runner;
    std::string error;
    if (!runner.start(config, error)) { std::fprintf(stderr, "start failed: %s\n", error.c_str()); return 1; }

    FILE *wav = wav_path.empty() ? nullptr : std::fopen(wav_path.c_str(), "wb");
    std::uint32_t wav_bytes = 0;
    if (wav) { std::vector<char> header(44, 0); std::fwrite(header.data(), 1, header.size(), wav); }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::vector<std::uint8_t> frame(p3pios::kScreenWidth * p3pios::kScreenHeight * 4u, 0);
    std::uint64_t serial = 0, ticks = 0, shown = 0, nonblack_frames = 0, audio_nonzero = 0, audio_frames = 0;
    std::uint64_t last_dialog = 0;
    constexpr std::size_t kChunk = 735; // 1/60 s of audio
    std::vector<std::int16_t> pcm(kChunk * 2u);
    double last_print = 0.0;
    for (;;) {
        const double t = std::chrono::duration<double>(Clock::now() - start).count();
        if (t >= seconds) break;
        const auto st = runner.status();
        if (st.finished) break;
        std::uint32_t buttons = 0;
        for (const auto &p : presses)
            if (t >= p.at && t < p.at + p.duration) buttons |= p.buttons;
        runner.set_pad(buttons);

        const std::uint64_t now_serial = runner.copy_frame(frame.data(), serial);
        if (now_serial != serial) {
            serial = now_serial;
            ++shown;
            bool nonblack = false;
            for (std::size_t i = 0; i < frame.size(); i += 4u * 97u)
                if (frame[i] | frame[i + 1] | frame[i + 2]) { nonblack = true; break; }
            nonblack_frames += nonblack;
        }
        if (!frames_dir.empty() && ticks % static_cast<std::uint64_t>(frame_every) == 0u && serial != 0u) {
            char name[64];
            std::snprintf(name, sizeof name, "/t%07.2f.bmp", t);
            write_bmp(frames_dir + name, frame);
        }
        runner.pull_audio(pcm.data(), kChunk);
        for (auto s : pcm) audio_nonzero += s != 0;
        audio_frames += kChunk;
        if (wav) { std::fwrite(pcm.data(), sizeof(std::int16_t), pcm.size(), wav); wav_bytes += static_cast<std::uint32_t>(pcm.size() * 2u); }

        p3pios::SaveDialog dialog;
        if (runner.pending_save_dialog(dialog) && dialog.serial != last_dialog) {
            last_dialog = dialog.serial;
            std::printf("[%.1fs] save dialog kind=%d slots=%zu suggested=%d\n", t, static_cast<int>(dialog.kind), dialog.slots.size(), dialog.suggested);
            for (std::size_t i = 0; i < dialog.slots.size() && i < 4; ++i)
                std::printf("    slot %zu: %s\n", i, dialog.slots[i].exists ? dialog.slots[i].text.c_str() : "(empty)");
            runner.resolve_save_dialog(save_slot == -2 ? dialog.suggested : save_slot);
        }
        if (t - last_print >= 5.0) {
            last_print = t;
            std::printf("[%.1fs] stage=%s fps=%.1f speed=%.0f%% vblank=%llu shown=%llu nonblack=%llu underruns=%llu\n", t, st.stage.c_str(), st.fps,
                        st.speed * 100.0, static_cast<unsigned long long>(st.vblanks), static_cast<unsigned long long>(shown),
                        static_cast<unsigned long long>(nonblack_frames), static_cast<unsigned long long>(st.audio_underruns));
            std::fflush(stdout);
        }
        ++ticks;
        std::this_thread::sleep_until(start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(static_cast<double>(ticks) / 60.0)));
    }
    runner.stop();
    const auto st = runner.status();
    if (wav) {
        std::fseek(wav, 0, SEEK_SET);
        auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, wav); };
        auto u16 = [&](std::uint16_t v) { std::fwrite(&v, 2, 1, wav); };
        std::fwrite("RIFF", 1, 4, wav); u32(36u + wav_bytes); std::fwrite("WAVEfmt ", 1, 8, wav);
        u32(16u); u16(1u); u16(2u); u32(p3pios::kAudioRate); u32(p3pios::kAudioRate * 4u); u16(4u); u16(16u);
        std::fwrite("data", 1, 4, wav); u32(wav_bytes);
        std::fclose(wav);
    }
    std::printf("\n%s\nshown frames: %llu (non-black %llu)\naudio: %llu frames pulled, %llu non-zero samples\nstop: %s\n", runner.report().c_str(),
                static_cast<unsigned long long>(shown), static_cast<unsigned long long>(nonblack_frames),
                static_cast<unsigned long long>(audio_frames), static_cast<unsigned long long>(audio_nonzero), st.stop_reason.c_str());
    return 0;
}
