#include "reference/FormIdReference.hpp"

#include "engine/Session.hpp"
#include "engine/ValidationEngine.hpp"
#include "util/Compression.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'F', 'O', '4', 'X', 'R', 'E', 'F', '\0'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kOverrideBit = 0x80000000u;
constexpr size_t kHeaderSize = 8 + 12 + 32;

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    void need(size_t n) const
    {
        if (pos + n > b.size())
            throw std::runtime_error("FormID reference: truncated data");
    }
    uint32_t u32()
    {
        need(4);
        uint32_t v = uint32_t(b[pos]) | uint32_t(b[pos + 1]) << 8 | uint32_t(b[pos + 2]) << 16 | uint32_t(b[pos + 3]) << 24;
        pos += 4;
        return v;
    }
    uint16_t u16()
    {
        need(2);
        uint16_t v = uint16_t(b[pos] | b[pos + 1] << 8);
        pos += 2;
        return v;
    }
    std::string str(size_t n)
    {
        need(n);
        std::string s(reinterpret_cast<const char*>(b.data() + pos), n);
        pos += n;
        return s;
    }
};

} // namespace

FormIdReference FormIdReference::load(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + pathToUtf8(file));
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < kHeaderSize || std::memcmp(raw.data(), kMagic, 8) != 0)
        throw std::runtime_error(pathToUtf8(file.filename()) + " is not a FormID reference file");
    Reader h{raw, 8};
    const uint32_t version = h.u32();
    const uint32_t payloadSize = h.u32();
    const uint32_t packedSize = h.u32();
    if (version != kVersion)
        throw std::runtime_error("FormID reference: unsupported version " + std::to_string(version));
    if (kHeaderSize + packedSize != raw.size())
        throw std::runtime_error("FormID reference: size does not match its header");

    FormIdReference ref;
    char hex[3];
    for (size_t i = 0; i < 32; ++i) {
        std::snprintf(hex, sizeof(hex), "%02x", raw[20 + i]);
        ref.sha256_ += hex;
    }
    const auto payload = zlibInflate(std::span<const uint8_t>(raw.data() + kHeaderSize, packedSize), payloadSize);
    if (payload.size() != payloadSize)
        throw std::runtime_error("FormID reference: damaged data");
    Reader r{payload};
    const uint32_t pluginCount = r.u32();
    for (uint32_t i = 0; i < pluginCount; ++i) {
        Plugin p;
        p.name = r.str(r.u16());
        p.sourceRows = r.u32();
        p.maxObjectId = r.u32();
        ref.plugins_.push_back(std::move(p));
    }
    const uint32_t rows = r.u32();
    r.need(size_t(rows) * 8);
    ref.keys_.resize(rows);
    ref.offsets_.resize(rows);
    for (uint32_t i = 0; i < rows; ++i) {
        ref.keys_[i] = r.u32();
        ref.offsets_[i] = r.u32();
    }
    ref.text_ = r.str(r.u32());
    if (!std::is_sorted(ref.keys_.begin(), ref.keys_.end()))
        throw std::runtime_error("FormID reference: rows are not sorted");
    for (uint32_t i = 0; i < rows; ++i)
        if ((ref.keys_[i] >> 24) >= pluginCount || (ref.offsets_[i] & ~kOverrideBit) >= ref.text_.size())
            throw std::runtime_error("FormID reference: row " + std::to_string(i) + " is out of range");
    return ref;
}

std::optional<FormIdReference> FormIdReference::tryLoad(const fs::path& file)
{
    std::error_code ec;
    if (file.empty() || !fs::is_regular_file(file, ec))
        return std::nullopt;
    return load(file);
}

int FormIdReference::pluginIndex(std::string_view name) const
{
    for (size_t i = 0; i < plugins_.size(); ++i)
        if (iequals(plugins_[i].name, name))
            return static_cast<int>(i);
    return -1;
}

std::vector<FormIdReference::Entry> FormIdReference::find(std::string_view plugin, uint32_t objectId) const
{
    std::vector<Entry> out;
    const int p = pluginIndex(plugin);
    if (p < 0 || objectId > 0xFFFFFF)
        return out;
    const uint32_t key = uint32_t(p) << 24 | objectId;
    auto [lo, hi] = std::equal_range(keys_.begin(), keys_.end(), key);
    for (auto it = lo; it != hi; ++it) {
        const uint32_t off = offsets_[size_t(it - keys_.begin())];
        out.push_back({std::string_view(text_.c_str() + (off & ~kOverrideBit)), (off & kOverrideBit) != 0});
    }
    return out;
}

std::vector<std::pair<uint32_t, FormIdReference::Entry>> FormIdReference::entries(std::string_view plugin) const
{
    std::vector<std::pair<uint32_t, Entry>> out;
    const int p = pluginIndex(plugin);
    if (p < 0)
        return out;
    const uint32_t first = uint32_t(p) << 24, last = first | 0xFFFFFF;
    auto lo = std::lower_bound(keys_.begin(), keys_.end(), first);
    auto hi = std::upper_bound(keys_.begin(), keys_.end(), last);
    for (auto it = lo; it != hi; ++it) {
        const uint32_t off = offsets_[size_t(it - keys_.begin())];
        out.push_back({*it & 0xFFFFFF,
                       Entry{std::string_view(text_.c_str() + (off & ~kOverrideBit)), (off & kOverrideBit) != 0}});
    }
    return out;
}

bool ReferenceComparison::agrees() const
{
    for (const auto& p : plugins)
        if (p.missing || p.different)
            return false;
    return true;
}

ReferenceComparison compareWithReference(const FormIdReference& reference, const Session& session)
{
    ReferenceComparison out;
    for (const auto& rp : reference.plugins()) {
        const auto* plugin = session.findPlugin(rp.name);
        if (!plugin || !plugin->error.empty())
            continue;
        // The records this plugin defines, with the Editor ID this app read.
        std::unordered_map<uint32_t, std::string_view> scanned;
        for (const auto& r : session.records)
            if (r.key.origin == plugin->index && r.plugin == plugin->index)
                scanned.emplace(r.key.objectId & 0xFFFFFF, session.strings.get(r.editorId));

        ReferenceComparison::PluginResult res;
        res.plugin = plugin->name;
        auto sample = [&](std::string s) {
            if (res.samples.size() < 20)
                res.samples.push_back(std::move(s));
        };
        std::unordered_map<uint32_t, bool> listed;
        for (const auto& [id, e] : reference.entries(rp.name)) {
            listed.emplace(id, true);
            if (e.baseGameOverride)
                continue;
            ++res.compared;
            char fid[16];
            std::snprintf(fid, sizeof(fid), "%06X", id);
            auto it = scanned.find(id);
            if (it == scanned.end()) {
                ++res.missing;
                sample(std::string(fid) + " " + std::string(e.editorId) + ": not found in this plugin");
            } else if (iequals(it->second, e.editorId)) {
                ++res.matched;
            } else {
                ++res.different;
                sample(std::string(fid) + ": the reference has " + std::string(e.editorId) + ", this app read " +
                       (it->second.empty() ? std::string("no Editor ID") : std::string(it->second)));
            }
        }
        for (const auto& [id, edid] : scanned)
            if (!edid.empty() && !listed.count(id))
                ++res.extra;
        out.plugins.push_back(std::move(res));
    }
    return out;
}

void reportReferenceComparison(const ReferenceComparison& comparison, ValidationReport& report)
{
    for (const auto& p : comparison.plugins) {
        const std::string counts = std::to_string(p.matched) + " of " + std::to_string(p.compared) +
                                   " Editor IDs match";
        if (!p.missing && !p.different) {
            report.fail(Severity::Info, "Reference Check",
                        p.plugin + ": the records this app read match the independent reference (" + counts +
                            (p.extra ? "; " + std::to_string(p.extra) + " Editor IDs the reference does not list" : "") +
                            ")",
                        p.plugin);
            continue;
        }
        std::string msg = p.plugin + ": " + counts + "; " + std::to_string(p.missing) + " not found, " +
                          std::to_string(p.different) + " with another Editor ID";
        if (!p.samples.empty())
            msg += ". First: " + join(p.samples, "; ");
        report.fail(Severity::Warning, "Reference Check", msg, p.plugin);
        auto& is = report.issues.back();
        is.cause = "The reference was made from Fallout 4's own masters by another tool. Many differences usually "
                   "mean another version of the game files (Old Gen / Next Gen, or an update); a few can mean this app "
                   "reads those records differently. Compare the listed records in xEdit.";
    }
}

} // namespace fo4mx
