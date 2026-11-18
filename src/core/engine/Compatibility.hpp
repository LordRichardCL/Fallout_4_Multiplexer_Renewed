#pragma once

#include "engine/Provenance.hpp"

#include <functional>
#include <string>
#include <vector>

namespace fo4mx {

// Compatibility analysis (docs/COMPATIBILITY_PATCH_RESEARCH.md, section 8.2): for each change the winning
// version does not hold, whether carrying it forward into a patch matches a documented low-risk rule. A
// layer above provenance: it reads the facts and adds a judgement, never changes them. Nothing is merged
// or written from it.

enum class CompatClass : uint8_t {
    Green,   // matches a documented low-risk rule and every check passed (not a guarantee)
    Yellow,  // review required: competing, removed or replaced later, a dependency, flags, order
    Red,     // detected, never carried automatically: scripts, conditions, templates, quests, cells...
    Unknown, // no rule for this record type or path: detection only
};
const char* compatClassName(CompatClass c);

enum class CompatKind : uint8_t {
    Independent, // no other version changed it differently
    Competing,   // another version not made against it changed it differently
    Dependent,   // needs a master the winner's plugin does not have and the chain does not provide
    Ambiguous,   // a later version made against it differs (removed, replaced, reverted), or order/flags
    Complex,     // a structure with no validated merge rule (scripts, conditions, templates...)
    Unsupported, // undescribed data, a missing reference, or a record type without a handler
};
const char* compatKindName(CompatKind k);

struct CompatFinding {
    CompatClass cls = CompatClass::Unknown;
    CompatKind kind = CompatKind::Unsupported;
    std::string rule;                       // the rule that decided it (see classifyChange)
    std::vector<PluginIndex> requiredMasters; // plugins a patch carrying it would have to master
};

struct CompatContext {
    Signature record;
    bool fieldsMerge = false;                      // the record type has a field-by-field layout (Specialised)
    PluginIndex winner = kInvalidPlugin;
    std::vector<PluginIndex> chainPlugins;         // every plugin with a version of the record
    std::function<bool(PluginIndex, PluginIndex)> masters; // (plugin, other): other is among plugin's masters
    std::function<bool(FormKey)> exists;           // a record with this identity is in the load order
};

// Judges one change (lost or not; a change the winner holds is Green/Independent by definition when its
// field could be judged at all). `chain` and `defs` give the field's kind.
CompatFinding classifyChange(const ChangeEvent& change, const std::vector<FieldSet>& chain,
                             const RecordDefinitions& defs, const CompatContext& ctx);

} // namespace fo4mx
