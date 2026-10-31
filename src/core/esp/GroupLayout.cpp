#include "esp/GroupLayout.hpp"

#include "esp/FormId.hpp"

namespace fo4mx {

const char* placementKindName(Placement::Kind k)
{
    switch (k) {
    case Placement::Kind::Top: return "top";
    case Placement::Kind::InteriorCell: return "interior cell";
    case Placement::Kind::WorldCell: return "worldspace cell";
    case Placement::Kind::CellChild: return "cell child";
    case Placement::Kind::TopicChild: return "topic child";
    case Placement::Kind::QuestChild: return "quest child";
    }
    return "";
}

std::optional<GridCell> cellGrid(const Record& cell)
{
    const auto* xclc = cell.find(Signature("XCLC"));
    if (!xclc || xclc->data.size() < 8)
        return std::nullopt;
    return GridCell{loadLE<int32_t>(xclc->data.data()), loadLE<int32_t>(xclc->data.data() + 4)};
}

namespace {
int32_t floorDiv(int32_t v, int32_t d)
{
    int32_t q = v / d;
    if (v < 0 && v % d != 0)
        --q;
    return q;
}
} // namespace

GridCell subBlockOf(GridCell cell)
{
    return {floorDiv(cell.x, 8), floorDiv(cell.y, 8)};
}

GridCell blockOf(GridCell subBlock)
{
    return {floorDiv(subBlock.x, 4), floorDiv(subBlock.y, 4)};
}

uint32_t gridLabel(GridCell g)
{
    return static_cast<uint32_t>(static_cast<uint16_t>(g.y)) | (static_cast<uint32_t>(static_cast<uint16_t>(g.x)) << 16);
}

GridCell gridFromLabel(uint32_t label)
{
    return {static_cast<int16_t>(label >> 16), static_cast<int16_t>(label & 0xFFFF)};
}

int32_t interiorBlockOf(uint32_t formId)
{
    return static_cast<int32_t>(objectIdOf(formId) % 10);
}

int32_t interiorSubBlockOf(uint32_t formId)
{
    return static_cast<int32_t>((objectIdOf(formId) / 10) % 10);
}

int recordSortPriority(Signature s)
{
    if (s == Signature("ROAD") || s == Signature("LAND"))
        return -2;
    if (s == Signature("CELL") || s == Signature("PGRD") || s == Signature("NAVM"))
        return -1;
    return 0;
}

} // namespace fo4mx
