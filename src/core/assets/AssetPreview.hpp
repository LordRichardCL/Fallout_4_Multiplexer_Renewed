#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fo4mx::preview {

struct TextureInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipCount = 0;
    uint32_t dxgiFormat = 0;
    std::string format; // human readable, e.g. "BC7 (sRGB)"
    bool cubemap = false;
    bool decodable = false;
    size_t dataOffset = 0;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width * height * 4
};

std::optional<TextureInfo> readDdsInfo(const std::vector<uint8_t>& dds);
// Decodes the top mip level. Returns nullopt for formats that are not supported (BC6H/BC7).
std::optional<Image> decodeDds(const std::vector<uint8_t>& dds, uint32_t maxDimension = 1024);

struct MeshInfo {
    std::string headerLine;
    uint32_t version = 0;
    uint32_t userVersion = 0;
    uint32_t bsVersion = 0;
    uint32_t blockCount = 0;
    std::map<std::string, uint32_t> blockTypes;
    std::vector<std::string> strings; // string table (node names, material paths)
    bool complete = false;
};
std::optional<MeshInfo> readNifInfo(const std::vector<uint8_t>& nif);

struct MaterialInfo {
    std::string kind; // BGSM / BGEM
    uint32_t version = 0;
    std::vector<std::string> textures;
};
std::optional<MaterialInfo> readMaterialInfo(const std::vector<uint8_t>& data);

} // namespace fo4mx::preview
