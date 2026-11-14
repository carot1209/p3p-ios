#pragma once
// GE renderer that draws with a GPU (gpu_backend.hpp) instead of rasterizing
// on the CPU. Portable: vertex decoding, CPU transform and lighting and texel
// decoding come from upstream core/src/ge/geometry.cpp, the same code the
// software renderer uses; this class turns each GE draw into screen-space
// triangles plus a State and tracks framebuffers and textures.
//
// Same design as upstream's New 3DS renderer (platform/3ds/gpu_renderer.cpp):
// one GPU target per PSP framebuffer address; a framebuffer the CPU wrote
// since the GPU last drew it (movie frames, block transfers) is uploaded
// before the next draw; a texture that starts at a GPU-drawn framebuffer
// samples the target. Known differences from the software renderer: points
// and lines are not drawn, colour masks are per channel, doubled-alpha blend
// factors and the absolute-difference blend equation are approximated, and
// GPU pixels reach guest memory only for block transfers.
#include "gpu_backend.hpp"

#include "p3p3ds/ge/geometry.hpp"
#include "p3p3ds/ge/renderer.hpp"
#include "p3p3ds/hle/display.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace p3pios::gpu {

struct HwStats {
    std::uint64_t draws{}, batches{}, triangles{}, skipped{}, uploads{}, readbacks{}, writebacks{};
    std::uint64_t texture_creates{}, texture_hits{}, target_textures{};
    std::uint32_t textures{}, targets{};
};

class HwRenderer final : public p3p3ds::ge::GeRenderer {
public:
    explicit HwRenderer(std::unique_ptr<Backend> backend);
    ~HwRenderer() override;

    void draw(psprecomp::GuestMemory &memory, const p3p3ds::ge::GeRegisters &regs, p3p3ds::ge::Prim prim, std::uint32_t count,
              std::uint32_t vertex_address, std::uint32_t index_address) override;
    void transfer(psprecomp::GuestMemory &memory, const p3p3ds::ge::GeRegisters &regs) override;
    [[nodiscard]] const p3p3ds::ge::DrawStats &stats() const override { return draw_stats_; }

    // CPU store into guest VRAM.
    void note_cpu_write(std::uint32_t address);
    // The displayed picture was written by the CPU last (or never drawn by the GE).
    [[nodiscard]] bool shown_from_cpu(std::uint32_t topaddr);
    // Once per vblank. When the displayed framebuffer is a GPU picture that
    // differs from the one returned last, copies it to `rgba` (kViewWidth x
    // kViewHeight, bytes R,G,B,A) and returns true.
    bool read_display(const p3p3ds::hle::DisplayFramebufInfo &fb, std::uint32_t *rgba);

    // Once per vblank: shows the displayed framebuffer on the back end's display
    // surface (a CPU-written picture is uploaded first). True when a new picture
    // was shown; false when it is the one already there or there is no surface.
    bool present_display(psprecomp::GuestMemory &memory, const p3p3ds::hle::DisplayFramebufInfo &fb);

    [[nodiscard]] const HwStats &hw_stats() const { return hw_stats_; }
    [[nodiscard]] Backend &backend() { return *backend_; }

private:
    struct Target {
        std::uint32_t address{}, format{}, stride{512}, id{};
        std::uint64_t gpu_seq{}, cpu_seq{}, uploaded_seq{};
    };
    struct CachedTexture {
        std::uint32_t id{}, width{}, height{};
        std::uint64_t hash{}, last_frame{}, checked_frame{};
    };

    Target *target_for(std::uint32_t address, bool create);
    Target *target_containing(std::uint32_t address);
    std::uint32_t depth_for(std::uint32_t address);
    void upload_guest(psprecomp::GuestMemory &memory, Target &t);
    void write_back(psprecomp::GuestMemory &memory, Target &t);
    bool bind_texture(psprecomp::GuestMemory &memory, const p3p3ds::ge::GeRegisters &regs, const Target &target, State &state,
                      float &scale_u, float &scale_v);
    void flush_batch();
    void evict_textures();

    std::unique_ptr<Backend> backend_;
    p3p3ds::ge::DrawStats draw_stats_;
    HwStats hw_stats_;
    std::vector<Target> targets_;
    std::unordered_map<std::uint32_t, std::uint32_t> depths_;
    std::unordered_map<std::uint64_t, CachedTexture> textures_;
    std::uint32_t next_id_{1};
    std::uint64_t seq_{0}, frame_{1};
    // Consecutive draws with the same State become one back-end draw.
    State batch_state_{};
    std::vector<Vertex> batch_;
    // The draw the batch state was built for: an unchanged GE state version
    // and target reuse it without looking anything up.
    bool fast_valid_{};
    std::uint64_t fast_version_{}, fast_frame_{};
    float fast_su_{1.0f}, fast_sv_{1.0f};
    std::uint32_t fast_texture_key_{};
    std::vector<p3p3ds::ge::ScreenVertex> screen_;
    std::vector<std::uint32_t> pixels_;
    std::uint32_t shown_target_{}, shown_present_target_{};
    std::uint64_t shown_present_version_{~0ull};
    std::uint64_t shown_seq_{~0ull};
};

} // namespace p3pios::gpu
