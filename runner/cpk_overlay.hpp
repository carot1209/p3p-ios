// Music mods: the game streams its ADX music straight out of umd0.cpk, past
// the Mod Support loose-file loader. This wraps the disc so that files under
// <mods>/bind/data/sound/ replace the ones in the archive: the archive's
// table of contents is served with their new size and an offset past the end
// of the real file, where the replacement's bytes are served from.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "p3p3ds/vfs.hpp"

namespace p3pios {

struct CpkOverride {
    std::string name;                      // "data/sound/bgm/23.ADX"
    std::uint64_t size{};
    std::shared_ptr<std::atomic<std::uint64_t>> bytes_read;
};

class CpkOverlayFileSystem final : public p3p3ds::vfs::FileSystem {
public:
    // `sound_root` is <mods>/bind/data/sound (it need not exist).
    CpkOverlayFileSystem(std::shared_ptr<p3p3ds::vfs::FileSystem> inner, std::filesystem::path sound_root);
    [[nodiscard]] std::shared_ptr<const p3p3ds::vfs::Source> open(std::string_view path) const override;
    [[nodiscard]] std::optional<p3p3ds::vfs::Entry> stat(std::string_view path) const override;
    [[nodiscard]] std::optional<std::vector<p3p3ds::vfs::Entry>> list(std::string_view path) const override;
    [[nodiscard]] const std::vector<CpkOverride> &overrides() const noexcept { return overrides_; }
    [[nodiscard]] const std::string &note() const noexcept { return note_; }

private:
    std::shared_ptr<p3p3ds::vfs::FileSystem> inner_;
    std::shared_ptr<const p3p3ds::vfs::Source> archive_; // the patched umd0.cpk, or null
    std::vector<CpkOverride> overrides_;
    std::string note_;
};

} // namespace p3pios
