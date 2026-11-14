#include "esp/PluginWriter.hpp"

#include "esp/FormId.hpp"
#include "esp/RecordCatalogue.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <map>

namespace fo4mx {

const std::vector<Signature>& PluginWriter::topLevelOrder()
{
    return RecordCatalogue::get().groupOrder();
}

std::vector<const PluginWriter::Entry*> PluginWriter::orderByPreviousInfo(const std::vector<const Entry*>& in)
{
    std::unordered_map<uint32_t, size_t> index;
    for (size_t i = 0; i < in.size(); ++i)
        index.emplace(in[i]->record.header.formId, i);
    std::vector<size_t> previous(in.size(), SIZE_MAX);
    for (size_t i = 0; i < in.size(); ++i)
        if (const auto* p = in[i]->record.find(Signature("PNAM")); p && p->data.size() >= 4) {
            uint32_t id = 0;
            std::memcpy(&id, p->data.data(), 4);
            if (auto it = index.find(id); it != index.end() && it->second != i)
                previous[i] = it->second;
        }
    std::vector<const Entry*> out;
    out.reserve(in.size());
    std::vector<uint8_t> state(in.size(), 0); // 0 not placed, 1 placing, 2 placed
    std::function<void(size_t)> place = [&](size_t i) {
        if (state[i])
            return; // placed, or a cycle: the given order decides
        state[i] = 1;
        if (previous[i] != SIZE_MAX)
            place(previous[i]);
        state[i] = 2;
        out.push_back(in[i]);
    };
    for (size_t i = 0; i < in.size(); ++i)
        place(i);
    return out;
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
    const Signature QUST("QUST");
    std::map<uint32_t, const Entry*> cells, worlds, topics, quests; // quests: only for the duplicate check
    for (const auto& e : records_) {
        const auto s = e.record.signature();
        auto& index = s == sig::CELL ? cells : s == WRLD ? worlds : s == DIAL ? topics : quests;
        if ((s == sig::CELL || s == WRLD || s == DIAL || s == QUST) && !index.emplace(e.record.header.formId, &e).second)
            throw FormatError(label(e) + " is in the plugin twice");
    }

    std::map<Signature, std::vector<const Entry*>> top;
    std::vector<const Entry*> interiorCells;
    std::map<uint32_t, std::vector<const Entry*>> worldCells, topicChildren, questChildren;
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
        case Kind::QuestChild:
            // A plugin may add topics to another plugin's quest without overriding the quest itself:
            // the quest's group is then written on its own.
            questChildren[p.parent].push_back(&e);
            break;
        }
    }
    if (!interiorCells.empty())
        top[sig::CELL]; // the CELL group holds interior cells only
    if (!questChildren.empty())
        top[QUST];

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

    auto writeTopic = [&](const Entry* e) { // a topic and its responses
        auto it = topicChildren.find(e->record.header.formId);
        if (it == topicChildren.end()) {
            writeRecord(e);
            return;
        }
        // The game resolves an INFO's previous INFO (PNAM) when it loads the INFO, so one that points to
        // another INFO of this group must come after it (xEdit sorts them the same way when it saves).
        // Otherwise the order is kept as added.
        const auto infos = orderByPreviousInfo(it->second);
        // TIFC is the number of responses the topic has in this plugin: the ones written here.
        if (const auto* tifc = e->record.find(Signature("TIFC")); tifc && tifc->data.size() == 4) {
            uint32_t count = 0;
            std::memcpy(&count, tifc->data.data(), 4);
            if (count != infos.size()) {
                Entry fixed = *e;
                const uint32_t n = static_cast<uint32_t>(infos.size());
                std::memcpy(fixed.record.find(Signature("TIFC"))->data.data(), &n, 4);
                writeRecord(&fixed);
            } else {
                writeRecord(e);
            }
        } else {
            writeRecord(e);
        }
        size_t tg = open(e->record.header.formId, GroupType::TopicChildren);
        for (const auto* info : infos)
            writeRecord(info);
        close(tg);
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
        } else if (s == QUST) {
            // Quests in FormID order, each followed by its quest children group (also when the quest
            // itself is not in the plugin).
            std::map<uint32_t, const Entry*> byId;
            for (const auto* e : top[s])
                byId[e->record.header.formId] = e;
            for (const auto& [id, _] : questChildren)
                byId.emplace(id, nullptr);
            for (const auto& [id, quest] : byId) {
                if (quest)
                    writeRecord(quest);
                auto it = questChildren.find(id);
                if (it == questChildren.end())
                    continue;
                byFormId(it->second);
                size_t qg = open(id, GroupType::QuestChildren);
                for (const auto* child : it->second)
                    writeTopic(child); // DIAL with its responses; DLBR and SCEN alone
                close(qg);
            }
        } else {
            auto& list = top[s];
            byFormId(list);
            for (const auto* e : list) {
                if (s == DIAL) {
                    writeTopic(e);
                    continue;
                }
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
                        // The persistent cell (and a cell without a grid position) sits directly in the
                        // worldspace's group.
                        if ((c->record.header.flags & RecordFlags::Persistent) || !cellGrid(c->record)) {
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
