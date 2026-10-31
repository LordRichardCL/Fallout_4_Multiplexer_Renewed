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

void PluginWriter::addRecord(Record record, Placement placement)
{
    records_.push_back({std::move(record), placement});
}

std::vector<uint8_t> PluginWriter::build(bool compressLargeRecords)
{
    using Kind = Placement::Kind;
    const Signature WRLD("WRLD"), DIAL("DIAL");
    auto label = [](const Entry& e) { return e.record.signature().str() + " " + formatFormId(e.record.header.formId); };

    // Parents present in this plugin.
    std::map<uint32_t, const Entry*> cells, worlds, topics;
    for (const auto& e : records_) {
        const auto s = e.record.signature();
        auto& index = s == sig::CELL ? cells : s == WRLD ? worlds : topics;
        if ((s == sig::CELL || s == WRLD || s == DIAL) && !index.emplace(e.record.header.formId, &e).second)
            throw FormatError(label(e) + " is in the plugin twice");
    }

    std::map<Signature, std::vector<const Entry*>> top;
    std::vector<const Entry*> interiorCells;
    std::map<uint32_t, std::vector<const Entry*>> worldCells, topicChildren;
    std::map<uint32_t, std::pair<std::vector<const Entry*>, std::vector<const Entry*>>> cellChildren; // persistent, temporary
    for (const auto& e : records_) {
        const auto s = e.record.signature();
        const auto& p = e.placement;
        if ((s == sig::CELL) != (p.kind == Kind::InteriorCell || p.kind == Kind::WorldCell))
            throw FormatError(label(e) + " cannot be placed as a " + placementKindName(p.kind));
        switch (p.kind) {
        case Kind::Top:
            if (const auto* c = RecordCatalogue::get().find(s);
                c && (c->placement == RecordPlacement::CellChild || c->placement == RecordPlacement::TopicChild))
                throw FormatError(label(e) + " belongs in a " +
                                  (c->placement == RecordPlacement::CellChild ? "cell" : "dialogue topic") +
                                  " and cannot be written as a top-level group");
            top[s].push_back(&e);
            break;
        case Kind::InteriorCell:
            interiorCells.push_back(&e);
            break;
        case Kind::WorldCell:
            if (!worlds.count(p.parent))
                throw FormatError(label(e) + ": its worldspace " + formatFormId(p.parent) + " is not in the plugin");
            if (!(e.record.header.flags & RecordFlags::Persistent) && !cellGrid(e.record))
                throw FormatError(label(e) + ": an exterior cell needs its grid position (XCLC)");
            worldCells[p.parent].push_back(&e);
            break;
        case Kind::CellChild:
            if (!cells.count(p.parent))
                throw FormatError(label(e) + ": its cell " + formatFormId(p.parent) + " is not in the plugin");
            (p.persistent ? cellChildren[p.parent].first : cellChildren[p.parent].second).push_back(&e);
            break;
        case Kind::TopicChild:
            if (!topics.count(p.parent))
                throw FormatError(label(e) + ": its topic " + formatFormId(p.parent) + " is not in the plugin");
            topicChildren[p.parent].push_back(&e);
            break;
        }
    }
    if (!interiorCells.empty())
        top[sig::CELL]; // the CELL group holds interior cells only

    auto byFormId = [](std::vector<const Entry*>& list) {
        std::stable_sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) {
            return std::make_pair(recordSortPriority(a->record.signature()), a->record.header.formId) <
                   std::make_pair(recordSortPriority(b->record.signature()), b->record.header.formId);
        });
    };

    BinaryWriter body;
    uint32_t groupCount = 0;
    uint32_t maxObjectId = 0;
    const uint32_t selfIndex = static_cast<uint32_t>(header_.masters.size());
    auto open = [&](uint32_t groupLabel, int32_t type) {
        size_t start = body.size();
        GroupHeader g;
        g.label = groupLabel;
        g.groupType = type;
        g.write(body);
        ++groupCount;
        return start;
    };
    auto close = [&](size_t start) { body.patch<uint32_t>(start + 4, static_cast<uint32_t>(body.size() - start)); };
    auto writeRecord = [&](const Entry* e) {
        const auto& r = e->record;
        bool compress = compressLargeRecords && r.serializeSubrecords().size() > 16 * 1024;
        r.write(body, compress);
        if (masterIndexOf(r.header.formId) == selfIndex)
            maxObjectId = std::max(maxObjectId, objectIdOf(r.header.formId));
    };
    auto writeCell = [&](const Entry* cell) {
        writeRecord(cell);
        auto it = cellChildren.find(cell->record.header.formId);
        if (it == cellChildren.end())
            return;
        const uint32_t id = cell->record.header.formId;
        size_t children = open(id, GroupType::CellChildren);
        for (auto [list, type] : {std::pair{&it->second.first, GroupType::CellPersistent},
                                  std::pair{&it->second.second, GroupType::CellTemporary}}) {
            if (list->empty())
                continue;
            byFormId(*list);
            size_t g = open(id, type);
            for (const auto* e : *list)
                writeRecord(e);
            close(g);
        }
        close(children);
    };

    std::vector<Signature> order;
    for (auto s : topLevelOrder())
        if (top.count(s))
            order.push_back(s);
    for (const auto& [s, _] : top)
        if (std::find(order.begin(), order.end(), s) == order.end())
            order.push_back(s);

    for (auto s : order) {
        size_t group = open(s.value, GroupType::Top);
        if (s == sig::CELL) {
            std::map<int32_t, std::map<int32_t, std::vector<const Entry*>>> blocks;
            for (const auto* c : interiorCells)
                blocks[interiorBlockOf(c->record.header.formId)][interiorSubBlockOf(c->record.header.formId)].push_back(c);
            for (auto& [block, subBlocks] : blocks) {
                size_t bg = open(static_cast<uint32_t>(block), GroupType::InteriorBlock);
                for (auto& [subBlock, list] : subBlocks) {
                    size_t sg = open(static_cast<uint32_t>(subBlock), GroupType::InteriorSubBlock);
                    byFormId(list);
                    for (const auto* c : list)
                        writeCell(c);
                    close(sg);
                }
                close(bg);
            }
        } else {
            auto& list = top[s];
            byFormId(list);
            for (const auto* e : list) {
                writeRecord(e);
                const uint32_t id = e->record.header.formId;
                if (s == WRLD) {
                    auto it = worldCells.find(id);
                    if (it == worldCells.end())
                        continue;
                    size_t wg = open(id, GroupType::WorldChildren);
                    std::vector<const Entry*> persistent;
                    std::map<std::pair<int32_t, int32_t>, std::map<std::pair<int32_t, int32_t>, std::vector<const Entry*>>> blocks;
                    for (const auto* c : it->second) {
                        if (c->record.header.flags & RecordFlags::Persistent) {
                            persistent.push_back(c);
                            continue;
                        }
                        const GridCell sub = subBlockOf(*cellGrid(c->record)), block = blockOf(sub);
                        blocks[{block.x, block.y}][{sub.x, sub.y}].push_back(c);
                    }
                    byFormId(persistent);
                    for (const auto* c : persistent)
                        writeCell(c);
                    for (auto& [block, subBlocks] : blocks) {
                        size_t bg = open(gridLabel({block.first, block.second}), GroupType::ExteriorBlock);
                        for (auto& [sub, cellsIn] : subBlocks) {
                            size_t sg = open(gridLabel({sub.first, sub.second}), GroupType::ExteriorSubBlock);
                            byFormId(cellsIn);
                            for (const auto* c : cellsIn)
                                writeCell(c);
                            close(sg);
                        }
                        close(bg);
                    }
                    close(wg);
                } else if (s == DIAL) {
                    auto it = topicChildren.find(id);
                    if (it == topicChildren.end())
                        continue;
                    size_t tg = open(id, GroupType::TopicChildren);
                    for (const auto* info : it->second) // order matters to the game: as added
                        writeRecord(info);
                    close(tg);
                }
            }
        }
        close(group);
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
