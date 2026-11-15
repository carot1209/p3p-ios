#include "trace.hpp"

#include <algorithm>
#include <cstring>

namespace p3pios::gpu {
namespace {
enum Op : std::uint32_t { kReset = 1, kCreateTexture, kDestroyTexture, kCreateTarget, kCreateDepth, kUpload, kDraw, kRead, kFlush };
constexpr std::size_t kViewPixels = static_cast<std::size_t>(kViewWidth) * kViewHeight;
} // namespace

TraceBackend::TraceBackend(std::unique_ptr<Backend> inner) : inner_(std::move(inner)) {}
TraceBackend::~TraceBackend() {
    if (file_ != nullptr) std::fclose(file_);
}

void TraceBackend::put(std::uint32_t op, const void *a, std::size_t a_bytes, const void *b, std::size_t b_bytes) {
    if (file_ == nullptr) return;
    const std::uint32_t header[2] = {op, static_cast<std::uint32_t>(a_bytes + b_bytes)};
    std::fwrite(header, 4, 2, file_);
    if (a_bytes != 0u) std::fwrite(a, 1, a_bytes, file_);
    if (b_bytes != 0u) std::fwrite(b, 1, b_bytes, file_);
}

bool TraceBackend::record(const std::string &path, std::uint32_t frames) {
    if (file_ != nullptr || frames == 0u) return false;
    file_ = std::fopen(path.c_str(), "ab");
    if (file_ == nullptr) return false;
    frames_left_ = frames;
    put(kReset, nullptr, 0);
    for (const auto &[id, t] : textures_) {
        const std::uint32_t head[3] = {id, t.w, t.h};
        put(kCreateTexture, head, sizeof(head), t.px.data(), t.px.size() * 4u);
    }
    std::vector<std::uint32_t> pixels(kViewPixels);
    for (const std::uint32_t id : targets_) {
        put(kCreateTarget, &id, 4);
        if (inner_->read_target(id, pixels.data())) put(kUpload, &id, 4, pixels.data(), kViewPixels * 4u);
    }
    for (const std::uint32_t id : depths_) put(kCreateDepth, &id, 4);
    return true;
}

void TraceBackend::create_texture(std::uint32_t id, std::uint32_t w, std::uint32_t h, const std::uint32_t *rgba) {
    Texture &t = textures_[id];
    t.w = w;
    t.h = h;
    t.px.assign(rgba, rgba + static_cast<std::size_t>(w) * h);
    const std::uint32_t head[3] = {id, w, h};
    put(kCreateTexture, head, sizeof(head), rgba, static_cast<std::size_t>(w) * h * 4u);
    inner_->create_texture(id, w, h, rgba);
}
void TraceBackend::destroy_texture(std::uint32_t id) {
    textures_.erase(id);
    put(kDestroyTexture, &id, 4);
    inner_->destroy_texture(id);
}
void TraceBackend::create_target(std::uint32_t id) {
    targets_.push_back(id);
    put(kCreateTarget, &id, 4);
    inner_->create_target(id);
}
void TraceBackend::create_depth(std::uint32_t id) {
    depths_.push_back(id);
    put(kCreateDepth, &id, 4);
    inner_->create_depth(id);
}
void TraceBackend::upload_target(std::uint32_t id, const std::uint32_t *rgba) {
    put(kUpload, &id, 4, rgba, kViewPixels * 4u);
    inner_->upload_target(id, rgba);
}
void TraceBackend::draw(const State &state, const Vertex *vertices, std::uint32_t count) {
    put(kDraw, &state, sizeof(state), vertices, static_cast<std::size_t>(count) * sizeof(Vertex));
    inner_->draw(state, vertices, count);
}
bool TraceBackend::read_target(std::uint32_t id, std::uint32_t *rgba) {
    const bool ok = inner_->read_target(id, rgba);
    if (file_ != nullptr) {
        put(kRead, &id, 4);
        if (--frames_left_ == 0u) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }
    return ok;
}
void TraceBackend::flush() {
    put(kFlush, nullptr, 0);
    inner_->flush();
}

int replay_trace(const std::string &path, Backend &backend,
                 const std::function<void(std::uint32_t, std::uint32_t, const std::uint32_t *)> &frame) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return -1;
    std::vector<std::uint32_t> textures; // live ids, destroyed at a reset
    std::vector<std::uint8_t> data;
    std::vector<std::uint32_t> pixels(kViewPixels);
    std::uint32_t window = 0, index = 0, id_base = 0, id_max = 0;
    int pictures = 0;
    std::uint32_t header[2];
    // Ids restart with every window, a back end's ids never do: each window's
    // ids are shifted past everything used before.
    auto map = [&](std::uint32_t id) { id_max = std::max(id_max, id_base + id); return id_base + id; };
    while (std::fread(header, 4, 2, f) == 2) {
        data.resize(header[1]);
        if (header[1] != 0u && std::fread(data.data(), 1, header[1], f) != header[1]) { std::fclose(f); return -1; }
        auto u32 = [&](std::size_t i) { std::uint32_t v; std::memcpy(&v, data.data() + i * 4u, 4); return v; };
        switch (header[0]) {
        case kReset:
            for (const std::uint32_t id : textures) backend.destroy_texture(id);
            textures.clear();
            id_base = id_max + 1u;
            ++window;
            index = 0;
            break;
        case kCreateTexture: {
            const std::uint32_t id = map(u32(0)), w = u32(1), h = u32(2);
            if (data.size() != 12u + static_cast<std::size_t>(w) * h * 4u) { std::fclose(f); return -1; }
            std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h);
            std::memcpy(px.data(), data.data() + 12, px.size() * 4u);
            backend.create_texture(id, w, h, px.data());
            textures.push_back(id);
            break;
        }
        case kDestroyTexture: {
            const std::uint32_t id = map(u32(0));
            backend.destroy_texture(id);
            for (auto &t : textures)
                if (t == id) { t = textures.back(); textures.pop_back(); break; }
            break;
        }
        case kCreateTarget: backend.create_target(map(u32(0))); break;
        case kCreateDepth: backend.create_depth(map(u32(0))); break;
        case kUpload:
            if (data.size() != 4u + kViewPixels * 4u) { std::fclose(f); return -1; }
            std::memcpy(pixels.data(), data.data() + 4, kViewPixels * 4u);
            backend.upload_target(map(u32(0)), pixels.data());
            break;
        case kDraw: {
            if (data.size() < sizeof(State) || (data.size() - sizeof(State)) % sizeof(Vertex) != 0u) { std::fclose(f); return -1; }
            State st;
            std::memcpy(&st, data.data(), sizeof(st));
            st.target = map(st.target);
            st.depth = map(st.depth);
            if (st.textured) st.texture = map(st.texture);
            std::vector<Vertex> vertices((data.size() - sizeof(State)) / sizeof(Vertex));
            if (!vertices.empty()) std::memcpy(vertices.data(), data.data() + sizeof(State), vertices.size() * sizeof(Vertex));
            backend.draw(st, vertices.data(), static_cast<std::uint32_t>(vertices.size()));
            break;
        }
        case kRead:
            if (backend.read_target(map(u32(0)), pixels.data())) {
                frame(window, index, pixels.data());
                ++pictures;
            }
            ++index;
            break;
        case kFlush: backend.flush(); break;
        default: std::fclose(f); return -1;
        }
    }
    std::fclose(f);
    return pictures;
}

} // namespace p3pios::gpu
