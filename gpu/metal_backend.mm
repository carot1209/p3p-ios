// Metal back end for the GE (see gpu_backend.hpp, hw_renderer.hpp).
//
// One command buffer collects everything between two read_target calls. Draws
// to the same target share a render pass; uploads and read-backs are blits in
// the same buffer, so they happen in order. Vertices live in pooled shared
// buffers that return to the pool when their command buffer completes.
// Targets are private RGBA8 512 x 512 textures, depth buffers Depth32Float;
// the shader source is compiled at start.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "metal_backend.hpp"

#include <TargetConditionals.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace p3pios::gpu {
namespace {

const char kShaderSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VIn {
    float4 pos   [[attribute(0)]];
    float2 uv    [[attribute(1)]];
    float4 color [[attribute(2)]];
};
struct VOut {
    float4 position [[position]];
    float2 uv;
    float4 color;
};
struct Uniforms {
    float4 env;
    uint flags;      // 1 textured, 2 colour doubling, 4 texture alpha, 8 alpha test
    uint func;
    uint alpha_func;
    uint alpha_ref;
    uint alpha_mask;
    uint pad0, pad1, pad2;
};

vertex VOut vmain(VIn in [[stage_in]]) {
    VOut out;
    float w = in.pos.w;
    // Target pixels (y down) and depth 0..65535 -> clip space of a 512 x 512 target.
    out.position = float4((in.pos.x * (2.0 / 512.0) - 1.0) * w, (1.0 - in.pos.y * (2.0 / 512.0)) * w,
                          in.pos.z * (1.0 / 65535.0) * w, w);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]],
                      constant Uniforms &u [[buffer(0)]]) {
    float4 c = in.color;
    if ((u.flags & 1u) != 0u) {
        float4 t = tex.sample(smp, in.uv);
        float3 rgb;
        if (u.func == 0u) rgb = c.rgb * t.rgb;
        else if (u.func == 1u) rgb = mix(c.rgb, t.rgb, t.a);
        else if (u.func == 2u) rgb = mix(c.rgb, u.env.rgb, t.rgb);
        else if (u.func == 3u) rgb = t.rgb;
        else rgb = c.rgb + t.rgb;
        if ((u.flags & 2u) != 0u) rgb = rgb * 2.0;
        float a = c.a;
        if (u.func != 1u && (u.flags & 4u) != 0u) a = (u.func == 3u) ? t.a : c.a * t.a;
        c = float4(min(rgb, float3(1.0)), a);
    }
    if ((u.flags & 8u) != 0u) {
        uint a = uint(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5) & u.alpha_mask;
        uint r = u.alpha_ref & u.alpha_mask;
        bool pass = true;
        if (u.alpha_func == 0u) pass = false;
        else if (u.alpha_func == 2u) pass = a == r;
        else if (u.alpha_func == 3u) pass = a != r;
        else if (u.alpha_func == 4u) pass = a < r;
        else if (u.alpha_func == 5u) pass = a <= r;
        else if (u.alpha_func == 6u) pass = a > r;
        else if (u.alpha_func == 7u) pass = a >= r;
        if (!pass) discard_fragment();
    }
    return c;
}
)MSL";

struct Uniforms {
    float env[4];
    std::uint32_t flags, func, alpha_func, alpha_ref, alpha_mask, pad[3];
};
static_assert(sizeof(Uniforms) == 48);

constexpr NSUInteger kVertexBufferBytes = 1u << 20;
constexpr NSUInteger kViewBytes = kViewWidth * kViewHeight * 4u;

// Vertex buffers whose command buffers have completed.
struct BufferPool {
    std::mutex mutex;
    std::vector<id<MTLBuffer>> free;
    id<MTLBuffer> take(NSUInteger bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < free.size(); ++i)
            if (free[i].length >= bytes) {
                id<MTLBuffer> b = free[i];
                free.erase(free.begin() + static_cast<std::ptrdiff_t>(i));
                return b;
            }
        return nil;
    }
    void give(const std::vector<id<MTLBuffer>> &buffers) {
        std::lock_guard<std::mutex> lock(mutex);
        for (id<MTLBuffer> b : buffers)
            if (free.size() < 8u) free.push_back(b);
    }
};

MTLBlendFactor blend_factor(std::uint8_t f) {
    switch (f) {
    case kZero: return MTLBlendFactorZero;
    case kOne: return MTLBlendFactorOne;
    case kSrcColor: return MTLBlendFactorSourceColor;
    case kInvSrcColor: return MTLBlendFactorOneMinusSourceColor;
    case kDstColor: return MTLBlendFactorDestinationColor;
    case kInvDstColor: return MTLBlendFactorOneMinusDestinationColor;
    case kSrcAlpha: return MTLBlendFactorSourceAlpha;
    case kInvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    case kDstAlpha: return MTLBlendFactorDestinationAlpha;
    case kInvDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
    default: return MTLBlendFactorBlendColor;
    }
}

MTLBlendOperation blend_op(std::uint8_t op) {
    switch (op) {
    case kSubtract: return MTLBlendOperationSubtract;
    case kReverseSubtract: return MTLBlendOperationReverseSubtract;
    case kMin: return MTLBlendOperationMin;
    case kMax: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLCompareFunction compare_function(std::uint8_t func) {
    switch (func & 7u) {
    case 0: return MTLCompareFunctionNever;
    case 1: return MTLCompareFunctionAlways;
    case 2: return MTLCompareFunctionEqual;
    case 3: return MTLCompareFunctionNotEqual;
    case 4: return MTLCompareFunctionLess;
    case 5: return MTLCompareFunctionLessEqual;
    case 6: return MTLCompareFunctionGreater;
    default: return MTLCompareFunctionGreaterEqual;
    }
}

std::string describe(NSError *error) {
    if (error == nil) return "unknown error";
    const char *text = error.localizedDescription.UTF8String;
    return text != nullptr ? text : "unknown error";
}

class MetalBackend final : public Backend {
public:
    ~MetalBackend() override {
        @autoreleasepool {
            commit(true);
        }
    }

    bool init(std::string &error) {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
#if TARGET_OS_OSX
            if (device_ == nil) device_ = MTLCopyAllDevices().firstObject; // command-line tools without Core Graphics
#endif
            if (device_ == nil) { error = "no Metal device"; return false; }
            name_ = std::string("Metal, ") + (device_.name.UTF8String != nullptr ? device_.name.UTF8String : "?");
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) { error = "no command queue"; return false; }
            NSError *err = nil;
            id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource] options:nil error:&err];
            if (library == nil) { error = "shader: " + describe(err); return false; }
            vertex_function_ = [library newFunctionWithName:@"vmain"];
            fragment_function_ = [library newFunctionWithName:@"fmain"];
            if (vertex_function_ == nil || fragment_function_ == nil) { error = "shader functions missing"; return false; }

            vertex_descriptor_ = [MTLVertexDescriptor vertexDescriptor];
            vertex_descriptor_.attributes[0].format = MTLVertexFormatFloat4;
            vertex_descriptor_.attributes[0].offset = 0;
            vertex_descriptor_.attributes[0].bufferIndex = 0;
            vertex_descriptor_.attributes[1].format = MTLVertexFormatFloat2;
            vertex_descriptor_.attributes[1].offset = 16;
            vertex_descriptor_.attributes[1].bufferIndex = 0;
            vertex_descriptor_.attributes[2].format = MTLVertexFormatUChar4Normalized;
            vertex_descriptor_.attributes[2].offset = 24;
            vertex_descriptor_.attributes[2].bufferIndex = 0;
            vertex_descriptor_.layouts[0].stride = sizeof(Vertex);
            vertex_descriptor_.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

            for (unsigned i = 0; i < 16u; ++i) {
                MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
                d.minFilter = (i & 1u) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                d.magFilter = (i & 2u) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
                d.mipFilter = MTLSamplerMipFilterNotMipmapped;
                d.sAddressMode = (i & 4u) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                d.tAddressMode = (i & 8u) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
                samplers_[i] = [device_ newSamplerStateWithDescriptor:d];
                if (samplers_[i] == nil) { error = "sampler state"; return false; }
            }
            const std::uint32_t white = 0xFFFFFFFFu;
            white_ = new_texture(1, 1, &white);
            readback_ = [device_ newBufferWithLength:kViewBytes options:MTLResourceStorageModeShared];
            if (white_ == nil || readback_ == nil) { error = "cannot allocate GPU memory"; return false; }
            pool_ = std::make_shared<BufferPool>();
            // Builds the pipeline every 2D draw starts with: a shader that does
            // not fit this GPU fails here instead of in the middle of the game.
            if (pipeline(7u << 12) == nil) { error = "pipeline: " + pipeline_error_; return false; }
            return true;
        }
    }

    const char *name() const override { return name_.c_str(); }

    void create_texture(std::uint32_t ident, std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) override {
        @autoreleasepool {
            id<MTLTexture> t = new_texture(w, h, rgba);
            if (t != nil) textures_[ident] = t;
        }
    }

    void destroy_texture(std::uint32_t ident) override {
        // The command buffer keeps a texture it uses alive until it completes.
        textures_.erase(ident);
    }

    void create_target(std::uint32_t ident) override {
        @autoreleasepool {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:kTargetSize
                                                                                        height:kTargetSize
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
            if (t == nil) return;
            targets_[ident] = t;
            fresh_.insert(ident);
        }
    }

    void create_depth(std::uint32_t ident) override {
        @autoreleasepool {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                         width:kTargetSize
                                                                                        height:kTargetSize
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget;
            d.storageMode = MTLStorageModePrivate;
            id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
            if (t == nil) return;
            depths_[ident] = t;
            fresh_.insert(ident);
        }
    }

    void upload_target(std::uint32_t ident, const std::uint32_t *rgba) override {
        @autoreleasepool {
            const auto it = targets_.find(ident);
            if (it == targets_.end()) return;
            id<MTLTexture> staging = new_texture(kViewWidth, kViewHeight, rgba);
            if (staging == nil) return;
            end_encoder();
            clear_if_fresh(ident);
            id<MTLBlitCommandEncoder> blit = [command_buffer() blitCommandEncoder];
            [blit copyFromTexture:staging
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(kViewWidth, kViewHeight, 1)
                        toTexture:it->second
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        }
    }

    void draw(const State &st, const Vertex *vertices, std::uint32_t count) override {
        @autoreleasepool {
            if (count == 0u) return;
            if (targets_.find(st.target) == targets_.end() || depths_.find(st.depth) == depths_.end()) return;
            id<MTLTexture> texture = white_;
            std::uint64_t texture_key = 0;
            if (st.textured) {
                if (st.texture_is_target) {
                    const auto it = targets_.find(st.texture);
                    if (it == targets_.end() || st.texture == st.target) return;
                    texture = it->second;
                    // A target sampled before anything was drawn into it is black.
                    if (fresh_.count(st.texture) != 0u) { end_encoder(); clear_if_fresh(st.texture); }
                } else {
                    const auto it = textures_.find(st.texture);
                    if (it == textures_.end()) return;
                    texture = it->second;
                }
                texture_key = (static_cast<std::uint64_t>(st.texture_is_target) << 32) | st.texture;
            }
            const std::uint32_t pipeline_key =
                (st.blend ? (1u | (static_cast<std::uint32_t>(st.src) << 1) | (static_cast<std::uint32_t>(st.dst) << 5) |
                             (static_cast<std::uint32_t>(st.op) << 9))
                          : 0u) |
                (static_cast<std::uint32_t>(st.write_mask & 15u) << 12);
            id<MTLRenderPipelineState> pipeline_state = pipeline(pipeline_key);
            const std::uint32_t depth_key =
                st.depth_test ? (1u | (static_cast<std::uint32_t>(st.depth_func & 7u) << 1) | (st.depth_write ? 16u : 0u)) : 0u;
            id<MTLDepthStencilState> depth_state_object = depth_state(depth_key);
            if (pipeline_state == nil || depth_state_object == nil) return;
            const NSUInteger bytes = static_cast<NSUInteger>(count) * sizeof(Vertex);
            if (!reserve_vertices(bytes)) return;

            ensure_encoder(st.target, st.depth);
            if (!bound_.pipeline_valid || bound_.pipeline != pipeline_key) {
                [encoder_ setRenderPipelineState:pipeline_state];
                bound_.pipeline = pipeline_key;
                bound_.pipeline_valid = true;
            }
            if (!bound_.depth_valid || bound_.depth != depth_key) {
                [encoder_ setDepthStencilState:depth_state_object];
                bound_.depth = depth_key;
                bound_.depth_valid = true;
            }
            const std::uint64_t scissor = static_cast<std::uint64_t>(st.sx1) | (static_cast<std::uint64_t>(st.sy1) << 16) |
                                          (static_cast<std::uint64_t>(st.sx2) << 32) | (static_cast<std::uint64_t>(st.sy2) << 48);
            if (!bound_.scissor_valid || bound_.scissor != scissor) {
                MTLScissorRect rect;
                rect.x = std::min<NSUInteger>(st.sx1, kTargetSize - 1u);
                rect.y = std::min<NSUInteger>(st.sy1, kTargetSize - 1u);
                const NSUInteger x2 = std::min<NSUInteger>(std::max<NSUInteger>(st.sx2, rect.x + 1u), kTargetSize);
                const NSUInteger y2 = std::min<NSUInteger>(std::max<NSUInteger>(st.sy2, rect.y + 1u), kTargetSize);
                rect.width = x2 - rect.x;
                rect.height = y2 - rect.y;
                [encoder_ setScissorRect:rect];
                bound_.scissor = scissor;
                bound_.scissor_valid = true;
            }
            if (!bound_.texture_valid || bound_.texture != texture_key) {
                [encoder_ setFragmentTexture:texture atIndex:0];
                bound_.texture = texture_key;
                bound_.texture_valid = true;
            }
            const std::uint32_t sampler = (st.linear_min ? 1u : 0u) | (st.linear_mag ? 2u : 0u) | (st.clamp_u ? 4u : 0u) | (st.clamp_v ? 8u : 0u);
            if (!bound_.sampler_valid || bound_.sampler != sampler) {
                [encoder_ setFragmentSamplerState:samplers_[sampler] atIndex:0];
                bound_.sampler = sampler;
                bound_.sampler_valid = true;
            }
            Uniforms u;
            std::memset(&u, 0, sizeof(u));
            u.env[0] = static_cast<float>(st.env_color & 0xFFu) / 255.0f;
            u.env[1] = static_cast<float>((st.env_color >> 8) & 0xFFu) / 255.0f;
            u.env[2] = static_cast<float>((st.env_color >> 16) & 0xFFu) / 255.0f;
            u.env[3] = 1.0f;
            u.flags = (st.textured ? 1u : 0u) | (st.color_double ? 2u : 0u) | (st.tex_alpha ? 4u : 0u) | (st.alpha_test ? 8u : 0u);
            u.func = st.tex_func;
            u.alpha_func = st.alpha_func;
            u.alpha_ref = st.alpha_ref;
            u.alpha_mask = st.alpha_mask;
            if (!bound_.uniforms_valid || std::memcmp(&u, &bound_.uniforms, sizeof(u)) != 0) {
                [encoder_ setFragmentBytes:&u length:sizeof(u) atIndex:0];
                bound_.uniforms = u;
                bound_.uniforms_valid = true;
            }
            if (st.blend && (!bound_.blend_color_valid || bound_.blend_color != st.blend_color)) {
                [encoder_ setBlendColorRed:static_cast<float>(st.blend_color & 0xFFu) / 255.0f
                                     green:static_cast<float>((st.blend_color >> 8) & 0xFFu) / 255.0f
                                      blue:static_cast<float>((st.blend_color >> 16) & 0xFFu) / 255.0f
                                     alpha:1.0f];
                bound_.blend_color = st.blend_color;
                bound_.blend_color_valid = true;
            }
            if (!bound_.vertex_buffer_valid) {
                [encoder_ setVertexBuffer:vertex_buffer_ offset:0 atIndex:0];
                bound_.vertex_buffer_valid = true;
            }
            std::memcpy(static_cast<std::uint8_t *>(vertex_buffer_.contents) + vertex_used_, vertices, bytes);
            [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:vertex_used_ / sizeof(Vertex) vertexCount:count];
            vertex_used_ += bytes;
        }
    }

    bool read_target(std::uint32_t ident, std::uint32_t *rgba) override {
        @autoreleasepool {
            const auto it = targets_.find(ident);
            if (it == targets_.end()) return false;
            end_encoder();
            clear_if_fresh(ident);
            id<MTLBlitCommandEncoder> blit = [command_buffer() blitCommandEncoder];
            [blit copyFromTexture:it->second
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(kViewWidth, kViewHeight, 1)
                                toBuffer:readback_
                       destinationOffset:0
                  destinationBytesPerRow:kViewWidth * 4u
                destinationBytesPerImage:kViewBytes];
            [blit endEncoding];
            if (!commit(true)) return false;
            std::memcpy(rgba, readback_.contents, kViewBytes);
            return true;
        }
    }

    void flush() override {
        @autoreleasepool {
            commit(false);
        }
    }

private:
    struct Bound {
        bool pipeline_valid{}, depth_valid{}, scissor_valid{}, texture_valid{}, sampler_valid{}, uniforms_valid{},
            blend_color_valid{}, vertex_buffer_valid{};
        std::uint32_t pipeline{}, depth{}, sampler{}, blend_color{};
        std::uint64_t scissor{}, texture{};
        Uniforms uniforms{};
    };

    id<MTLTexture> new_texture(std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) {
        if (w == 0u || h == 0u) return nil;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [device_ newTextureWithDescriptor:d];
        if (t == nil) return nil;
        [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:static_cast<NSUInteger>(w) * 4u];
        return t;
    }

    id<MTLCommandBuffer> command_buffer() {
        if (command_buffer_ == nil) command_buffer_ = [queue_ commandBuffer];
        return command_buffer_;
    }

    void end_encoder() {
        if (encoder_ == nil) return;
        [encoder_ endEncoding];
        encoder_ = nil;
    }

    // Submits everything queued; with `wait`, returns whether the GPU finished it without an error.
    bool commit(bool wait) {
        end_encoder();
        if (command_buffer_ == nil) return true;
        id<MTLCommandBuffer> buffer = command_buffer_;
        command_buffer_ = nil;
        if (!used_buffers_.empty()) {
            std::shared_ptr<BufferPool> pool = pool_;
            std::vector<id<MTLBuffer>> used = std::move(used_buffers_);
            used_buffers_.clear();
            [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
              pool->give(used);
            }];
        }
        vertex_buffer_ = nil;
        vertex_used_ = 0;
        [buffer commit];
        if (!wait) return true;
        [buffer waitUntilCompleted];
        return buffer.status == MTLCommandBufferStatusCompleted;
    }

    bool reserve_vertices(NSUInteger bytes) {
        if (vertex_buffer_ != nil && vertex_used_ + bytes <= vertex_buffer_.length) return true;
        const NSUInteger wanted = std::max<NSUInteger>(bytes, kVertexBufferBytes);
        id<MTLBuffer> b = pool_->take(wanted);
        if (b == nil) b = [device_ newBufferWithLength:wanted options:MTLResourceStorageModeShared];
        if (b == nil) return false;
        used_buffers_.push_back(b);
        vertex_buffer_ = b;
        vertex_used_ = 0;
        bound_.vertex_buffer_valid = false;
        return true;
    }

    MTLRenderPassDescriptor *pass_descriptor(std::uint32_t target, std::uint32_t depth, bool with_depth) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = targets_[target];
        pass.colorAttachments[0].loadAction = fresh_.erase(target) != 0u ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (with_depth) {
            pass.depthAttachment.texture = depths_[depth];
            pass.depthAttachment.loadAction = fresh_.erase(depth) != 0u ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.clearDepth = 0.0;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
        }
        return pass;
    }

    // A target nothing was drawn into yet holds undefined memory: make it black.
    void clear_if_fresh(std::uint32_t target) {
        if (fresh_.count(target) == 0u) return;
        id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:pass_descriptor(target, 0, false)];
        [e endEncoding];
    }

    void ensure_encoder(std::uint32_t target, std::uint32_t depth) {
        if (encoder_ != nil && encoder_target_ == target && encoder_depth_ == depth) return;
        end_encoder();
        encoder_ = [command_buffer() renderCommandEncoderWithDescriptor:pass_descriptor(target, depth, true)];
        encoder_target_ = target;
        encoder_depth_ = depth;
        [encoder_ setDepthClipMode:MTLDepthClipModeClamp];
        [encoder_ setCullMode:MTLCullModeNone];
        bound_ = Bound{};
    }

    id<MTLRenderPipelineState> pipeline(std::uint32_t key) {
        const auto it = pipelines_.find(key);
        if (it != pipelines_.end()) return it->second;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vertex_function_;
        d.fragmentFunction = fragment_function_;
        d.vertexDescriptor = vertex_descriptor_;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
        c.pixelFormat = MTLPixelFormatRGBA8Unorm;
        const std::uint32_t mask = (key >> 12) & 15u;
        MTLColorWriteMask write = MTLColorWriteMaskNone;
        if (mask & 1u) write |= MTLColorWriteMaskRed;
        if (mask & 2u) write |= MTLColorWriteMaskGreen;
        if (mask & 4u) write |= MTLColorWriteMaskBlue;
        if (mask & 8u) write |= MTLColorWriteMaskAlpha;
        c.writeMask = write;
        if (key & 1u) {
            const MTLBlendFactor src = blend_factor(static_cast<std::uint8_t>((key >> 1) & 15u));
            const MTLBlendFactor dst = blend_factor(static_cast<std::uint8_t>((key >> 5) & 15u));
            const MTLBlendOperation op = blend_op(static_cast<std::uint8_t>((key >> 9) & 7u));
            c.blendingEnabled = YES;
            c.rgbBlendOperation = op;
            c.alphaBlendOperation = op;
            c.sourceRGBBlendFactor = src;
            c.sourceAlphaBlendFactor = src;
            c.destinationRGBBlendFactor = dst;
            c.destinationAlphaBlendFactor = dst;
        }
        NSError *err = nil;
        id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&err];
        if (state == nil) pipeline_error_ = describe(err);
        pipelines_[key] = state; // nil is remembered too: the same failure is not retried per draw
        return state;
    }

    id<MTLDepthStencilState> depth_state(std::uint32_t key) {
        const auto it = depth_states_.find(key);
        if (it != depth_states_.end()) return it->second;
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = (key & 1u) ? compare_function(static_cast<std::uint8_t>((key >> 1) & 7u)) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = (key & 1u) != 0u && (key & 16u) != 0u;
        id<MTLDepthStencilState> state = [device_ newDepthStencilStateWithDescriptor:d];
        depth_states_[key] = state;
        return state;
    }

    std::string name_{"Metal"}, pipeline_error_;
    id<MTLDevice> device_{nil};
    id<MTLCommandQueue> queue_{nil};
    id<MTLFunction> vertex_function_{nil}, fragment_function_{nil};
    MTLVertexDescriptor *vertex_descriptor_{nil};
    id<MTLSamplerState> samplers_[16]{};
    id<MTLTexture> white_{nil};
    id<MTLBuffer> readback_{nil};
    std::unordered_map<std::uint32_t, id<MTLTexture>> textures_, targets_, depths_;
    std::unordered_set<std::uint32_t> fresh_; // targets and depth buffers no pass has loaded yet
    std::unordered_map<std::uint32_t, id<MTLRenderPipelineState>> pipelines_;
    std::unordered_map<std::uint32_t, id<MTLDepthStencilState>> depth_states_;
    id<MTLCommandBuffer> command_buffer_{nil};
    id<MTLRenderCommandEncoder> encoder_{nil};
    std::uint32_t encoder_target_{}, encoder_depth_{};
    Bound bound_;
    std::shared_ptr<BufferPool> pool_;
    std::vector<id<MTLBuffer>> used_buffers_;
    id<MTLBuffer> vertex_buffer_{nil};
    NSUInteger vertex_used_{0};
};

} // namespace

std::unique_ptr<Backend> make_metal_backend(std::string &error) {
    auto backend = std::make_unique<MetalBackend>();
    if (!backend->init(error)) return nullptr;
    return backend;
}

} // namespace p3pios::gpu
