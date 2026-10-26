#pragma once

#include "engine/Normalizer.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fo4mx {

enum class FieldShape : uint8_t {
    Scalar,       // one subrecord
    Region,       // a run of subrecords compared as a unit
    PackedArray,  // elements packed into one subrecord (KWDA, FVPA)
    RepeatedList, // one subrecord (plus companions) per element (LNAM, LVLO+COED)
};

struct ListItem {
    std::vector<NormSub> subs; // PackedArray: one element pseudo-subrecord
};

// One logical field of a record, the unit of conflict detection and merging.
struct Field {
    std::string key;   // stable identity, e.g. "FULL#0", "region:Model#0", "list:LVLO"
    std::string label; // display name
    FieldShape shape = FieldShape::Scalar;
    Signature sig;
    MergeMode merge = MergeMode::Scalar;
    std::vector<NormSub> subs;   // Scalar / Region
    std::vector<ListItem> items; // PackedArray / RepeatedList
    Signature countSig;
    uint8_t countWidth = 0;
    uint16_t stride = 0;
    bool hadCount = false;

    bool isList() const { return shape == FieldShape::PackedArray || shape == FieldShape::RepeatedList; }
};

struct FieldSet {
    FormKey key;
    Signature sig;
    PluginIndex plugin = kInvalidPlugin;
    uint16_t formVersion = 0;
    std::vector<Field> fields; // in record order; the first is the "@flags" pseudo field

    const Field* find(const std::string& key) const;
    Field* find(const std::string& key);
    uint32_t flags() const;
};

FieldSet splitFields(const NormRecord& rec, const RecordDefinitions& defs);
std::vector<NormSub> joinFields(const FieldSet& set);

bool itemEquivalent(const ListItem& a, const ListItem& b);
bool fieldEquivalent(const Field* a, const Field* b); // null = field absent
// Key used by MergeMode::Keyed lists (the first FormID of the item).
FormKey itemKey(const ListItem& item);

// Human readable value, using `label` to describe referenced forms.
std::string describeField(const Field* f, const std::function<std::string(FormKey)>& label, size_t maxLength = 240);

// All FormKeys referenced by the set.
std::vector<std::pair<FormKey, Signature>> collectRefs(const FieldSet& set, bool includeSoft = false);

} // namespace fo4mx
