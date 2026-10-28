#include "esp/PluginWriter.hpp"

#include "esp/FormId.hpp"
#include "esp/RecordCatalogue.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <map>

namespace fo4mx {

const std::vector<Signature>& PluginWriter::topLevelOrder()
{
    return RecordCatalogue::get().groupOrder();
}

std::vector<uint8_t> PluginWriter::build(bool compressLargeRecords)
{
    std::map<Signature, std::vector<const Record*>> bySignature;
    for (const auto& r : records_)
        bySignature[r.signature()].push_back(&r);

    std::vector<Signature> order;
    for (auto s : topLevelOrder())
        if (bySignature.count(s))
            order.push_back(s);
    for (const auto& [s, _] : bySignature)
        if (std::find(order.begin(), order.end(), s) == order.end())
            order.push_back(s);

    BinaryWriter body;
    uint32_t groupCount = 0;
    uint32_t maxObjectId = 0;
    uint32_t selfIndex = static_cast<uint32_t>(header_.masters.size());
    for (auto s : order) {
        auto& list = bySignature[s];
        std::stable_sort(list.begin(), list.end(),
                         [](const Record* a, const Record* b) { return a->header.formId < b->header.formId; });
        size_t groupStart = body.size();
        GroupHeader g;
        g.label = s.value;
        g.groupType = 0;
        g.write(body);
        for (const Record* r : list) {
            bool compress = compressLargeRecords && r->serializeSubrecords().size() > 16 * 1024;
            r->write(body, compress);
            if (masterIndexOf(r->header.formId) == selfIndex)
                maxObjectId = std::max(maxObjectId, objectIdOf(r->header.formId));
        }
        body.patch<uint32_t>(groupStart + 4, static_cast<uint32_t>(body.size() - groupStart));
        ++groupCount;
    }

    header_.numRecords = static_cast<int32_t>(records_.size() + groupCount);
    header_.nextObjectId = std::max<uint32_t>(PluginHeader::kDefaultNextObjectId, maxObjectId + 1);

    BinaryWriter out;
    header_.toRecord().write(out, false);
    out.writeBytes(body.buffer());
    return out.take();
}

void PluginWriter::write(const std::filesystem::path& path, bool compressLargeRecords)
{
    auto image = build(compressLargeRecords);
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw FormatError("cannot write " + pathToUtf8(path));
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!out)
            throw FormatError("write failed for " + pathToUtf8(path));
    }
    std::filesystem::rename(tmp, path);
}

} // namespace fo4mx
