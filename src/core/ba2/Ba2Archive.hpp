#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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
    std::string path; // normalised asset path
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

    // Extract one file. Texture entries are rebuilt into a complete DDS file.
    std::vector<uint8_t> extract(size_t index) const;
    // Extract at most maxBytes from the start of a general entry (fast metadata reads).
    std::vector<uint8_t> extractHead(size_t index, size_t maxBytes) const;

private:
    std::filesystem::path path_;
    Ba2Type type_ = Ba2Type::General;
    uint32_t version_ = 0;
    std::vector<Ba2Entry> entries_;
    mutable std::mutex mutex_;
};

// Builds a DDS header for a DXGI format (used by texture extraction and tests).
std::vector<uint8_t> makeDdsHeader(uint32_t width, uint32_t height, uint32_t mipCount, uint32_t dxgiFormat, bool cubemap);

// Writes a general (GNRL) BA2. Used by tests and tooling.
void writeGeneralBa2(const std::filesystem::path& path,
                     const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files, bool compress);

} // namespace fo4mx
