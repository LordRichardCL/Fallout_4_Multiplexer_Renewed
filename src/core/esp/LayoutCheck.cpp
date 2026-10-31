#include "esp/LayoutCheck.hpp"

#include "esp/PluginWriter.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

namespace fo4mx {

namespace {

constexpr size_t kMaxProblems = 20;

std::string groupName(int32_t type, uint32_t label)
{
    switch (type) {
    case GroupType::Top: return "top group " + Signature(label).str();
    case GroupType::WorldChildren: return "worldspace children of " + formatFormId(label);
    case GroupType::InteriorBlock: return "interior block " + std::to_string(static_cast<int32_t>(label));
    case GroupType::InteriorSubBlock: return "interior sub-block " + std::to_string(static_cast<int32_t>(label));
    case GroupType::ExteriorBlock:
    case GroupType::ExteriorSubBlock: {
        auto g = gridFromLabel(label);
        return std::string(type == GroupType::ExteriorBlock ? "exterior block " : "exterior sub-block ") +
               std::to_string(g.x) + "," + std::to_string(g.y);
    }
    case GroupType::CellChildren: return "cell children of " + formatFormId(label);
    case GroupType::TopicChildren: return "topic children of " + formatFormId(label);
    case GroupType::CellPersistent: return "persistent children of " + formatFormId(label);
    case GroupType::CellTemporary: return "temporary children of " + formatFormId(label);
    }
    return "group type " + std::to_string(type) + " " + formatFormId(label);
}

std::vector<uint8_t> canonical(const Record& r)
{
    BinaryWriter w;
    r.write(w, false);
    return w.take();
}

using GroupKey = std::pair<int32_t, uint32_t>;
// Groups and records in file order, for the order comparison.
std::pair<std::vector<GroupKey>, std::vector<uint32_t>> sequence(const PluginFile& f)
{
    std::pair<std::vector<GroupKey>, std::vector<uint32_t>> out;
    f.forEachGroup([&](const GroupHeader& g, uint32_t) { out.first.emplace_back(g.groupType, g.label); });
    f.forEachRecord([&](const RecordLocation& loc) {
        out.second.push_back(loc.header.formId);
        return true;
    });
    return out;
}

} // namespace

LayoutCheck checkLayout(const PluginFile& file, bool rebuild)
{
    LayoutCheck out;
    auto problem = [&](std::string text) {
        if (out.problems.size() < kMaxProblems)
            out.problems.push_back(std::move(text));
        else if (out.problems.size() == kMaxProblems)
            out.problems.push_back("(further differences not listed)");
    };

    std::multiset<GroupKey> groups;
    file.forEachGroup([&](const GroupHeader& g, uint32_t depth) {
        ++out.groups;
        groups.insert({g.groupType, g.label});
        if (depth == 0 && (Signature(g.label) == sig::CELL || Signature(g.label) == Signature("WRLD") ||
                           Signature(g.label) == Signature("DIAL")))
            out.nested = true;
        if (g.groupType > GroupType::CellTemporary)
            problem("contains a " + groupName(g.groupType, g.label) + ", which this app does not write");
    });
    if (!out.nested)
        return out;

    // Placement.
    struct Placed {
        Signature sig;
        Placement placement;
        std::vector<uint8_t> bytes;
    };
    std::map<uint32_t, Placed> original;
    std::vector<std::pair<Record, Placement>> records;
    file.forEachRecord([&](const RecordLocation& loc) {
        ++out.records;
        const auto placement = loc.placement();
        const auto label = loc.header.signature.str() + " " + formatFormId(loc.header.formId);
        if (loc.header.signature == sig::CELL) {
            ++out.cells;
            auto cell = file.recordAt(loc.offset);
            if (placement.kind == Placement::Kind::InteriorCell) {
                if (loc.blockType != GroupType::InteriorBlock ||
                    static_cast<int32_t>(loc.blockLabel) != interiorBlockOf(loc.header.formId) ||
                    static_cast<int32_t>(loc.subBlockLabel) != interiorSubBlockOf(loc.header.formId))
                    problem(label + " is in " + groupName(loc.blockType, loc.blockLabel) + " / sub-block " +
                            std::to_string(static_cast<int32_t>(loc.subBlockLabel)) + "; the writer puts it in block " +
                            std::to_string(interiorBlockOf(loc.header.formId)) + " / sub-block " +
                            std::to_string(interiorSubBlockOf(loc.header.formId)));
            } else if (loc.header.flags & RecordFlags::Persistent) {
                if (loc.groupType != GroupType::WorldChildren)
                    problem(label + " is a persistent worldspace cell inside " + groupName(loc.groupType, loc.groupLabel));
            } else if (auto grid = cellGrid(cell)) {
                const auto sub = subBlockOf(*grid), block = blockOf(sub);
                if (loc.blockType != GroupType::ExteriorBlock || loc.blockLabel != gridLabel(block) ||
                    loc.subBlockLabel != gridLabel(sub))
                    problem(label + " at " + std::to_string(grid->x) + "," + std::to_string(grid->y) + " is in " +
                            groupName(loc.blockType, loc.blockLabel) + "; the writer puts it in " +
                            groupName(GroupType::ExteriorBlock, gridLabel(block)) + " / " +
                            groupName(GroupType::ExteriorSubBlock, gridLabel(sub)));
            } else {
                problem(label + " is a worldspace cell with neither a grid position nor the persistent flag");
            }
            if (rebuild)
                records.emplace_back(std::move(cell), placement);
        } else {
            if (placement.kind == Placement::Kind::CellChild) {
                ++out.children;
                if (loc.groupType != GroupType::CellPersistent && loc.groupType != GroupType::CellTemporary)
                    problem(label + " is directly in the " + groupName(loc.groupType, loc.groupLabel) +
                            ", not in a persistent or temporary group");
            } else if (placement.kind == Placement::Kind::TopicChild) {
                ++out.topicChildren;
            } else if (loc.groupType != GroupType::Top) {
                problem(label + " is in the " + groupName(loc.groupType, loc.groupLabel) +
                        " but belongs to no cell or topic");
            }
            if (rebuild)
                records.emplace_back(file.recordAt(loc.offset), placement);
        }
        if (rebuild) {
            if (!original.emplace(loc.header.formId, Placed{loc.header.signature, placement, canonical(records.back().first)}).second)
                problem(label + " is in the plugin twice");
        }
        return true;
    });
    if (!rebuild || !out.problems.empty())
        return out;

    // Rebuild.
    out.rebuilt = true;
    std::vector<uint8_t> image;
    try {
        PluginWriter w;
        w.setHeader(file.header());
        for (auto& [r, p] : records)
            w.addRecord(std::move(r), p);
        records.clear();
        image = w.build();
    } catch (const std::exception& e) {
        problem(std::string("the writer refused the plugin: ") + e.what());
        return out;
    }
    const auto again = PluginFile::fromBytes(std::move(image));
    size_t seen = 0;
    again.forEachRecord([&](const RecordLocation& loc) {
        ++seen;
        const auto label = loc.header.signature.str() + " " + formatFormId(loc.header.formId);
        auto it = original.find(loc.header.formId);
        if (it == original.end()) {
            problem(label + " appeared in the rebuilt plugin");
        } else {
            if (!(it->second.placement == loc.placement()))
                problem(label + " moved from a " + placementKindName(it->second.placement.kind) + " to a " +
                        placementKindName(loc.placement().kind) + " (parent " + formatFormId(loc.placement().parent) +
                        ")");
            if (canonical(again.recordAt(loc.offset)) != it->second.bytes)
                problem(label + " changed content");
        }
        return true;
    });
    if (seen != original.size())
        problem(std::to_string(original.size()) + " records written, " + std::to_string(seen) + " read back");

    std::multiset<GroupKey> rebuiltGroups;
    again.forEachGroup([&](const GroupHeader& g, uint32_t) { rebuiltGroups.insert({g.groupType, g.label}); });
    std::vector<GroupKey> missing, extra;
    std::set_difference(groups.begin(), groups.end(), rebuiltGroups.begin(), rebuiltGroups.end(), std::back_inserter(missing));
    std::set_difference(rebuiltGroups.begin(), rebuiltGroups.end(), groups.begin(), groups.end(), std::back_inserter(extra));
    for (const auto& [t, l] : missing)
        problem("the original has a " + groupName(t, l) + " that the rebuild does not");
    for (const auto& [t, l] : extra)
        problem("the rebuild has a " + groupName(t, l) + " that the original does not");
    out.sameOrder = out.problems.empty() && sequence(file) == sequence(again);
    return out;
}

} // namespace fo4mx
