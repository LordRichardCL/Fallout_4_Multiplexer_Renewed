#pragma once

#include "engine/FieldModel.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fo4mx {

// Change provenance (docs/COMPATIBILITY_PATCH_RESEARCH.md, sections 7 and 8.1): what each version of a
// record changed against the version it was made against (its parent version), and what became of
// each change by the winning version. Detection only: observable facts, never the author's intent.

enum class ChangeOp : uint8_t {
    Set,     // a value set or changed
    Add,     // a value or list element added
    Remove,  // a value or list element removed against the parent version
    Modify,  // a keyed list element's values changed (same key)
    Reorder, // an ordered list's elements in another order (ordered lists only, e.g. FLST)
};
const char* changeOpName(ChangeOp op);

enum class ChangeStatus : uint8_t {
    Survives,     // the winning version holds it
    Replaced,     // a later version made against it holds another value
    Reverted,     // a later version made against it holds the defining version's value again
    RemovedLater, // a later version made against it no longer contains the element
    OmittedLater, // the winning version was not made against it: it never had the change to keep
};
const char* changeStatusName(ChangeStatus s);
inline bool isLost(ChangeStatus s) { return s != ChangeStatus::Survives; }

struct ChangeEvent {
    std::string fieldKey; // Field::key
    std::string label;    // Field::label
    std::string item;     // the list element, described ("" for a whole field)
    size_t version = 0;   // chain index that made the change
    ChangeOp op = ChangeOp::Set;
    ChangeStatus status = ChangeStatus::Survives;
    std::string before, after; // described values (the parent version's, this version's)
    std::vector<size_t> keptBy;       // later versions made against it that still hold it
    int lostAt = -1;                  // the first version on the winner's line that does not hold it
    std::vector<size_t> competesWith; // versions not made against each other that changed it differently
    int duplicateOf = -1;             // a version not made against this one that added the same element
    bool opaque = false;              // the field holds undescribed bytes: the difference is not understood
    std::string identity;             // list elements: the element's identity (key, or value and repeat number)
    std::vector<FormKey> references;  // records the changed value or added element names
};

struct RecordProvenance {
    std::vector<size_t> parents; // per chain index, the version it was made against (0 = the defining one)
    std::vector<ChangeEvent> events;

    // True when `b` was made with `a`'s version in its masters, directly or through others (or b == a).
    bool madeAgainst(size_t b, size_t a) const;
    size_t lostCount() const;
};

// Lists whose element order is data (xEdit keeps them unsorted): FormID lists (FLST LNAM). Other lists the
// field model merges are sorted by xEdit, so their order is not a change.
bool listOrderMatters(Signature record, const Field& field);

// chain[0] is the defining version, chain.back() the winner; `parents` as Session::overrideParents
// (empty: every version against chain[0]). `label` describes referenced forms.
RecordProvenance traceProvenance(const std::vector<FieldSet>& chain, const std::vector<size_t>& parents,
                                 const std::function<std::string(FormKey)>& label);

// Carries `changes` (events of a traceProvenance over `chain`) into `result`, normally a copy of the winning
// version (docs/COMPATIBILITY_PATCH_RESEARCH.md, phase 8): a whole field takes the change's value (or is
// removed), a list element is added, removed or replaced by its identity, an ordered list takes the change's
// order for the elements both hold. Afterwards every change is checked against the result; the indices
// (into `changes`) of those the result does not hold are returned, so the caller can refuse the record.
std::vector<size_t> applyChanges(FieldSet& result, const std::vector<FieldSet>& chain,
                                 const std::vector<const ChangeEvent*>& changes);

} // namespace fo4mx
