#pragma once

#include "esp/Signature.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// Where a record lives in a plugin file.
enum class RecordPlacement : uint8_t {
    Header,     // TES4
    Top,        // in its own top-level group
    Cell,       // CELL: interior block groups, or inside its worldspace
    CellChild,  // placed references, landscape, navmeshes: in a cell's child groups
    TopicChild, // INFO: in its dialogue topic's child group
};
const char* placementName(RecordPlacement p);

// How the compiler treats a record type. Set per type in the catalogue; never inferred.
enum class MergeStrategy : uint8_t {
    Specialised, // described field by field: compiled, merged and patched
    Preserve,    // never written; a plugin containing it stays in the load order as it is
};
const char* strategyName(MergeStrategy s);

struct CatalogueEntry {
    Signature sig;
    std::string name;
    std::string category; // display grouping only
    RecordPlacement placement = RecordPlacement::Top;
    MergeStrategy strategy = MergeStrategy::Preserve;
    int groupOrder = -1;      // top-level group position in a written plugin, -1 = not a top-level group
    int allocationOrder = -1; // output grouping for allocation, -1 = the shared last group
    std::string notes;
};

// The record types of Fallout 4 (resources/record_catalogue.csv, embedded at build time). The
// single source for record names, categories, placement, group order and merge strategy.
class RecordCatalogue {
public:
    static const RecordCatalogue& get();
    // Parses catalogue text; throws FormatError on malformed rows.
    static RecordCatalogue parse(std::string_view csv);

    const CatalogueEntry* find(Signature s) const;
    bool contains(Signature s) const { return find(s) != nullptr; }
    const std::vector<CatalogueEntry>& entries() const { return entries_; }
    // Top-level group signatures in the order a plugin stores them.
    const std::vector<Signature>& groupOrder() const { return groupOrder_; }
    // Number of allocation groups: every ranked type, plus one shared group for the rest.
    int allocationGroups() const { return allocationGroups_; }

private:
    std::vector<CatalogueEntry> entries_;
    std::unordered_map<Signature, size_t> index_;
    std::vector<Signature> groupOrder_;
    int allocationGroups_ = 1;
};

} // namespace fo4mx
