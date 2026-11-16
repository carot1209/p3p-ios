#include "cpk_overlay.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace p3pios {
namespace {

using p3p3ds::vfs::Source;

constexpr char kArchive[] = "PSP_GAME/USRDIR/UMD0.CPK";

std::string upper(std::string s) {
    for (auto &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::uint32_t be16(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
std::uint64_t be64(const std::uint8_t *p) { return static_cast<std::uint64_t>(be32(p)) << 32 | be32(p + 4); }
void put(std::uint8_t *p, std::uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
}
unsigned width(unsigned type) {
    switch (type) {
    case 0: case 1: return 1;
    case 2: case 3: return 2;
    case 4: case 5: case 8: case 0xA: return 4;
    case 6: case 7: case 0xB: return 8;
    default: return 0;
    }
}

// One "@UTF" table (CRI): fixed-length rows, typed columns.
struct Utf {
    std::uint8_t *data{};      // at "@UTF"
    std::size_t size{};
    std::uint32_t rows_off{}, str_off{}, row_len{}, rows{};
    struct Column { std::string name; unsigned type{}; bool per_row{}; std::uint32_t at{}; }; // at: in the row, or of the constant
    std::vector<Column> columns;

    bool parse(std::uint8_t *d, std::size_t n) {
        if (n < 32 || std::memcmp(d, "@UTF", 4) != 0) return false;
        data = d; size = n;
        rows_off = be16(d + 10) + 8; str_off = be32(d + 12) + 8;
        const std::uint32_t count = be16(d + 24);
        row_len = be16(d + 26); rows = be32(d + 28);
        std::size_t p = 32; std::uint32_t in_row = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (p + 5 > n) return false;
            const unsigned flags = d[p]; const std::uint32_t name = be32(d + p + 1); p += 5;
            Column c; c.type = flags & 0xF;
            if (str_off + name < n) c.name = reinterpret_cast<const char *>(d + str_off + name);
            const unsigned w = width(c.type);
            if (w == 0) return false;
            if ((flags & 0xF0) == 0x30) { c.at = static_cast<std::uint32_t>(p); p += w; }
            else if ((flags & 0xF0) == 0x50) { c.per_row = true; c.at = in_row; in_row += w; }
            else c.type = 0xFF; // no value
            columns.push_back(std::move(c));
        }
        return rows_off + static_cast<std::uint64_t>(row_len) * rows <= n && in_row <= row_len;
    }
    const Column *column(const char *name) const {
        for (const auto &c : columns) if (c.name == name && c.type != 0xFF) return &c;
        return nullptr;
    }
    std::uint8_t *cell(const Column &c, std::uint32_t row) const { return c.per_row ? data + rows_off + row * row_len + c.at : data + c.at; }
    std::uint64_t number(const Column &c, std::uint32_t row) const {
        const std::uint8_t *p = cell(c, row);
        switch (width(c.type)) { case 1: return p[0]; case 2: return be16(p); case 4: return be32(p); default: return be64(p); }
    }
    std::string text(const Column &c, std::uint32_t row) const {
        const std::uint32_t o = str_off + be32(cell(c, row));
        return o < size ? std::string(reinterpret_cast<const char *>(data + o)) : std::string();
    }
};

struct HostFile {
    std::filesystem::path path;
    std::uint64_t at{}, size{};
    std::shared_ptr<std::atomic<std::uint64_t>> bytes_read;
    mutable std::ifstream stream;
};

class Archive final : public Source {
public:
    std::shared_ptr<const Source> inner;
    std::uint64_t inner_size{}, total{};
    std::uint64_t toc_at{};
    std::vector<std::uint8_t> toc;       // the patched copy of [toc_at, toc_at + toc.size())
    std::vector<HostFile> files;         // ascending `at`, all past inner_size
    mutable std::mutex lock;

    [[nodiscard]] std::uint64_t size() const override { return total; }
    std::size_t read(std::uint64_t offset, void *destination, std::size_t length) const override {
        auto *out = static_cast<std::uint8_t *>(destination);
        std::size_t done = 0;
        while (done < length) {
            const std::uint64_t at = offset + done;
            if (at >= total) break;
            std::size_t want = length - done, got = 0;
            if (at >= toc_at && at < toc_at + toc.size()) {
                got = static_cast<std::size_t>(std::min<std::uint64_t>(want, toc_at + toc.size() - at));
                std::memcpy(out + done, toc.data() + (at - toc_at), got);
            } else if (at < inner_size) {
                std::uint64_t end = inner_size;
                if (at < toc_at) end = std::min(end, toc_at);
                got = inner->read(at, out + done, static_cast<std::size_t>(std::min<std::uint64_t>(want, end - at)));
                if (got == 0) break;
            } else {
                const HostFile *file = nullptr; std::uint64_t next = total;
                for (const auto &f : files) {
                    if (at >= f.at && at < f.at + f.size) { file = &f; break; }
                    if (f.at > at) { next = f.at; break; }
                }
                if (file == nullptr) { // padding between files
                    got = static_cast<std::size_t>(std::min<std::uint64_t>(want, next - at));
                    std::memset(out + done, 0, got);
                } else {
                    got = static_cast<std::size_t>(std::min<std::uint64_t>(want, file->at + file->size - at));
                    std::lock_guard<std::mutex> guard(lock);
                    if (!file->stream.is_open()) file->stream.open(file->path, std::ios::binary);
                    file->stream.clear();
                    file->stream.seekg(static_cast<std::streamoff>(at - file->at));
                    file->stream.read(reinterpret_cast<char *>(out + done), static_cast<std::streamsize>(got));
                    const auto n = static_cast<std::size_t>(std::max<std::streamsize>(0, file->stream.gcount()));
                    if (n < got) std::memset(out + done + n, 0, got - n);
                    file->bytes_read->fetch_add(got, std::memory_order_relaxed);
                }
            }
            done += got;
        }
        return done;
    }
};

} // namespace

CpkOverlayFileSystem::CpkOverlayFileSystem(std::shared_ptr<p3p3ds::vfs::FileSystem> inner, std::filesystem::path sound_root)
    : inner_(std::move(inner)) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> wanted; // upper-case archive name -> host file
    if (fs::is_directory(sound_root, ec)) {
        for (auto it = fs::recursive_directory_iterator(sound_root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            wanted.emplace_back(upper("data/sound/" + fs::relative(it->path(), sound_root, ec).generic_string()), it->path());
        }
    }
    if (wanted.empty()) return;
    auto source = inner_->open(kArchive);
    if (!source) { note_ = "umd0.cpk not found"; return; }

    auto archive = std::make_shared<Archive>();
    archive->inner = source;
    archive->inner_size = source->size();
    // "CPK " header, then its @UTF at 16 with the offsets.
    std::vector<std::uint8_t> head(2048);
    if (source->read(0, head.data(), head.size()) != head.size() || std::memcmp(head.data(), "CPK ", 4) != 0) { note_ = "umd0.cpk: no CPK header"; return; }
    Utf header;
    if (!header.parse(head.data() + 16, head.size() - 16)) { note_ = "umd0.cpk: header table unreadable"; return; }
    const auto *toc_col = header.column("TocOffset"), *content_col = header.column("ContentOffset");
    if (toc_col == nullptr || content_col == nullptr) { note_ = "umd0.cpk: no TOC offset"; return; }
    const std::uint64_t toc_offset = header.number(*toc_col, 0), content = header.number(*content_col, 0);
    const std::uint64_t base = std::min(toc_offset, content);
    std::uint8_t toc_head[16];
    if (source->read(toc_offset, toc_head, 16) != 16 || std::memcmp(toc_head, "TOC ", 4) != 0) { note_ = "umd0.cpk: no TOC"; return; }
    std::uint64_t toc_size = 0;
    for (int i = 7; i >= 0; --i) toc_size = toc_size << 8 | toc_head[8 + i]; // little-endian
    if (toc_size < 32 || toc_size > (64u << 20)) { note_ = "umd0.cpk: TOC size"; return; }
    archive->toc_at = toc_offset + 16;
    archive->toc.resize(static_cast<std::size_t>(toc_size));
    if (source->read(archive->toc_at, archive->toc.data(), archive->toc.size()) != archive->toc.size()) { note_ = "umd0.cpk: TOC read"; return; }
    Utf toc;
    if (!toc.parse(archive->toc.data(), archive->toc.size())) { note_ = "umd0.cpk: TOC table unreadable"; return; }
    const auto *dir = toc.column("DirName"), *file = toc.column("FileName"), *size = toc.column("FileSize"),
               *extract = toc.column("ExtractSize"), *offset = toc.column("FileOffset");
    if (dir == nullptr || file == nullptr || size == nullptr || offset == nullptr || !size->per_row || !offset->per_row) { note_ = "umd0.cpk: TOC columns"; return; }

    std::uint64_t next = (archive->inner_size + 2047) / 2048 * 2048;
    for (std::uint32_t row = 0; row < toc.rows; ++row) {
        const std::string d = toc.text(*dir, row);
        const std::string name = (d.empty() ? std::string() : d + "/") + toc.text(*file, row);
        const std::string key = upper(name);
        const auto hit = std::find_if(wanted.begin(), wanted.end(), [&](const auto &w) { return w.first == key; });
        if (hit == wanted.end()) continue;
        const std::uint64_t bytes = fs::file_size(hit->second, ec);
        if (ec || bytes == 0) continue;
        const unsigned ow = width(offset->type), sw = width(size->type);
        if (next < base || (ow < 8 && next - base > 0xFFFFFFFFull) || (sw < 8 && bytes > 0xFFFFFFFFull)) continue;
        put(toc.cell(*offset, row), next - base, ow);
        put(toc.cell(*size, row), bytes, sw);
        if (extract != nullptr && extract->per_row) put(toc.cell(*extract, row), bytes, width(extract->type));
        CpkOverride o; o.name = name; o.size = bytes; o.bytes_read = std::make_shared<std::atomic<std::uint64_t>>(0);
        HostFile f; f.path = hit->second; f.at = next; f.size = bytes; f.bytes_read = o.bytes_read;
        archive->files.push_back(std::move(f));
        overrides_.push_back(std::move(o));
        next = (next + bytes + 2047) / 2048 * 2048;
    }
    if (overrides_.empty()) { note_ = "no file under mods/bind/data/sound matches one in umd0.cpk"; return; }
    archive->total = next;
    archive_ = archive;
}

std::shared_ptr<const p3p3ds::vfs::Source> CpkOverlayFileSystem::open(std::string_view path) const {
    if (archive_) {
        const auto clean = p3p3ds::vfs::normalize(path);
        if (clean && upper(*clean) == kArchive) return archive_;
    }
    return inner_->open(path);
}

std::optional<p3p3ds::vfs::Entry> CpkOverlayFileSystem::stat(std::string_view path) const {
    auto entry = inner_->stat(path);
    if (entry && archive_) {
        const auto clean = p3p3ds::vfs::normalize(path);
        if (clean && upper(*clean) == kArchive) entry->size = archive_->size();
    }
    return entry;
}

std::optional<std::vector<p3p3ds::vfs::Entry>> CpkOverlayFileSystem::list(std::string_view path) const {
    return inner_->list(path);
}

} // namespace p3pios
