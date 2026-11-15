#pragma once
// Recording and replay of gpu::Backend calls. A trace is a sequence of
// self-contained windows: each starts with a reset record followed by every
// live texture and the current contents of every target, then the calls as
// they happened. Replaying a window on another back end must give the same
// pictures at each read_target record.
//
// File: records of {u32 op, u32 bytes} + payload, little endian.
#include "gpu_backend.hpp"

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace p3pios::gpu {

class TraceBackend final : public Backend {
public:
    explicit TraceBackend(std::unique_ptr<Backend> inner);
    ~TraceBackend() override;
    // Appends a window to `path`, ending after `frames` read_target calls.
    bool record(const std::string &path, std::uint32_t frames);
    [[nodiscard]] bool recording() const { return file_ != nullptr; }

    const char *name() const override { return inner_->name(); }
    void create_texture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) override;
    void destroy_texture(std::uint32_t id) override;
    void create_target(std::uint32_t id) override;
    void create_depth(std::uint32_t id) override;
    void upload_target(std::uint32_t id, const std::uint32_t *rgba) override;
    void draw(const State &state, const Vertex *vertices, std::uint32_t count) override;
    bool read_target(std::uint32_t id, std::uint32_t *rgba) override;
    void flush() override;
    // While recording, a presented picture is also a read_target record, so
    // traces of a presenting back end replay like the others.
    bool present(std::uint32_t id) override {
        if (recording()) { std::vector<std::uint32_t> scratch(kViewWidth * kViewHeight); read_target(id, scratch.data()); }
        return inner_->present(id);
    }
    bool present_picture(std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba, double aspect) override {
        return inner_->present_picture(w, h, rgba, aspect);
    }

private:
    struct Texture { std::uint32_t w{}, h{}; std::vector<std::uint32_t> px; };
    void put(std::uint32_t op, const void *a, std::size_t a_bytes, const void *b = nullptr, std::size_t b_bytes = 0);
    std::unique_ptr<Backend> inner_;
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::vector<std::uint32_t> targets_, depths_;
    std::FILE *file_{};
    std::uint32_t frames_left_{};
};

// Replays `path` on `backend`; `frame(window, index, rgba)` gets every picture
// read (kViewWidth x kViewHeight). Returns the number of pictures, -1 on a bad file.
int replay_trace(const std::string &path, Backend &backend,
                 const std::function<void(std::uint32_t window, std::uint32_t index, const std::uint32_t *rgba)> &frame);

} // namespace p3pios::gpu
