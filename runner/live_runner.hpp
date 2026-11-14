#pragma once
// Live (real-time) runner for the P3P static recompilation on a host with a
// screen, speakers and a touch pad. Portable C++: the game runs on its own
// thread; the host UI pulls video frames and audio and pushes controller
// state. Used by the iOS shell (ios/main.mm) and by the headless host test
// (runner/host_test.cpp), so everything except the UI code is exercised off
// the device.
#include "gpu_backend.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace p3pios {

inline constexpr std::uint32_t kScreenWidth = 480, kScreenHeight = 272;
inline constexpr std::uint32_t kAudioRate = 44100;

// PSP button bits (same values as p3p3ds::input::button).
namespace button {
inline constexpr std::uint32_t Select = 0x0001u, Start = 0x0008u, Up = 0x0010u, Right = 0x0020u, Down = 0x0040u,
                               Left = 0x0080u, L = 0x0100u, R = 0x0200u, Triangle = 0x1000u, Circle = 0x2000u,
                               Cross = 0x4000u, Square = 0x8000u;
}

struct Config {
    std::string elf_path;    // decrypted EBOOT the AOT code was generated from
    std::string iso_path;    // the user's ULUS-10512 image
    std::string data_dir;    // writable: ms0/ (saves), mods/, report.txt
    std::string build_id;
    bool skip_movies{true};  // press START for the game while a movie plays (no video decoder yet)
    bool quiet{true};        // discard the runtime's std::cout diagnostics
    // GPU renderer: called once on the game thread. Without it, or when it
    // returns nullptr (`error` says why), the CPU renderer draws.
    std::function<std::unique_ptr<gpu::Backend>(std::string &error)> make_gpu_backend;
    // The GPU back end shows pictures itself (Backend::present) instead of the
    // host copying frames out with copy_frame().
    bool gpu_presents{false};
    // Test hook: at each of these guest times (seconds) append a window of
    // `trace_frames` pictures of GPU back-end calls to `trace_path` (gpu/trace.hpp).
    std::string trace_path;
    std::vector<double> trace_at;
    std::uint32_t trace_frames{3};
};

struct SaveSlot {
    bool exists{};
    std::string text; // one line: the save's detail, or empty
};
struct SaveDialog {
    enum class Kind { Load, Save, Delete } kind{Kind::Load};
    std::vector<SaveSlot> slots;
    int suggested{};          // newest existing save, else 0
    std::uint64_t serial{};   // changes for every new dialog
};

struct Status {
    bool running{}, finished{};
    double fps{}, speed{};    // game frames per second; guest time / wall time over the last second
    std::uint64_t vblanks{}, audio_underruns{};
    std::string stage, stop_reason, renderer;
};

class LiveRunner {
public:
    LiveRunner();
    ~LiveRunner();
    LiveRunner(const LiveRunner &) = delete;
    LiveRunner &operator=(const LiveRunner &) = delete;

    // Starts the game thread. Returns false (and `error`) when it cannot start.
    bool start(const Config &config, std::string &error);
    // Asks the game thread to end and waits for it.
    void stop();

    // Video: copies the newest frame (kScreenWidth x kScreenHeight, bytes R,G,B,X) when
    // its serial differs from `seen`; returns the frame's serial (0: none yet).
    // `rgbx` may be null: only the serial is wanted (GPU presenting).
    std::uint64_t copy_frame(std::uint8_t *rgbx, std::uint64_t seen);
    // Audio: `frames` interleaved stereo S16 frames at kAudioRate. Silence on underrun.
    // Safe to call from a real-time audio thread (no locks, no allocation).
    void pull_audio(std::int16_t *out, std::size_t frames) noexcept;
    // Controller: PSP button bits, analog 0..255 (128 = centre).
    void set_pad(std::uint32_t buttons, std::uint8_t analog_x = 128, std::uint8_t analog_y = 128) noexcept;
    // While paused the guest clock stands still (app in the background).
    void set_paused(bool paused) noexcept;

    // Save/load list: true when a dialog is waiting; fills `out`.
    bool pending_save_dialog(SaveDialog &out);
    // Choice for the waiting dialog: slot index, or -1 to cancel.
    void resolve_save_dialog(int slot) noexcept;

    Status status();
    // The text also written to <data_dir>/report.txt.
    std::string report();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p3pios
