#include "live_runner.hpp"

#include "p3p3ds/ge/renderer.hpp"
#include "p3p3ds/hle/hle_modules.hpp"
#include "p3p3ds/input.hpp"
#include "p3p3ds/interpreter.hpp"
#include "p3p3ds/kernel_state.hpp"
#include "p3p3ds/vfs.hpp"
#include "bootstrap_expectations.hpp"
#include "mod_paths.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

namespace psprecomp {
void register_generated_functions(Runtime &);
void apply_generated_patches(GuestMemory &);
} // namespace psprecomp

namespace p3pios {
namespace {

using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// Single-producer (game thread) / single-consumer (audio thread) ring of stereo frames.
class AudioRing {
public:
    static constexpr std::size_t kCapacity = 1u << 15; // frames (0.74 s)
    static constexpr std::size_t kTarget = kAudioRate * 70u / 1000u;    // cushion before playback starts
    static constexpr std::size_t kHighWater = kAudioRate * 220u / 1000u; // more than this: skip ahead

    void push(const std::int16_t *stereo, std::size_t frames) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        const std::size_t free_frames = kCapacity - (w - r);
        if (frames > free_frames) { dropped_.fetch_add(frames - free_frames, std::memory_order_relaxed); frames = free_frames; }
        for (std::size_t i = 0; i < frames; ++i) {
            const std::size_t at = ((w + i) & (kCapacity - 1u)) * 2u;
            data_[at] = stereo[i * 2u];
            data_[at + 1u] = stereo[i * 2u + 1u];
        }
        write_.store(w + frames, std::memory_order_release);
    }
    void pull(std::int16_t *out, std::size_t frames) noexcept {
        auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        std::size_t available = w - r;
        if (buffering_) {
            if (available < kTarget) { std::memset(out, 0, frames * 2u * sizeof(std::int16_t)); return; }
            buffering_ = false;
        }
        if (available > kHighWater) { // the producer ran ahead (or playback stalled): keep latency bounded
            r += available - kTarget;
            available = kTarget;
            skips_.fetch_add(1u, std::memory_order_relaxed);
        }
        const std::size_t n = std::min(frames, available);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t at = ((r + i) & (kCapacity - 1u)) * 2u;
            out[i * 2u] = data_[at];
            out[i * 2u + 1u] = data_[at + 1u];
        }
        if (n < frames) {
            std::memset(out + n * 2u, 0, (frames - n) * 2u * sizeof(std::int16_t));
            underruns_.fetch_add(1u, std::memory_order_relaxed);
            buffering_ = true;
        }
        read_.store(r + n, std::memory_order_release);
    }
    std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t skips() const noexcept { return skips_.load(std::memory_order_relaxed); }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::vector<std::int16_t> data_ = std::vector<std::int16_t>(kCapacity * 2u);
    std::atomic<std::size_t> write_{0}, read_{0};
    std::atomic<std::uint64_t> underruns_{0}, skips_{0}, dropped_{0};
    bool buffering_{true}; // consumer only
};

// Mixes every sceAudio buffer at its virtual start time into one timeline
// (game thread). Everything before the guest clock's "now" is final: a buffer
// handed over later starts at or after the time it was handed over.
class LiveMixer {
public:
    void add(std::uint64_t start_us, const std::vector<std::int16_t> &stereo) {
        std::uint64_t start = start_us * kAudioRate / 1000000u;
        std::size_t skip = 0;
        if (start < flushed_) { // late: keep the part that is still ahead
            skip = static_cast<std::size_t>(flushed_ - start);
            late_frames_ += skip;
            start = flushed_;
        }
        const std::size_t frames = stereo.size() / 2u;
        if (skip >= frames) return;
        const std::size_t offset = static_cast<std::size_t>(start - flushed_);
        const std::size_t end = offset + (frames - skip);
        if (end > kMaxPendingFrames) return; // far in the future: not expected
        if (end * 2u > pending_.size()) pending_.resize(end * 2u, 0);
        for (std::size_t i = skip; i < frames; ++i) {
            pending_[(offset + i - skip) * 2u] += stereo[i * 2u];
            pending_[(offset + i - skip) * 2u + 1u] += stereo[i * 2u + 1u];
        }
        ++buffers_;
    }
    // Hands every frame before `now_us` to the ring (silence where nothing played).
    void release_until(std::uint64_t now_us, AudioRing &ring) {
        const std::uint64_t target = now_us * kAudioRate / 1000000u;
        if (target <= flushed_) return;
        std::uint64_t count = target - flushed_;
        if (count > kAudioRate) { // a jump of the guest clock: keep the last second only
            const std::uint64_t drop = count - kAudioRate;
            const std::size_t have = std::min<std::uint64_t>(drop, pending_.size() / 2u);
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(have * 2u));
            flushed_ += drop;
            count = kAudioRate;
        }
        scratch_.assign(static_cast<std::size_t>(count) * 2u, 0);
        const std::size_t have = std::min<std::size_t>(static_cast<std::size_t>(count), pending_.size() / 2u);
        for (std::size_t i = 0; i < have * 2u; ++i) {
            const auto v = pending_[i];
            const auto c = std::clamp<std::int32_t>(v, -32768, 32767);
            clipped_ += c != v;
            scratch_[i] = static_cast<std::int16_t>(c);
            nonzero_ += c != 0;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(have * 2u));
        flushed_ += count;
        ring.push(scratch_.data(), static_cast<std::size_t>(count));
    }
    std::uint64_t buffers() const noexcept { return buffers_; }
    std::uint64_t late_frames() const noexcept { return late_frames_; }
    std::uint64_t nonzero_samples() const noexcept { return nonzero_; }
    std::uint64_t clipped() const noexcept { return clipped_; }

private:
    static constexpr std::size_t kMaxPendingFrames = kAudioRate * 4u;
    std::deque<std::int32_t> pending_;
    std::vector<std::int16_t> scratch_;
    std::uint64_t flushed_{0}, buffers_{0}, late_frames_{0}, nonzero_{0}, clipped_{0};
};

struct PadSource final : p3p3ds::input::InputSource {
    p3p3ds::input::PadState pad;
    p3p3ds::input::PadState sample(std::uint64_t) override { return pad; }
};

// PARAM.SFO texts are UTF-8; line breaks become spaces for a one-line label.
std::string one_line(std::string text) {
    for (auto &c : text)
        if (c == '\n' || c == '\r') c = ' ';
    return text;
}

constexpr int kNoChoice = INT_MIN;

} // namespace

struct LiveRunner::Impl {
    Config config;
    pthread_t thread{};
    bool thread_started{false};
    std::atomic<bool> stop_requested{false}, paused{false}, finished{false}, running{false};

    // video
    std::mutex frame_mutex;
    std::vector<std::uint8_t> frame = std::vector<std::uint8_t>(kScreenWidth * kScreenHeight * 4u, 0);
    std::uint64_t frame_serial{0};

    // audio
    AudioRing ring;
    LiveMixer mixer; // game thread

    // input: buttons | analog_x << 32 | analog_y << 40
    std::atomic<std::uint64_t> pad{(128ull << 32) | (128ull << 40)};

    // save dialog
    std::mutex dialog_mutex;
    SaveDialog dialog;
    bool dialog_pending{false};
    std::atomic<int> dialog_choice{kNoChoice};

    // status
    std::mutex status_mutex;
    Status state;
    std::string report_text;

    // game thread state
    std::uint64_t cpu_fb_bytes{0};     // CPU stores into the displayed framebuffer since the last vblank
    std::uint32_t fb_base{0}, fb_end{0}; // canonical range of the displayed framebuffer

    static void *thread_main(void *arg) {
        static_cast<Impl *>(arg)->run();
        return nullptr;
    }

    void set_stage(const std::string &stage) {
        std::lock_guard<std::mutex> lock(status_mutex);
        state.stage = stage;
    }

    void write_report(const std::string &text) {
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            report_text = text;
        }
        if (config.data_dir.empty()) return;
        const std::string path = config.data_dir + "/report.txt";
        if (FILE *f = std::fopen(path.c_str(), "w")) {
            std::fputs(text.c_str(), f);
            std::fclose(f);
        }
    }

    void present(const psprecomp::GuestMemory &memory, const p3p3ds::hle::DisplayFramebufInfo &fb) {
        if (!fb.active) return;
        const std::uint32_t format = static_cast<std::uint32_t>(fb.pixelformat);
        const std::uint32_t bpp = format == 3u ? 4u : 2u;
        const std::uint32_t stride = static_cast<std::uint32_t>(std::max(fb.bufferwidth, 1));
        const std::size_t bytes = static_cast<std::size_t>(stride) * kScreenHeight * bpp;
        const std::uint8_t *src = memory.raw_pointer(fb.topaddr, bytes);
        if (src == nullptr || stride < kScreenWidth) return;
        std::lock_guard<std::mutex> lock(frame_mutex);
        std::uint8_t *dst = frame.data();
        for (std::uint32_t y = 0; y < kScreenHeight; ++y) {
            const std::uint8_t *row = src + static_cast<std::size_t>(y) * stride * bpp;
            if (format == 3u) {
                std::memcpy(dst, row, kScreenWidth * 4u); // guest ABGR8888 little endian = bytes R,G,B,A
                dst += kScreenWidth * 4u;
                continue;
            }
            for (std::uint32_t x = 0; x < kScreenWidth; ++x) {
                const std::uint32_t c = static_cast<std::uint32_t>(row[x * 2u]) | (static_cast<std::uint32_t>(row[x * 2u + 1u]) << 8);
                std::uint8_t r, g, b;
                if (format == 0u) { r = (c & 0x1Fu) << 3; g = ((c >> 5) & 0x3Fu) << 2; b = ((c >> 11) & 0x1Fu) << 3; }
                else if (format == 1u) { r = (c & 0x1Fu) << 3; g = ((c >> 5) & 0x1Fu) << 3; b = ((c >> 10) & 0x1Fu) << 3; }
                else { r = (c & 0xFu) * 17u; g = ((c >> 4) & 0xFu) * 17u; b = ((c >> 8) & 0xFu) * 17u; }
                dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 255;
                dst += 4;
            }
        }
        ++frame_serial;
    }

    void run() {
        using namespace p3p3ds;
        std::string stage = "create runtime";
        std::string fatal;
        std::unique_ptr<psprecomp::Runtime> rt_owner;
        std::unique_ptr<KernelState> kernel_owner;
        const auto run_start = Clock::now();
        auto pad_source = std::make_shared<PadSource>();
        std::uint64_t game_frames = 0;
        double fps = 0.0, speed = 0.0;
        std::uint64_t movie_presses = 0;

        const auto make_report = [&](const char *status) {
            char buf[1024];
            const double wall = seconds_since(run_start);
            const double guest = kernel_owner ? static_cast<double>(kernel_owner->threads().now()) / 1e6 : 0.0;
            std::snprintf(buf, sizeof buf,
                          "P3P iOS - Persona 3 Portable static recompilation\n"
                          "build  : %s\n"
                          "status : %s\n"
                          "stage  : %s\n"
                          "fps    : %.1f game frames/s\n"
                          "speed  : %.0f%% of real time (last second)\n"
                          "guest  : %.1f s   wall: %.1f s\n"
                          "vblank : %llu   frames: %llu\n"
                          "audio  : %llu buffers, %llu underruns, %llu skips, %llu dropped, %llu late frames\n"
                          "movie  : %llu START presses\n"
                          "iso    : %s\n"
                          "stop   : %s\n"
                          "error  : %s\n",
                          config.build_id.c_str(), status, stage.c_str(), fps, speed * 100.0, guest, wall,
                          static_cast<unsigned long long>(kernel_owner ? kernel_owner->threads().vblank_count() : 0u),
                          static_cast<unsigned long long>(game_frames), static_cast<unsigned long long>(mixer.buffers()),
                          static_cast<unsigned long long>(ring.underruns()), static_cast<unsigned long long>(ring.skips()),
                          static_cast<unsigned long long>(ring.dropped()), static_cast<unsigned long long>(mixer.late_frames()),
                          static_cast<unsigned long long>(movie_presses),
                          std::filesystem::path(config.iso_path).filename().string().c_str(),
                          rt_owner && rt_owner->stopped() ? rt_owner->stop_reason().c_str() : "-", fatal.empty() ? "-" : fatal.c_str());
            return std::string(buf);
        };

        try {
            write_report(make_report("booting"));
            rt_owner = std::make_unique<psprecomp::Runtime>();
            kernel_owner = std::make_unique<KernelState>();
            auto &rt = *rt_owner;
            auto &kernel = *kernel_owner;

            stage = "load eboot";
            set_stage(stage);
            const auto elf = psprecomp::Elf32Image::from_file(config.elf_path);
            const auto entry = elf.runtime_entry();
            (void)elf.load_and_relocate(rt.memory());
            psprecomp::apply_generated_patches(rt.memory());
            const auto module = elf.find_module_info(rt.memory());
            if (!module) throw std::runtime_error("missing module info");
            const auto sp = profile::stack_top - 0x100;
            rt.memory().zero(profile::stack_top - profile::stack_size, profile::stack_size);
            rt.cpu().gpr[28] = module->gp; rt.cpu().gpr[29] = sp; rt.cpu().gpr[26] = sp;
            rt.cpu().gpr[31] = hle::ThreadManager::kThreadReturnSentinel;

            stage = "register AOT";
            set_stage(stage);
            psprecomp::register_generated_functions(rt);

            stage = "mount iso";
            set_stage(stage);
            kernel.umd().set_medium_present(true);
            kernel.io().mount("disc0:", std::make_shared<vfs::IsoFileSystem>(config.iso_path));
            std::error_code ec;
            const std::string ms0 = config.data_dir + "/ms0";
            const std::string mods = config.data_dir + "/mods";
            std::filesystem::create_directories(ms0, ec);
            std::filesystem::create_directories(mods, ec);
            kernel.io().mount("ms0:", std::make_shared<vfs::HostFileSystem>(ms0), true);
            kernel.savedata().root = ms0 + "/PSP/SAVEDATA";
            kernel.io().alias(profile::kModPrefix, std::make_shared<vfs::HostFileSystem>(mods, true));
            kernel.threads().init_root_thread("root", entry, sp, module->gp);
            {
                std::set<std::uint32_t> stubs;
                std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>> imports;
                for (const auto &import : elf.scan_imports(rt.memory(), *module)) {
                    stubs.insert(import.stub_address);
                    imports.emplace_back(import.library, import.nid, import.stub_address);
                }
                kernel.modules().set_host_imports(std::move(imports));
                kernel.threads().set_import_stubs(std::move(stubs));
            }

            stage = "register HLE";
            set_stage(stage);
            hle::register_all_hle_modules(rt, kernel);
            static Interpreter interpreter;
            install_interpreter_fallback(&interpreter);
            kernel.input().source = pad_source;
            kernel.audio().set_sink([this](unsigned, std::uint64_t start, const std::vector<std::int16_t> &stereo) { mixer.add(start, stereo); });
            // CPU stores into the displayed framebuffer: a movie frame (the GE
            // rasterizer writes through raw pointers and is not seen here).
            rt.memory().vram_write_observer = [this](std::uint32_t address, std::size_t bytes) {
                if (address >= fb_base && address < fb_end) cpu_fb_bytes += bytes;
            };

            stage = "run";
            set_stage(stage);
            {
                std::lock_guard<std::mutex> lock(status_mutex);
                state.running = true;
            }
            running.store(true);

            std::uint64_t last_vblank = ~0ull;
            double wall_minus_guest_us = 0.0;
            bool pacing_started = false;
            const auto pace_origin = Clock::now();
            auto last_second = Clock::now();
            auto last_report = Clock::now();
            std::uint64_t frames_at_second = 0, guest_at_second = 0;
            std::uint32_t last_topaddr = 0;
            std::uint64_t movie_frames_seen = 0, movie_vblanks = 0;
            bool dialog_open = false;
            const hle::SavedataDialog *dialog_shown = nullptr;
            std::uint64_t dialog_serial = 0;
            bool suppress_input = false;

            kernel.display().on_vblank = [&](const hle::DisplayFramebufInfo &fb) {
                const auto vblank = kernel.threads().vblank_count();
                if (vblank == last_vblank) return;
                last_vblank = vblank;
                if (stop_requested.load(std::memory_order_relaxed)) {
                    rt.stop("Closed by the user");
                    throw psprecomp::FrontierHalt{};
                }
                bool reset_pacing = false;
                while (paused.load(std::memory_order_relaxed) && !stop_requested.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    reset_pacing = true;
                }

                // Controller.
                const std::uint64_t packed = pad.load(std::memory_order_relaxed);
                input::PadState p;
                p.buttons = static_cast<std::uint32_t>(packed & 0xFFFFFFFFu);
                p.analog_x = static_cast<std::uint8_t>(packed >> 32);
                p.analog_y = static_cast<std::uint8_t>(packed >> 40);
                const std::uint32_t host_buttons = p.buttons;

                // Movies: no video decoder yet (pictures are black), so START is
                // pressed for the game while it shows CPU-written movie frames.
                const std::uint32_t fb_bytes = static_cast<std::uint32_t>(std::max(fb.bufferwidth, 1)) * kScreenHeight * (fb.pixelformat == 3 ? 4u : 2u);
                const bool cpu_frame = cpu_fb_bytes >= fb_bytes / 2u;
                cpu_fb_bytes = 0;
                fb_base = psprecomp::GuestMemory::canonical(fb.topaddr);
                fb_end = fb_base + fb_bytes;
                if (config.skip_movies) {
                    std::uint64_t decoded = 0;
                    for (const auto &[handle, inst] : kernel.mpeg().instances) decoded += inst.video_decoded;
                    if (decoded != movie_frames_seen && cpu_frame) {
                        movie_frames_seen = decoded;
                        if (movie_vblanks++ % 40u < 6u) { p.buttons |= input::button::Start; ++movie_presses; }
                    } else {
                        if (decoded != movie_frames_seen) movie_frames_seen = decoded;
                        movie_vblanks = 0;
                    }
                }

                // Save/load list: the host UI chooses; the game gets no buttons meanwhile.
                if (const auto *d = kernel.savedata().pending_dialog(); d != nullptr) {
                    if (!dialog_open || d != dialog_shown) {
                        SaveDialog out;
                        out.kind = d->kind == hle::SavedataDialog::Kind::Load ? SaveDialog::Kind::Load
                                 : d->kind == hle::SavedataDialog::Kind::Save ? SaveDialog::Kind::Save : SaveDialog::Kind::Delete;
                        int best = -1;
                        for (int i = 0; i < static_cast<int>(d->slots.size()); ++i) {
                            const auto &s = d->slots[static_cast<std::size_t>(i)];
                            out.slots.push_back({s.exists, s.exists ? one_line(s.detail.empty() ? s.savedata_title : s.detail) : std::string()});
                            if (s.exists && (best < 0 || s.modified > d->slots[static_cast<std::size_t>(best)].modified)) best = i;
                        }
                        out.suggested = best < 0 ? 0 : best;
                        out.serial = ++dialog_serial;
                        dialog_choice.store(kNoChoice);
                        {
                            std::lock_guard<std::mutex> lock(dialog_mutex);
                            dialog = std::move(out);
                            dialog_pending = true;
                        }
                        dialog_open = true;
                        dialog_shown = d;
                    }
                    const int choice = dialog_choice.exchange(kNoChoice);
                    if (choice != kNoChoice) {
                        const int n = static_cast<int>(d->slots.size());
                        kernel.savedata().resolve(choice < 0 || choice >= n ? -1 : choice);
                        std::lock_guard<std::mutex> lock(dialog_mutex);
                        dialog_pending = false;
                        dialog_open = false;
                        dialog_shown = nullptr;
                    }
                    p = {};
                    suppress_input = true;
                    reset_pacing = true;
                } else {
                    if (dialog_open) {
                        std::lock_guard<std::mutex> lock(dialog_mutex);
                        dialog_pending = false;
                        dialog_open = false;
                        dialog_shown = nullptr;
                    }
                    if (suppress_input) { // until every button is released
                        if (host_buttons == 0u) suppress_input = false;
                        else p = {};
                    }
                }
                pad_source->pad = p;

                // Audio up to the guest clock's now.
                const std::uint64_t now_us = kernel.threads().now();
                mixer.release_until(now_us, ring);

                // Real-time pacing: the guest clock may not run ahead of the wall
                // clock; when it falls behind, the reference moves instead of racing.
                {
                    const double guest_us = static_cast<double>(now_us);
                    const double wall_us = std::chrono::duration<double, std::micro>(Clock::now() - pace_origin).count();
                    if (!pacing_started || reset_pacing) { wall_minus_guest_us = wall_us - guest_us; pacing_started = true; }
                    const double ahead_us = guest_us + wall_minus_guest_us - wall_us;
                    if (ahead_us > 500.0) std::this_thread::sleep_for(std::chrono::duration<double, std::micro>(ahead_us));
                    else if (ahead_us < -50000.0) wall_minus_guest_us = wall_us - guest_us;
                }

                present(rt.memory(), fb);
                if (fb.active && fb.topaddr != last_topaddr) { ++game_frames; last_topaddr = fb.topaddr; }

                const auto now = Clock::now();
                const double since = std::chrono::duration<double>(now - last_second).count();
                if (since >= 1.0) {
                    fps = static_cast<double>(game_frames - frames_at_second) / since;
                    speed = static_cast<double>(now_us - guest_at_second) / 1e6 / since;
                    frames_at_second = game_frames;
                    guest_at_second = now_us;
                    last_second = now;
                    {
                        std::lock_guard<std::mutex> lock(status_mutex);
                        state.fps = fps;
                        state.speed = speed;
                        state.vblanks = vblank;
                        state.audio_underruns = ring.underruns();
                    }
                    if (std::chrono::duration<double>(now - last_report).count() >= 10.0) {
                        write_report(make_report("running"));
                        last_report = now;
                    }
                }
            };

            write_report(make_report("running"));
            try { rt.run(entry, ~0ull); }
            catch (const psprecomp::FrontierHalt &) {}
            if (!rt.stopped()) rt.stop("Runtime returned");
        } catch (const std::exception &e) {
            fatal = e.what();
            if (rt_owner && !rt_owner->stopped()) rt_owner->stop(std::string("Exception: ") + fatal);
        } catch (...) {
            fatal = "unknown exception";
        }
        running.store(false);
        const std::string text = make_report("stopped");
        write_report(text);
        {
            std::lock_guard<std::mutex> lock(status_mutex);
            state.running = false;
            state.finished = true;
            state.stage = stage;
            state.stop_reason = !fatal.empty() ? fatal : (rt_owner && rt_owner->stopped() ? rt_owner->stop_reason() : std::string("stopped"));
        }
        if (kernel_owner) kernel_owner->display().on_vblank = {};
        p3p3ds::install_interpreter_fallback(nullptr);
        kernel_owner.reset();
        rt_owner.reset();
        finished.store(true);
    }
};

LiveRunner::LiveRunner() : impl_(std::make_unique<Impl>()) {}
LiveRunner::~LiveRunner() { stop(); }

bool LiveRunner::start(const Config &config, std::string &error) {
    if (impl_->thread_started) { error = "already started"; return false; }
    impl_->config = config;
    if (config.quiet) std::cout.rdbuf(nullptr);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Translated guest code chains through host calls: a deep host stack.
    pthread_attr_setstacksize(&attr, std::size_t{64} << 20);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    const int rc = pthread_create(&impl_->thread, &attr, &Impl::thread_main, impl_.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) { error = "cannot create the game thread (" + std::to_string(rc) + ")"; return false; }
    impl_->thread_started = true;
    return true;
}

void LiveRunner::stop() {
    if (!impl_->thread_started) return;
    impl_->stop_requested.store(true);
    pthread_join(impl_->thread, nullptr);
    impl_->thread_started = false;
}

std::uint64_t LiveRunner::copy_frame(std::uint8_t *rgbx, std::uint64_t seen) {
    std::lock_guard<std::mutex> lock(impl_->frame_mutex);
    if (impl_->frame_serial != seen && impl_->frame_serial != 0u) std::memcpy(rgbx, impl_->frame.data(), impl_->frame.size());
    return impl_->frame_serial;
}

void LiveRunner::pull_audio(std::int16_t *out, std::size_t frames) noexcept { impl_->ring.pull(out, frames); }

void LiveRunner::set_pad(std::uint32_t buttons, std::uint8_t analog_x, std::uint8_t analog_y) noexcept {
    impl_->pad.store(static_cast<std::uint64_t>(buttons) | (static_cast<std::uint64_t>(analog_x) << 32) |
                     (static_cast<std::uint64_t>(analog_y) << 40), std::memory_order_relaxed);
}

void LiveRunner::set_paused(bool paused) noexcept { impl_->paused.store(paused, std::memory_order_relaxed); }

bool LiveRunner::pending_save_dialog(SaveDialog &out) {
    std::lock_guard<std::mutex> lock(impl_->dialog_mutex);
    if (!impl_->dialog_pending) return false;
    out = impl_->dialog;
    return true;
}

void LiveRunner::resolve_save_dialog(int slot) noexcept { impl_->dialog_choice.store(slot < 0 ? -1 : slot); }

Status LiveRunner::status() {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    return impl_->state;
}

std::string LiveRunner::report() {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    return impl_->report_text;
}

} // namespace p3pios
