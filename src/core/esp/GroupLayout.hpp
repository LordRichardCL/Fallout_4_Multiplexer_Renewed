#pragma once

#include "esp/Record.hpp"

#include <cstdint>
#include <optional>

namespace fo4mx {

// Group types of a Fallout 4 plugin (the GRUP header's type field), as xEdit reads and writes them.
namespace GroupType {
inline constexpr int32_t Top = 0;              // label: record signature
inline constexpr int32_t WorldChildren = 1;    // label: WRLD FormID
inline constexpr int32_t InteriorBlock = 2;    // label: last decimal digit of the cell's object ID
inline constexpr int32_t InteriorSubBlock = 3; // label: second-to-last decimal digit
inline constexpr int32_t ExteriorBlock = 4;    // label: block grid (Y low word, X high word)
inline constexpr int32_t ExteriorSubBlock = 5; // label: sub-block grid (Y low word, X high word)
inline constexpr int32_t CellChildren = 6;     // label: CELL FormID
inline constexpr int32_t TopicChildren = 7;    // label: DIAL FormID
inline constexpr int32_t CellPersistent = 8;   // label: CELL FormID
inline constexpr int32_t CellTemporary = 9;    // label: CELL FormID
inline constexpr int32_t QuestChildren = 10;   // label: QUST FormID (Fallout 4: the quest's DIAL, DLBR, SCEN)
} // namespace GroupType

// Where a record sits in the group tree.
struct Placement {
    enum class Kind : uint8_t {
        Top,          // in its signature's top-level group (WRLD and DIAL records too)
        InteriorCell, // CELL in the CELL group's block and sub-block
        WorldCell,    // CELL inside a worldspace: its persistent cell, or in a block/sub-block by grid
        CellChild,    // placed reference, landscape, navmesh... in a cell's persistent or temporary group
        TopicChild,   // INFO in its topic's group
        QuestChild,   // DIAL, DLBR or SCEN in its quest's group
    };
    Kind kind = Kind::Top;
    uint32_t parent = 0;     // WorldCell: WRLD; CellChild: CELL; TopicChild: DIAL; QuestChild: QUST (as in the file)
    bool persistent = false; // CellChild: persistent (group 8) or temporary (group 9)

    bool operator==(const Placement&) const = default;
};
const char* placementKindName(Placement::Kind k);

struct GridCell {
    int32_t x = 0, y = 0;
    bool operator==(const GridCell&) const = default;
};

// Exterior cell grid from XCLC (x, y as int32).
std::optional<GridCell> cellGrid(const Record& cell);
// Rounds towards negative infinity, as xEdit does: cell (-1, 9) is in sub-block (-1, 1).
GridCell subBlockOf(GridCell cell);
GridCell blockOf(GridCell subBlock);
uint32_t gridLabel(GridCell g);
GridCell gridFromLabel(uint32_t label);

int32_t interiorBlockOf(uint32_t formId);    // last decimal digit of the object ID
int32_t interiorSubBlockOf(uint32_t formId); // second-to-last decimal digit

// Order of records inside one group (xEdit): ROAD and LAND first, then CELL, PGRD and NAVM, then
// the rest; by FormID within each.
int recordSortPriority(Signature s);

} // namespace fo4mx
