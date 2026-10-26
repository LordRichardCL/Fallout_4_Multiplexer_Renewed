#pragma once

#include "engine/MergeEngine.hpp"
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
};
const char* conflictTypeName(ConflictType t);

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
    std::string summary;
    std::vector<ConflictField> fields;
};

struct ConflictReport {
    std::vector<ConflictEntry> entries;
    std::array<size_t, 4> byType{};
    size_t identicalOverrides = 0; // groups whose overrides are all ITMs
    size_t analysedGroups = 0;
    size_t failedGroups = 0;

    size_t conflictCount() const { return byType[1] + byType[2] + byType[3]; }
    const ConflictEntry* find(FormKey key) const;
};

struct ConflictOptions {
    bool analyseAllRecordTypes = true; // field-level analysis for non-compilable types too
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
