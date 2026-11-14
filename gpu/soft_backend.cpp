// Reference implementation of gpu::Backend on the CPU. Not for playing: it
// exists so the GE front end (hw_renderer.cpp) and recorded traces can be
// checked on a machine without a GPU, and so a Metal picture has something
// to be compared with. It follows GPU rules: top-left fill rule, pixel-centre
// sampling, perspective-correct varyings.
#include "soft_backend.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace p3pios::gpu {
namespace {

struct Color { float r, g, b, a; };

Color unpack(std::uint32_t c) {
    return {(c & 0xFFu) / 255.0f, ((c >> 8) & 0xFFu) / 255.0f, ((c >> 16) & 0xFFu) / 255.0f, (c >> 24) / 255.0f};
}
std::uint32_t to8(float f) { return static_cast<std::uint32_t>(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); }
std::uint32_t pack(const Color &c) { return to8(c.r) | (to8(c.g) << 8) | (to8(c.b) << 16) | (to8(c.a) << 24); }

bool compare(std::uint32_t func, float a, float b) {
    switch (func & 7u) {
    case 0: return false; case 1: return true; case 2: return a == b; case 3: return a != b;
    case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b;
    }
}

float factor(std::uint8_t f, float s, float d, float sa, float da, float k) {
    switch (f) {
    case kZero: return 0.0f; case kOne: return 1.0f;
    case kSrcColor: return s; case kInvSrcColor: return 1.0f - s;
    case kDstColor: return d; case kInvDstColor: return 1.0f - d;
    case kSrcAlpha: return sa; case kInvSrcAlpha: return 1.0f - sa;
    case kDstAlpha: return da; case kInvDstAlpha: return 1.0f - da;
    default: return k;
    }
}

float blend_channel(const State &st, float s, float d, float sa, float da, float k) {
    const float a = s * factor(st.src, s, d, sa, da, k), b = d * factor(st.dst, s, d, sa, da, k);
    switch (st.op) {
    case kSubtract: return a - b;
    case kReverseSubtract: return b - a;
    case kMin: return std::min(s, d);
    case kMax: return std::max(s, d);
    default: return a + b;
    }
}

struct Image {
    std::uint32_t w{}, h{};
    std::vector<std::uint32_t> px;
};

class SoftBackend final : public Backend {
public:
    const char *name() const override { return "soft"; }
    void create_texture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) override {
        Image &t = textures_[id];
        t.w = w;
        t.h = h;
        t.px.assign(rgba, rgba + static_cast<std::size_t>(w) * h);
    }
    void destroy_texture(std::uint32_t id) override { textures_.erase(id); }
    void create_target(std::uint32_t id) override {
        Image &t = targets_[id];
        t.w = t.h = kTargetSize;
        t.px.assign(static_cast<std::size_t>(kTargetSize) * kTargetSize, 0xFF000000u);
    }
    void create_depth(std::uint32_t id) override { depths_[id].assign(static_cast<std::size_t>(kTargetSize) * kTargetSize, 0.0f); }
    void upload_target(std::uint32_t id, const std::uint32_t *rgba) override {
        const auto it = targets_.find(id);
        if (it == targets_.end()) return;
        for (std::uint32_t y = 0; y < kViewHeight; ++y)
            std::memcpy(&it->second.px[static_cast<std::size_t>(y) * kTargetSize], rgba + static_cast<std::size_t>(y) * kViewWidth, kViewWidth * 4u);
    }
    bool read_target(std::uint32_t id, std::uint32_t *rgba) override {
        const auto it = targets_.find(id);
        if (it == targets_.end()) return false;
        for (std::uint32_t y = 0; y < kViewHeight; ++y)
            std::memcpy(rgba + static_cast<std::size_t>(y) * kViewWidth, &it->second.px[static_cast<std::size_t>(y) * kTargetSize], kViewWidth * 4u);
        return true;
    }
    void flush() override {}

    void draw(const State &st, const Vertex *v, std::uint32_t count) override {
        const auto target = targets_.find(st.target);
        const auto depth = depths_.find(st.depth);
        if (target == targets_.end() || depth == depths_.end()) return;
        const Image *tex = nullptr;
        if (st.textured) {
            if (st.texture_is_target) {
                const auto it = targets_.find(st.texture);
                if (it != targets_.end() && it != target) tex = &it->second;
            } else {
                const auto it = textures_.find(st.texture);
                if (it != textures_.end()) tex = &it->second;
            }
            if (tex == nullptr) return;
        }
        for (std::uint32_t i = 0; i + 2 < count; i += 3) triangle(st, target->second, depth->second, tex, v[i], v[i + 1], v[i + 2]);
    }

private:
    static Color texel(const State &st, const Image &t, int x, int y) {
        const int w = static_cast<int>(t.w), h = static_cast<int>(t.h);
        x = st.clamp_u ? std::clamp(x, 0, w - 1) : ((x % w) + w) % w;
        y = st.clamp_v ? std::clamp(y, 0, h - 1) : ((y % h) + h) % h;
        return unpack(t.px[static_cast<std::size_t>(y) * t.w + static_cast<std::size_t>(x)]);
    }
    static Color sample(const State &st, const Image &t, float u, float v) {
        const float x = u * static_cast<float>(t.w), y = v * static_cast<float>(t.h);
        if (!st.linear_mag) return texel(st, t, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
        const float fx = x - 0.5f, fy = y - 0.5f;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float ax = fx - static_cast<float>(x0), ay = fy - static_cast<float>(y0);
        const Color a = texel(st, t, x0, y0), b = texel(st, t, x0 + 1, y0), c = texel(st, t, x0, y0 + 1), d = texel(st, t, x0 + 1, y0 + 1);
        auto mix = [&](float p, float q, float r, float s) { return (p * (1 - ax) + q * ax) * (1 - ay) + (r * (1 - ax) + s * ax) * ay; };
        return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
    }

    static void triangle(const State &st, Image &target, std::vector<float> &depth, const Image *tex, const Vertex &a, const Vertex &b,
                         const Vertex &c) {
        if (!(a.w > 0.0f) || !(b.w > 0.0f) || !(c.w > 0.0f)) return;
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (area == 0.0f || std::isnan(area)) return;
        const Vertex *p0 = &a, *p1 = &b, *p2 = &c;
        if (area < 0.0f) { std::swap(p1, p2); area = -area; } // edges below assume one winding
        const int minx = std::max<int>(st.sx1, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int maxx = std::min<int>(st.sx2 - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int miny = std::max<int>(st.sy1, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int maxy = std::min<int>(st.sy2 - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        // Edge function of (q -> r) at p, and whether that edge owns pixels exactly on it (top-left rule, y down).
        auto edge = [](const Vertex *q, const Vertex *r, float px, float py) { return (r->x - q->x) * (py - q->y) - (r->y - q->y) * (px - q->x); };
        auto owns = [](const Vertex *q, const Vertex *r) {
            const float dx = r->x - q->x, dy = r->y - q->y;
            return (dy == 0.0f && dx > 0.0f) || dy < 0.0f;
        };
        const bool o0 = owns(p1, p2), o1 = owns(p2, p0), o2 = owns(p0, p1);
        const Color c0 = {p0->r / 255.0f, p0->g / 255.0f, p0->b / 255.0f, p0->a / 255.0f};
        const Color c1 = {p1->r / 255.0f, p1->g / 255.0f, p1->b / 255.0f, p1->a / 255.0f};
        const Color c2 = {p2->r / 255.0f, p2->g / 255.0f, p2->b / 255.0f, p2->a / 255.0f};
        const Color env = unpack(st.env_color | 0xFF000000u), konst = unpack(st.blend_color | 0xFF000000u);
        for (int y = miny; y <= maxy; ++y)
            for (int x = minx; x <= maxx; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float e0 = edge(p1, p2, px, py), e1 = edge(p2, p0, px, py), e2 = edge(p0, p1, px, py);
                if (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) continue;
                if ((e0 == 0.0f && !o0) || (e1 == 0.0f && !o1) || (e2 == 0.0f && !o2)) continue;
                const float l0 = e0 / area, l1 = e1 / area, l2 = e2 / area;
                const float q0 = l0 / p0->w, q1 = l1 / p1->w, q2 = l2 / p2->w, iq = 1.0f / (q0 + q1 + q2);
                const float k0 = q0 * iq, k1 = q1 * iq, k2 = q2 * iq;
                Color col = {k0 * c0.r + k1 * c1.r + k2 * c2.r, k0 * c0.g + k1 * c1.g + k2 * c2.g, k0 * c0.b + k1 * c1.b + k2 * c2.b,
                             k0 * c0.a + k1 * c1.a + k2 * c2.a};
                if (tex != nullptr) {
                    const Color t = sample(st, *tex, k0 * p0->u + k1 * p1->u + k2 * p2->u, k0 * p0->v + k1 * p1->v + k2 * p2->v);
                    Color out = col;
                    switch (st.tex_func) {
                    case 0: out.r = col.r * t.r; out.g = col.g * t.g; out.b = col.b * t.b; break;
                    case 1: out.r = col.r + (t.r - col.r) * t.a; out.g = col.g + (t.g - col.g) * t.a; out.b = col.b + (t.b - col.b) * t.a; break;
                    case 2: out.r = col.r + (env.r - col.r) * t.r; out.g = col.g + (env.g - col.g) * t.g; out.b = col.b + (env.b - col.b) * t.b; break;
                    case 3: out.r = t.r; out.g = t.g; out.b = t.b; break;
                    default: out.r = col.r + t.r; out.g = col.g + t.g; out.b = col.b + t.b; break;
                    }
                    if (st.color_double) { out.r *= 2.0f; out.g *= 2.0f; out.b *= 2.0f; }
                    if (st.tex_func != 1u && st.tex_alpha) out.a = st.tex_func == 3u ? t.a : col.a * t.a;
                    col = {std::min(out.r, 1.0f), std::min(out.g, 1.0f), std::min(out.b, 1.0f), out.a};
                }
                if (st.alpha_test) {
                    const std::uint32_t av = to8(col.a) & st.alpha_mask, ref = static_cast<std::uint32_t>(st.alpha_ref) & st.alpha_mask;
                    if (!compare(st.alpha_func, static_cast<float>(av), static_cast<float>(ref))) continue;
                }
                const std::size_t at = static_cast<std::size_t>(y) * kTargetSize + static_cast<std::size_t>(x);
                const float z = std::clamp((l0 * p0->z + l1 * p1->z + l2 * p2->z) / 65535.0f, 0.0f, 1.0f);
                if (st.depth_test && !compare(st.depth_func, z, depth[at])) continue;
                const std::uint32_t old = target.px[at];
                if (st.blend) {
                    const Color d = unpack(old);
                    col = {blend_channel(st, col.r, d.r, col.a, d.a, konst.r), blend_channel(st, col.g, d.g, col.a, d.a, konst.g),
                           blend_channel(st, col.b, d.b, col.a, d.a, konst.b), col.a};
                }
                std::uint32_t mask = 0;
                if (st.write_mask & 1u) mask |= 0x000000FFu;
                if (st.write_mask & 2u) mask |= 0x0000FF00u;
                if (st.write_mask & 4u) mask |= 0x00FF0000u;
                if (st.write_mask & 8u) mask |= 0xFF000000u;
                target.px[at] = (pack(col) & mask) | (old & ~mask);
                if (st.depth_test && st.depth_write) depth[at] = z;
            }
    }

    std::unordered_map<std::uint32_t, Image> textures_, targets_;
    std::unordered_map<std::uint32_t, std::vector<float>> depths_;
};

} // namespace

std::unique_ptr<Backend> make_soft_backend() { return std::make_unique<SoftBackend>(); }

} // namespace p3pios::gpu
