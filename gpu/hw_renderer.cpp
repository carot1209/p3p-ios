// GE front end for a GPU back end (see hw_renderer.hpp).
#include "hw_renderer.hpp"

#include "psprecomp/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace p3pios::gpu {
namespace {

namespace ge = p3p3ds::ge;

constexpr std::size_t kMaxTargets = 16;
constexpr std::uint32_t kVramBase = 0x04000000u, kVramMask = 0x001FFFFFu;

std::uint32_t r24(const ge::GeRegisters &g, std::uint32_t i) { return g.reg[i] & 0xFFFFFFu; }
bool on(const ge::GeRegisters &g, std::uint32_t i) { return (g.reg[i] & 1u) != 0u; }

// Address inside the 2 MiB VRAM (mirrors folded); false for anything else.
bool vram_address(std::uint32_t address, std::uint32_t &out) {
    const std::uint32_t c = psprecomp::GuestMemory::canonical(address);
    if ((c & 0xFF000000u) != kVramBase) return false;
    out = kVramBase | (c & kVramMask);
    return true;
}

std::uint8_t blend_factor(std::uint32_t f, bool src_side, std::uint32_t fixed, bool &uses_constant) {
    switch (f) {
    case 0: return src_side ? kDstColor : kSrcColor;
    case 1: return src_side ? kInvDstColor : kInvSrcColor;
    case 2: case 6: return kSrcAlpha; // 6-9: doubled alpha, approximated
    case 3: case 7: return kInvSrcAlpha;
    case 4: case 8: return kDstAlpha;
    case 5: case 9: return kInvDstAlpha;
    default:
        if (fixed == 0u) return kZero;
        if (fixed == 0xFFFFFFu) return kOne;
        uses_constant = true;
        return kConstant;
    }
}

std::uint32_t pack(std::uint32_t rgba, std::uint32_t format) {
    const std::uint32_t r = rgba & 0xFFu, g = (rgba >> 8) & 0xFFu, b = (rgba >> 16) & 0xFFu, a = rgba >> 24;
    switch (format) {
    case 0: return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    case 1: return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    case 2: return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
    default: return rgba;
    }
}

} // namespace

HwRenderer::HwRenderer(std::unique_ptr<Backend> backend) : backend_(std::move(backend)) {
    pixels_.resize(static_cast<std::size_t>(kViewWidth) * kViewHeight);
}

HwRenderer::~HwRenderer() = default;

HwRenderer::Target *HwRenderer::target_for(std::uint32_t address, bool create) {
    for (auto &t : targets_)
        if (t.address == address) return &t;
    if (!create || targets_.size() >= kMaxTargets) return nullptr;
    flush_batch();
    Target t;
    t.address = address;
    t.id = next_id_++;
    t.cpu_seq = ++seq_; // guest VRAM holds the current contents until the GE draws
    backend_->create_target(t.id);
    targets_.push_back(t);
    hw_stats_.targets = static_cast<std::uint32_t>(targets_.size());
    return &targets_.back();
}

HwRenderer::Target *HwRenderer::target_containing(std::uint32_t address) {
    for (auto &t : targets_) {
        const std::uint32_t bytes = t.stride * kViewHeight * (t.format == 3u ? 4u : 2u);
        if (address >= t.address && address < t.address + bytes) return &t;
    }
    return nullptr;
}

std::uint32_t HwRenderer::depth_for(std::uint32_t address) {
    const auto it = depths_.find(address);
    if (it != depths_.end()) return it->second;
    flush_batch();
    const std::uint32_t id = next_id_++;
    backend_->create_depth(id);
    depths_.emplace(address, id);
    return id;
}

void HwRenderer::note_cpu_write(std::uint32_t address) {
    std::uint32_t a;
    if (!vram_address(address, a)) return;
    if (Target *t = target_containing(a)) t->cpu_seq = ++seq_;
}

bool HwRenderer::shown_from_cpu(std::uint32_t topaddr) {
    std::uint32_t a;
    if (!vram_address(topaddr, a)) return true;
    const Target *t = target_for(a, false);
    return t == nullptr || t->gpu_seq <= t->cpu_seq;
}

void HwRenderer::upload_guest(psprecomp::GuestMemory &memory, Target &t) {
    const std::uint32_t bpp = t.format == 3u ? 4u : 2u;
    const std::uint8_t *src = t.stride >= kViewWidth ? memory.raw_pointer(t.address, static_cast<std::size_t>(t.stride) * kViewHeight * bpp) : nullptr;
    std::uint32_t *dst = pixels_.data();
    for (std::uint32_t y = 0; y < kViewHeight; ++y)
        for (std::uint32_t x = 0; x < kViewWidth; ++x) {
            std::uint32_t rgba = 0xFF000000u;
            if (src != nullptr) {
                const std::uint8_t *p = src + (static_cast<std::size_t>(y) * t.stride + x) * bpp;
                if (bpp == 4u) rgba = p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000u;
                else {
                    const auto c = ge::unpack16(static_cast<std::uint32_t>(p[0] | (p[1] << 8)), t.format);
                    rgba = c[0] | (c[1] << 8) | (c[2] << 16) | 0xFF000000u;
                }
            }
            *dst++ = rgba;
        }
    backend_->upload_target(t.id, pixels_.data());
    ++hw_stats_.uploads;
}

// GPU picture -> guest VRAM, so a block transfer copies what the GE drew.
void HwRenderer::write_back(psprecomp::GuestMemory &memory, Target &t) {
    const std::uint32_t bpp = t.format == 3u ? 4u : 2u;
    if (t.stride < kViewWidth) return;
    std::uint8_t *dst = memory.raw_pointer(t.address, static_cast<std::size_t>(t.stride) * kViewHeight * bpp);
    if (dst == nullptr || !backend_->read_target(t.id, pixels_.data())) return;
    const std::uint32_t *src = pixels_.data();
    for (std::uint32_t y = 0; y < kViewHeight; ++y)
        for (std::uint32_t x = 0; x < kViewWidth; ++x) {
            const std::uint32_t v = pack(*src++, t.format);
            std::memcpy(dst + (static_cast<std::size_t>(y) * t.stride + x) * bpp, &v, bpp);
        }
    ++hw_stats_.writebacks;
}

void HwRenderer::flush_batch() {
    if (batch_.empty()) return;
    backend_->draw(batch_state_, batch_.data(), static_cast<std::uint32_t>(batch_.size()));
    ++hw_stats_.batches;
    hw_stats_.triangles += batch_.size() / 3u;
    batch_.clear();
}

void HwRenderer::evict_textures() {
    bool flushed = false;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.last_frame + 600u < frame_) {
            if (!flushed) { flush_batch(); flushed = true; }
            backend_->destroy_texture(it->second.id);
            it = textures_.erase(it);
        } else ++it;
    }
    hw_stats_.textures = static_cast<std::uint32_t>(textures_.size());
}

bool HwRenderer::bind_texture(psprecomp::GuestMemory &memory, const ge::GeRegisters &regs, const Target &target, State &state,
                              float &scale_u, float &scale_v) {
    const auto info = ge::texture_info(regs);
    // Render-to-texture: a texture that starts at a GPU-drawn framebuffer.
    std::uint32_t vram;
    if (vram_address(info.address, vram)) {
        for (const auto &t : targets_)
            if (t.address == vram && t.gpu_seq > t.cpu_seq) {
                if (t.id == target.id) return false; // a target cannot be drawn from itself
                state.texture = t.id;
                state.texture_is_target = 1;
                scale_u = scale_v = 1.0f / static_cast<float>(kTargetSize);
                ++hw_stats_.target_textures;
                return true;
            }
    }
    if (info.width > 1024u || info.height > 1024u) return false;

    // Cache key: texture parameters plus, for CLUT formats, the palette.
    std::uint64_t key = (static_cast<std::uint64_t>(info.address) << 32) ^
                        (static_cast<std::uint64_t>(info.format) << 4 | info.clut_format | (info.swizzled ? 0x100u : 0u)) ^
                        (static_cast<std::uint64_t>(info.width) << 12) ^ (static_cast<std::uint64_t>(info.height) << 20) ^
                        (static_cast<std::uint64_t>(info.stride) << 40);
    if (info.format >= 4u) {
        key ^= static_cast<std::uint64_t>(r24(regs, 0xC5)) * 0x9E3779B97F4A7C15ull;
        key = (key ^ regs.clut_hash) * 0x100000001B3ull;
    }
    auto it = textures_.find(key);
    if (it != textures_.end() && it->second.checked_frame == frame_) {
        ++hw_stats_.texture_hits;
    } else {
        const std::uint64_t hash = ge::texture_hash(memory, regs, info);
        if (it != textures_.end() && it->second.hash == hash) {
            it->second.checked_frame = frame_;
            ++hw_stats_.texture_hits;
        } else {
            flush_batch(); // the pending draw may use the old texture
            if (it != textures_.end()) {
                backend_->destroy_texture(it->second.id);
                textures_.erase(it);
            }
            std::vector<std::uint32_t> texels;
            ge::decode_texture(memory, regs, info, texels);
            CachedTexture entry;
            entry.id = next_id_++;
            entry.width = info.width;
            entry.height = info.height;
            entry.hash = hash;
            entry.checked_frame = frame_;
            backend_->create_texture(entry.id, info.width, info.height, texels.data());
            it = textures_.emplace(key, entry).first;
            ++hw_stats_.texture_creates;
            hw_stats_.textures = static_cast<std::uint32_t>(textures_.size());
        }
    }
    it->second.last_frame = frame_;
    state.texture = it->second.id;
    state.texture_is_target = 0;
    scale_u = 1.0f / static_cast<float>(it->second.width);
    scale_v = 1.0f / static_cast<float>(it->second.height);
    return true;
}

void HwRenderer::draw(psprecomp::GuestMemory &memory, const ge::GeRegisters &regs, ge::Prim prim, std::uint32_t count,
                      std::uint32_t vertex_address, std::uint32_t index_address) {
    ++draw_stats_.prims;
    draw_stats_.vertices += count;
    if (prim == ge::Prim::Points || prim == ge::Prim::Lines || prim == ge::Prim::LineStrip) { ++hw_stats_.skipped; return; }
    const std::uint32_t stride = r24(regs, 0x9D) & 0x7FCu;
    if (stride == 0u) return;
    Target *target = target_for(kVramBase | (r24(regs, 0x9C) & 0x1FFFF0u), true);
    if (target == nullptr) { ++hw_stats_.skipped; return; }
    target->format = r24(regs, 0xD2) & 3u;
    target->stride = stride;
    if (target->cpu_seq > target->gpu_seq && target->cpu_seq != target->uploaded_seq) {
        // The guest wrote this framebuffer since the GPU last drew it: start from its contents.
        flush_batch();
        upload_guest(memory, *target);
        target->uploaded_seq = target->cpu_seq;
    }

    const auto layout = ge::vertex_layout(r24(regs, 0x12));
    const std::uint32_t clear = r24(regs, 0xD3);
    const bool clear_mode = (clear & 1u) != 0u;
    if (clear_mode) ++draw_stats_.clears;
    const bool sprites = prim == ge::Prim::Sprites;

    float su = fast_su_, sv = fast_sv_;
    const bool reuse = fast_valid_ && regs.state_version == fast_version_ && frame_ == fast_frame_ && batch_state_.target == target->id;
    if (!reuse) {
        fast_valid_ = false;
        State st;
        std::memset(&st, 0, sizeof(st));
        st.target = target->id;
        st.depth = depth_for(kVramBase | (r24(regs, 0x9E) & 0x1FFFF0u));
        const std::uint32_t sa = r24(regs, 0xD4), sb = r24(regs, 0xD5);
        st.sx1 = static_cast<std::uint16_t>(std::min(sa & 0x3FFu, kTargetSize));
        st.sy1 = static_cast<std::uint16_t>(std::min((sa >> 10) & 0x3FFu, kTargetSize));
        st.sx2 = static_cast<std::uint16_t>(std::min((sb & 0x3FFu) + 1u, kTargetSize));
        st.sy2 = static_cast<std::uint16_t>(std::min(((sb >> 10) & 0x3FFu) + 1u, kTargetSize));
        if (st.sx2 <= st.sx1 || st.sy2 <= st.sy1) { ++hw_stats_.skipped; return; }
        su = sv = 1.0f;
        if (clear_mode) {
            // Clear mode: vertex colour and depth, masks from CLEARMODE bits
            // (0x100 colour, 0x200 alpha/stencil, 0x400 depth); no tests or blending.
            st.write_mask = static_cast<std::uint8_t>(((clear & 0x100u) ? 7u : 0u) | ((clear & 0x200u) ? 8u : 0u));
            st.depth_test = 1;
            st.depth_func = 1;
            st.depth_write = (clear & 0x400u) != 0u;
            if (st.write_mask == 0u && st.depth_write == 0u) { ++hw_stats_.skipped; return; }
        } else {
            if (ge::draw_is_textured(regs, layout)) {
                if (!bind_texture(memory, regs, *target, st, su, sv)) { ++hw_stats_.skipped; return; }
                const std::uint32_t filter = r24(regs, 0xC6), wrap = r24(regs, 0xC7), func = r24(regs, 0xC9);
                st.textured = 1;
                st.linear_min = filter & 1u;
                st.linear_mag = (filter >> 8) & 1u;
                st.clamp_u = wrap & 1u;
                st.clamp_v = (wrap >> 8) & 1u;
                st.tex_func = static_cast<std::uint8_t>(std::min(func & 7u, 4u));
                st.tex_alpha = (func & 0x100u) != 0u;
                st.color_double = (func & 0x10000u) != 0u;
                if (st.tex_func == 2u) st.env_color = r24(regs, 0xCA);
            }
            if (on(regs, 0x22)) {
                const std::uint32_t at = r24(regs, 0xDB);
                st.alpha_test = 1;
                st.alpha_func = at & 7u;
                st.alpha_ref = (at >> 8) & 0xFFu;
                st.alpha_mask = (at >> 16) & 0xFFu;
            }
            if (on(regs, 0x21)) {
                const std::uint32_t mode = r24(regs, 0xDF), fix_a = r24(regs, 0xE0), fix_b = r24(regs, 0xE1);
                bool const_a = false, const_b = false;
                st.blend = 1;
                st.src = blend_factor(mode & 0xFu, true, fix_a, const_a);
                st.dst = blend_factor((mode >> 4) & 0xFu, false, fix_b, const_b);
                // One blend constant: when both sides need one, side A wins (as on the New 3DS).
                if (const_a) st.blend_color = fix_a;
                else if (const_b) st.blend_color = fix_b;
                switch ((mode >> 8) & 0xFu) {
                case 1: st.op = kSubtract; break;
                case 2: st.op = kReverseSubtract; break;
                case 3: st.op = kMin; break;
                case 4: st.op = kMax; break;
                default: st.op = kAdd; break; // 5 (absolute difference) approximated
                }
            }
            // Colour mask (0xE8: set bits keep the old value), per channel. The
            // alpha channel holds stencil on the PSP and is left unchanged, as
            // in the software renderer.
            const std::uint32_t keep = r24(regs, 0xE8);
            if ((keep & 0xFFu) != 0xFFu) st.write_mask |= 1u;
            if (((keep >> 8) & 0xFFu) != 0xFFu) st.write_mask |= 2u;
            if (((keep >> 16) & 0xFFu) != 0xFFu) st.write_mask |= 4u;
            if (on(regs, 0x23)) {
                st.depth_test = 1;
                st.depth_func = r24(regs, 0xDE) & 7u;
                st.depth_write = (r24(regs, 0xE7) & 1u) == 0u;
            }
            if (st.write_mask == 0u && st.depth_write == 0u) { ++hw_stats_.skipped; return; }
        }
        if (std::memcmp(&st, &batch_state_, sizeof(st)) != 0) {
            flush_batch();
            batch_state_ = st;
        }
        fast_valid_ = true;
        fast_version_ = regs.state_version;
        fast_frame_ = frame_;
        fast_su_ = su;
        fast_sv_ = sv;
    }
    target->gpu_seq = ++seq_;
    ++hw_stats_.draws;

    ge::decode_screen_vertices(memory, regs, count, vertex_address, index_address, screen_);
    const bool flat = (r24(regs, 0x50) & 1u) == 0u;
    const bool cull = on(regs, 0x1D) && !clear_mode;
    // Screen-space winding (y down); transform mode keeps the opposite winding
    // to through mode (software_renderer.cpp).
    const bool culled_ccw = ((r24(regs, 0x9B) & 1u) != 0u) == layout.through;
    auto put = [&](float x, float y, float z, float w, float u, float v, const ge::Rgba &c) {
        batch_.push_back(Vertex{x, y, z, w, u * su, v * sv, c[0], c[1], c[2], c[3]});
    };
    auto triangle = [&](const ge::ScreenVertex &a, const ge::ScreenVertex &b, const ge::ScreenVertex &d) {
        if (a.clipped || b.clipped || d.clipped) return;
        const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        if (area == 0.0f) return;
        if (cull && (area > 0.0f) == culled_ccw) return;
        put(a.x, a.y, a.z, a.w, a.u, a.v, flat ? d.color : a.color);
        put(b.x, b.y, b.z, b.w, b.u, b.v, flat ? d.color : b.color);
        put(d.x, d.y, d.z, d.w, d.u, d.v, d.color);
    };
    const auto &v = screen_;
    switch (prim) {
    case ge::Prim::Sprites:
        for (std::uint32_t i = 0; i + 1 < count; i += 2) {
            const auto &a = v[i], &b = v[i + 1];
            // u follows x and v follows y from a to b; colour and depth from b.
            put(a.x, a.y, b.z, 1.0f, a.u, a.v, b.color);
            put(b.x, a.y, b.z, 1.0f, b.u, a.v, b.color);
            put(a.x, b.y, b.z, 1.0f, a.u, b.v, b.color);
            put(a.x, b.y, b.z, 1.0f, a.u, b.v, b.color);
            put(b.x, a.y, b.z, 1.0f, b.u, a.v, b.color);
            put(b.x, b.y, b.z, 1.0f, b.u, b.v, b.color);
        }
        break;
    case ge::Prim::Triangles:
        for (std::uint32_t i = 0; i + 2 < count; i += 3) triangle(v[i], v[i + 1], v[i + 2]);
        break;
    case ge::Prim::TriangleStrip:
        for (std::uint32_t i = 0; i + 2 < count; ++i)
            (i & 1u) ? triangle(v[i + 1], v[i], v[i + 2]) : triangle(v[i], v[i + 1], v[i + 2]);
        break;
    default: // TriangleFan
        for (std::uint32_t i = 1; i + 1 < count; ++i) triangle(v[0], v[i], v[i + 1]);
        break;
    }
    (void)sprites;
    if (batch_.size() >= 60000u) flush_batch();
}

void HwRenderer::transfer(psprecomp::GuestMemory &memory, const ge::GeRegisters &regs) {
    // Block transfers copy guest memory like the reference renderer. A source
    // or destination inside a GPU-drawn framebuffer is first written back to
    // guest memory; the destination then holds newer data than the GPU copy.
    flush_batch();
    ++draw_stats_.transfers;
    const std::uint32_t src = (r24(regs, 0xB2) & 0xFFFFF0u) | ((r24(regs, 0xB3) >> 16) & 0xFFu) << 24;
    const std::uint32_t dst = (r24(regs, 0xB4) & 0xFFFFF0u) | ((r24(regs, 0xB5) >> 16) & 0xFFu) << 24;
    Target *touched[2] = {nullptr, nullptr};
    std::uint32_t a;
    if (vram_address(src, a)) touched[0] = target_containing(a);
    if (vram_address(dst, a)) touched[1] = target_containing(a);
    if (touched[1] == touched[0]) touched[1] = nullptr;
    for (Target *t : touched)
        if (t != nullptr && t->gpu_seq > t->cpu_seq) write_back(memory, *t);
    ge::SoftwareRenderer copy;
    copy.transfer(memory, regs);
    note_cpu_write(dst);
}

bool HwRenderer::present_display(psprecomp::GuestMemory &memory, const p3p3ds::hle::DisplayFramebufInfo &fb) {
    bool shown = false;
    std::uint32_t a;
    if (fb.active && fb.topaddr != 0u && vram_address(fb.topaddr, a)) {
        Target *t = target_for(a, true);
        if (t != nullptr) {
            const bool from_cpu = t->gpu_seq <= t->cpu_seq;
            if (from_cpu) {
                t->format = static_cast<std::uint32_t>(fb.pixelformat);
                t->stride = static_cast<std::uint32_t>(std::max(fb.bufferwidth, 1));
            }
            const std::uint64_t version = std::max(t->gpu_seq, t->cpu_seq);
            if (t->id != shown_present_target_ || version != shown_present_version_) {
                if (from_cpu && t->cpu_seq != t->uploaded_seq) {
                    flush_batch();
                    upload_guest(memory, *t);
                    t->uploaded_seq = t->cpu_seq;
                }
                flush_batch();
                shown = backend_->present(t->id);
                if (shown) {
                    shown_present_target_ = t->id;
                    shown_present_version_ = version;
                    ++hw_stats_.readbacks;
                }
            }
        }
    }
    ++frame_;
    if ((frame_ & 127u) == 0u) evict_textures();
    return shown;
}

bool HwRenderer::read_display(const p3p3ds::hle::DisplayFramebufInfo &fb, std::uint32_t *rgba) {
    bool fresh = false;
    std::uint32_t a;
    if (fb.active && fb.topaddr != 0u && vram_address(fb.topaddr, a)) {
        const Target *t = target_for(a, false);
        if (t != nullptr && t->gpu_seq > t->cpu_seq) {
            if (t->id != shown_target_ || t->gpu_seq != shown_seq_) {
                flush_batch();
                fresh = backend_->read_target(t->id, rgba);
                if (fresh) ++hw_stats_.readbacks;
                shown_target_ = t->id;
                shown_seq_ = t->gpu_seq;
            }
        } else {
            shown_target_ = 0;
        }
    }
    ++frame_;
    if ((frame_ & 127u) == 0u) evict_textures();
    return fresh;
}

} // namespace p3pios::gpu
