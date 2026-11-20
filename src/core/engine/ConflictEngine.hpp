#pragma once

#include "engine/MergeEngine.hpp"
#include "engine/Compatibility.hpp"
#include "engine/Provenance.hpp"
#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <array>
#include <string>
#include <vector>

namespace fo4mx {

enum class ConflictType : uint8_t {
    None = 0,
    SimpleOverride = 1,  // Type 1: last plugin wins, nothing is lost
    CompatibleMerge = 2, // Type 2: plugins changed different fields
    HardConflict = 3,    // Type 3: plugins changed the same field differently
    RecordConflict = 4,  // a record type that is not merged: the winner drops another plugin's change
};
const char* conflictTypeName(ConflictType t);
// Display order, most serious first: hard conflicts, conflicts that are not merged, merges, overrides.
int conflictPriority(ConflictType t);

// How compilation treats the record type of a conflict.
enum class MergeHandling : uint8_t {
    Merged = 0,      // Specialised: fields merged
    NotCompiled = 1, // no layout or not enabled: kept in its plugin as it is
    WinnerOnly = 2,  // Winner: compiled, the winning version taken whole
};
const char* mergeHandlingName(MergeHandling h);

enum class ConflictSeverity : uint8_t { Low, Medium, High, Critical };
const char* conflictSeverityName(ConflictSeverity s);

struct ConflictCell {
    PluginIndex plugin = kInvalidPlugin;
    std::string value;
    CellStatus status = CellStatus::Same;
};

struct ConflictField {
    std::string key;
    std::string label;
    bool hard = false;
    bool list = false;
    std::string result;              // merged value (what compilation would produce)
    std::vector<ConflictCell> cells; // one per chain entry
};

// One change a version made against the version it was made from, and what became of it by the winner
// (engine/Provenance). Facts only: "omitted" means the winner was not made with the source as a master.
struct TracedChange {
    std::string fieldKey; // Field::key
    std::string changeId; // stable across scans and load orders (field, operation, source, element, value)
    std::string field;  // field label
    std::string item;   // list element (tokens until the report is finalised), "" for a whole field
    std::string before; // the parent version's value
    std::string after;  // the source's value
    std::string position; // an element added to an ordered list (FLST): where ("after <element>", "first")
    PluginIndex source = kInvalidPlugin;
    ChangeOp op = ChangeOp::Set;
    ChangeStatus status = ChangeStatus::Survives;
    PluginIndex lostAt = kInvalidPlugin;    // first version on the winner's line without it
    std::vector<PluginIndex> keptBy;        // later versions made against the source that still hold it
    std::vector<PluginIndex> competesWith;  // versions not made against each other that changed it differently
    PluginIndex duplicateOf = kInvalidPlugin; // an unrelated version that added the same element
    bool opaque = false;                    // undescribed bytes: the difference is not understood
    // Compatibility analysis (engine/Compatibility): whether carrying it forward matches a documented rule.
    CompatClass cls = CompatClass::Unknown;
    CompatKind kind = CompatKind::Unsupported;
    std::string rule;
    std::vector<PluginIndex> requiredMasters; // beyond the winner's masters
};

// The stable id of a traced change (`ev` traced with formToken labels; `source` = its version's plugin):
// a hash of field, operation, source plugin, element and value, independent of the load order.
std::string tracedChangeId(const Session& session, const ChangeEvent& ev, PluginIndex source);

struct ConflictEntry {
    uint32_t group = 0;
    FormKey key;
    Signature sig;
    std::string editorId;
    ConflictType type = ConflictType::None;
    ConflictSeverity severity = ConflictSeverity::Low;
    std::vector<PluginIndex> chain; // load order; front = defining plugin
    PluginIndex winner = kInvalidPlugin;
    std::vector<PluginIndex> itmPlugins;
    bool fieldsAnalysed = false;
    bool winnerLosesChanges = false;
    // Not Merged: classified record by record (never Compatible Merge or Hard Conflict).
    MergeHandling handling = MergeHandling::Merged;
    bool fieldsMerged() const { return handling == MergeHandling::Merged; }
    std::string summary;
    std::vector<ConflictField> fields;
    std::vector<TracedChange> changes; // provenance of every change (with field details)
    size_t lostChanges = 0;            // changes the winner does not hold
};

// An override identical to the version it was made against (xEdit: Identical To Master).
struct ItmRecord {
    PluginIndex plugin = kInvalidPlugin;
    FormKey key;
    Signature sig;
    bool cancelsChanges = false; // it is the winner and other plugins changed the record: loading last, it undoes them
};

struct ConflictReport {
    std::vector<ConflictEntry> entries;
    std::array<size_t, 5> byType{};
    size_t identicalOverrides = 0; // groups whose overrides are all ITMs
    size_t lostChanges = 0;        // changes, over every record, the winning version does not hold
    std::array<size_t, 4> lostByClass{}; // those changes by CompatClass (Green, Yellow, Red, Unknown)
    std::vector<ItmRecord> itms;   // every ITM override, in record order
    size_t analysedGroups = 0;
    size_t failedGroups = 0;
    size_t reusedGroups = 0; // restored from the build cache

    size_t conflictCount() const { return byType[1] + byType[2] + byType[3] + byType[4]; }
    const ConflictEntry* find(FormKey key) const;
};

struct ConflictOptions {
    bool analyseAllRecordTypes = true; // field differences for types that are not merged too (display only)
    bool keepFieldDetails = true;
    unsigned threads = 0;
};

// Stage 2: walks every override chain, determines the winning record, why it
// wins, and which fields each plugin changed.
class ConflictEngine {
public:
    static ConflictReport analyse(Session& session, const ConflictOptions& options = {},
                                  ProgressSink& progress = nullProgress());
    static ConflictEntry analyseGroup(Session& session, uint32_t groupIndex, bool fieldLevel, bool keepDetails);
};

} // namespace fo4mx
