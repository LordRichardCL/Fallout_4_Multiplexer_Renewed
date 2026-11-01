#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fo4mx {

enum class Ba2Type : uint8_t { General, Texture };

struct Ba2TextureChunk {
    uint64_t offset = 0;
    uint32_t packedSize = 0;
    uint32_t unpackedSize = 0;
    uint16_t startMip = 0;
    uint16_t endMip = 0;
};

struct Ba2Entry {
    std::string path; // normalised asset path; empty in an archive without a name table
    // How the game finds the file (xEdit TwbHash.FO4): CRC32 of the folder and of the file name
    // without extension, lower case with backslashes, and the extension's first 4 characters.
    uint32_t nameHash = 0;
    uint32_t extension = 0;
    uint32_t dirHash = 0;
    uint64_t offset = 0;
    uint32_t packedSize = 0; // 0 = stored uncompressed
    uint32_t unpackedSize = 0;
    // Texture archives only:
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mipCount = 0;
    uint8_t dxgiFormat = 0;
    bool cubemap = false;
    std::vector<Ba2TextureChunk> chunks;

    uint64_t size() const;
};

// Bethesda Archive v2 (Fallout 4). Reads the index eagerly and file data on demand.
class Ba2Archive {
public:
    static std::unique_ptr<Ba2Archive> open(const std::filesystem::path& path);

    const std::filesystem::path& path() const { return path_; }
    std::string name() const;
    Ba2Type type() const { return type_; }
    uint32_t version() const { return version_; }
    const std::vector<Ba2Entry>& entries() const { return entries_; }
    // Archive2 can write an archive without its name table (-nostrings): the game finds files by
    // their hashes, so it loads; paths are then matched by hash (Ba2Hash).
    bool hasNames() const { return hasNames_; }

    // Extract one file. Texture entries are rebuilt into a complete DDS file.
    std::vector<uint8_t> extract(size_t index) const;
    // Extract at most maxBytes from the start of a general entry (fast metadata reads).
    std::vector<uint8_t> extractHead(size_t index, size_t maxBytes) const;

private:
    std::filesystem::path path_;
    Ba2Type type_ = Ba2Type::General;
    uint32_t version_ = 0;
    std::vector<Ba2Entry> entries_;
    bool hasNames_ = true;
    mutable std::mutex mutex_;
};

// The hashes the game uses to find a file in a BA2 (xEdit TwbHash.FO4 over a normalised path).
struct Ba2Hash {
    uint32_t nameHash = 0;
    uint32_t extension = 0;
    uint32_t dirHash = 0;
    bool operator==(const Ba2Hash&) const = default;
};
Ba2Hash ba2Hash(std::string_view normalizedPath);
uint32_t bsCrc32(std::string_view text); // CRC32 table, start 0, no final xor, lower case
// The name xEdit shows for a file whose name is not stored: "<dir hash>\<name hash>.<ext>".
std::string unnamedBa2Path(const Ba2Entry& e);

// Builds a DDS header for a DXGI format (used by texture extraction and tests).
std::vector<uint8_t> makeDdsHeader(uint32_t width, uint32_t height, uint32_t mipCount, uint32_t dxgiFormat, bool cubemap);

// Writes a general (GNRL) BA2. Used by tests and tooling.
void writeGeneralBa2(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, bool compress,
                     bool names = true);

} // namespace fo4mx
