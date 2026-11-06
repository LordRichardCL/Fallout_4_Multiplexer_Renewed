#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fo4mx {

struct ZipStats {
    size_t files = 0;
    uint64_t bytes = 0; // uncompressed
};

// Writes every file under `folder` into a zip archive at `zip`, each named `prefix` + its path
// relative to `folder` with forward slashes (for example "Data/Dummy_Plugin_01.esp"). Files are
// deflated, except formats that are already compressed (voice, textures, archives), which are
// stored. Zip64 records are written when the archive needs them (over 65,535 files or 4 GB), so any
// size works. Written to a temporary file first and renamed, so a failed run leaves no broken zip.
ZipStats writeZip(const std::filesystem::path& zip, const std::filesystem::path& folder, const std::string& prefix);
// The same, leaving out files for which `skip(path relative to folder)` is true.
ZipStats writeZip(const std::filesystem::path& zip, const std::filesystem::path& folder, const std::string& prefix,
                  const std::function<bool(const std::filesystem::path&)>& skip);

} // namespace fo4mx
