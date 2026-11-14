#pragma once
// GPU back end interface for the PSP GE (see hw_renderer.hpp for the front
// end). Everything that crosses it is plain data, so a stream of calls can be
// recorded on one machine and replayed on another (trace.hpp): the Metal back
// end is tested on a macOS runner with frames recorded from the real game.
//
// Coordinates: render targets and depth buffers are 512 x 512, PSP pixel
// (x, y) at texel (x, y) with row 0 at the top; only 480 x 272 is shown.
// Vertices are already transformed: x, y in target pixels, z in 0..65535,
// w the clip w (1 for 2D); u, v normalised texture coordinates. Varyings are
// interpolated perspective-correct with w.
#include <cstdint>

namespace p3pios::gpu {

inline constexpr std::uint32_t kTargetSize = 512, kViewWidth = 480, kViewHeight = 272;

struct Vertex {
    float x, y, z, w;
    float u, v;
    std::uint8_t r, g, b, a;
};
static_assert(sizeof(Vertex) == 28);

enum Factor : std::uint8_t { kZero, kOne, kSrcColor, kInvSrcColor, kDstColor, kInvDstColor, kSrcAlpha, kInvSrcAlpha,
                             kDstAlpha, kInvDstAlpha, kConstant };
enum BlendOp : std::uint8_t { kAdd, kSubtract, kReverseSubtract, kMin, kMax };
// Comparison codes are the GE's: 0 never, 1 always, 2 ==, 3 !=, 4 <, 5 <=, 6 >, 7 >=
// (new value on the left, stored value / reference on the right).

// Everything a draw depends on besides its vertices. No padding: states are
// compared and hashed as bytes.
struct State {
    std::uint32_t target, depth;      // ids from create_target / create_depth
    std::uint32_t texture;            // id from create_texture, or a target id (texture_is_target)
    std::uint32_t env_color;          // texture function 2 (blend): r in the low byte
    std::uint32_t blend_color;        // kConstant factor: r in the low byte
    std::uint16_t sx1, sy1, sx2, sy2; // scissor, x2/y2 exclusive
    std::uint8_t textured, texture_is_target, linear_min, linear_mag, clamp_u, clamp_v;
    std::uint8_t tex_func;            // 0 modulate, 1 decal, 2 blend, 3 replace, 4 add
    std::uint8_t tex_alpha;           // the texture's alpha takes part
    std::uint8_t color_double;
    std::uint8_t alpha_test, alpha_func, alpha_ref, alpha_mask;
    std::uint8_t blend, src, dst, op; // Factor, Factor, BlendOp (colour and alpha alike)
    std::uint8_t write_mask;          // bit 0 r, 1 g, 2 b, 3 a
    std::uint8_t depth_test, depth_func, depth_write;
    std::uint8_t pad[3];
};
static_assert(sizeof(State) == 52);

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual const char *name() const = 0;
    // w x h texels, r in the low byte, top row first. The id is free (never created or destroyed before).
    virtual void create_texture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) = 0;
    virtual void destroy_texture(std::uint32_t id) = 0;
    // Black, opaque colour buffer; depth buffer filled with 0.
    virtual void create_target(std::uint32_t id) = 0;
    virtual void create_depth(std::uint32_t id) = 0;
    // kViewWidth x kViewHeight texels into the top-left corner of a target.
    virtual void upload_target(std::uint32_t id, const std::uint32_t *rgba) = 0;
    // Triangle list (count is a multiple of 3), drawn after everything before it.
    virtual void draw(const State &state, const Vertex *vertices, std::uint32_t count) = 0;
    // The top-left kViewWidth x kViewHeight texels of a target as they are after
    // everything queued so far. False when the pixels cannot be read.
    virtual bool read_target(std::uint32_t id, std::uint32_t *rgba) = 0;
    // Shows the top-left kViewWidth x kViewHeight texels of a target on the back
    // end's display surface, scaled to fit. False when there is none.
    virtual bool present(std::uint32_t /*id*/) { return false; }
    // Hands queued work to the GPU without waiting for it.
    virtual void flush() = 0;
};

} // namespace p3pios::gpu
