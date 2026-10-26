#include "assets/AssetIndex.hpp"

#include "util/Binary.hpp"
#include "util/Text.hpp"
#include "vfs/VirtualFileSystem.hpp"

#include <fstream>

namespace fo4mx {

const char* assetTypeName(AssetType t)
{
    switch (t) {
    case AssetType::Texture: return "Texture";
    case AssetType::Mesh: return "Mesh";
    case AssetType::Script: return "Script";
    case AssetType::Material: return "Material";
    case AssetType::Audio: return "Audio";
    case AssetType::Animation: return "Animation";
    case AssetType::Interface: return "Interface";
    case AssetType::FaceGen: return "FaceGen";
    case AssetType::Precombined: return "Precombined";
    case AssetType::Previs: return "Previs";
    case AssetType::Strings: return "Strings";
    case AssetType::Other: return "Other";
    }
    return "Other";
}

AssetType classifyAsset(std::string_view p)
{
    auto ext = p.substr(p.find_last_of('.') == std::string_view::npos ? p.size() : p.find_last_of('.'));
    if (istartsWith(p, "meshes\\actors\\character\\facegendata\\") ||
        istartsWith(p, "textures\\actors\\character\\facecustomization\\"))
        return AssetType::FaceGen;
    if (istartsWith(p, "meshes\\precombined\\"))
        return AssetType::Precombined;
    if (istartsWith(p, "vis\\"))
        return AssetType::Previs;
    if (iequals(ext, ".dds"))
        return AssetType::Texture;
    if (iequals(ext, ".nif") || iequals(ext, ".btr") || iequals(ext, ".bto") || iequals(ext, ".tri") ||
        iequals(ext, ".ssf") || iequals(ext, ".lod"))
        return AssetType::Mesh;
    if (iequals(ext, ".pex") || iequals(ext, ".psc"))
        return AssetType::Script;
    if (iequals(ext, ".bgsm") || iequals(ext, ".bgem"))
        return AssetType::Material;
    if (iequals(ext, ".wav") || iequals(ext, ".xwm") || iequals(ext, ".fuz") || iequals(ext, ".lip"))
        return AssetType::Audio;
    if (iequals(ext, ".hkx") || iequals(ext, ".hkt") || (iequals(ext, ".txt") && istartsWith(p, "meshes\\")))
        return AssetType::Animation;
    if (iequals(ext, ".swf") || istartsWith(p, "interface\\"))
        return AssetType::Interface;
    if (iequals(ext, ".strings") || iequals(ext, ".dlstrings") || iequals(ext, ".ilstrings"))
        return AssetType::Strings;
    return AssetType::Other;
}

AssetEntry& AssetIndex::getOrAdd(const std::string& normalizedPath)
{
    auto [it, inserted] = byPath_.try_emplace(normalizedPath, static_cast<uint32_t>(entries.size()));
    if (inserted) {
        AssetEntry e;
        e.path = strings.intern(normalizedPath);
        e.type = classifyAsset(normalizedPath);
        entries.push_back(std::move(e));
    }
    return entries[it->second];
}

const AssetEntry* AssetIndex::find(std::string_view normalizedPath) const
{
    auto it = byPath_.find(std::string(normalizedPath));
    return it == byPath_.end() ? nullptr : &entries[it->second];
}

std::string AssetIndex::sourceLabel(const AssetSource& s) const
{
    if (s.archive == kLooseFile)
        return modName(s) + " (loose)";
    return modName(s) + " [" + archives[s.archive].name + "]";
}

std::vector<uint8_t> AssetIndex::read(const AssetEntry& e, size_t sourceSlot, const VirtualFileSystem& vfs,
                                      size_t maxBytes) const
{
    const auto& s = e.sources.at(sourceSlot);
    if (s.archive != kLooseFile) {
        const auto& ar = *archives.at(s.archive).archive;
        return maxBytes ? ar.extractHead(s.entry, maxBytes) : ar.extract(s.entry);
    }
    const auto& f = vfs.files().at(s.entry);
    auto path = vfs.physicalPath(f, s.slot);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw FormatError("cannot open " + pathToUtf8(path));
    size_t size = static_cast<size_t>(s.size);
    if (maxBytes && size > maxBytes)
        size = maxBytes;
    std::vector<uint8_t> out(size);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

size_t AssetIndex::conflictCount() const
{
    size_t n = 0;
    for (const auto& e : entries)
        if (e.conflicting())
            ++n;
    return n;
}

size_t AssetIndex::looseFileCount() const
{
    size_t n = 0;
    for (const auto& e : entries)
        for (const auto& s : e.sources)
            if (s.archive == kLooseFile)
                ++n;
    return n;
}

void AssetIndex::clear()
{
    archives.clear();
    entries.clear();
    byPath_.clear();
}

} // namespace fo4mx
