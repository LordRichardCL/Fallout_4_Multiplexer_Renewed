#pragma once

#include "engine/FieldModel.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/GroupLayout.hpp"
#include "esp/PluginHeader.hpp"
#include "util/Progress.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fo4mx {

enum class AllocationStrategy : uint8_t {
    ByRecordType, // Strategy A
    Balanced,     // Strategy B
    UserDefined,  // Strategy C
};
const char* allocationStrategyName(AllocationStrategy s);

// Strategy C rule: records whose signature or source plugin matches `pattern`
// go to output plugin `output` (1-based).
struct AllocationRule {
    std::string pattern;
    int output = 1;
};

struct CompileOptions {
    int outputCount = 20;
    // Automatic: the fewest output plugins whose master lists fit (outputCount is then the result).
    bool autoOutputs = false;
    // Output plugins are <prefix>01.esp, <prefix>02.esp, ...; the user may choose another prefix.
    static constexpr const char* kDefaultOutputPrefix = "Dummy_Plugin_";
    std::string outputPrefix = kDefaultOutputPrefix;
    AllocationStrategy strategy = AllocationStrategy::Balanced;
    std::vector<AllocationRule> rules;
    bool mergeCompatible = true;
    bool lightOutputsWhenPossible = false;
    // New records an output must leave free before it is first made light (the range holds 2,048). An
    // output keeps its light or full state on rebuilds (switching changes its records' FormIDs), so an
    // output made light while nearly full would have to go full, and break saves, as soon as a mod update
    // adds a few records. Outputs that are light already are not affected.
    static constexpr uint32_t kDefaultLightHeadroom = 200;
    uint32_t lightHeadroom = kDefaultLightHeadroom;
    // Write a cell's previs file (.uvd) translated into the output's numbering when the output holds the
    // cell's winning record (P2b). Off: the same plugins compile and the same files are checked (the
    // guard still keeps out what cannot be translated), but no translated copy is written, so the game
    // reads the original file. For comparing the two in game.
    bool translatePrevis = true;
    // Cell configuration merge (off by default; PJM's "Fix only Cell Config Conflicts"): a written cell
    // keeps the winner's precombine and previs data and takes the last real change of each
    // configuration field (lighting, location, encounter zone, music, water, regions...) from the
    // versions loading today, instead of the winner's version of all of them.
    bool mergeCellConfig = false;
    // Compatibility patch plan (off by default): records the plan marks Ready are written as their winning
    // version plus the changes the plan carries (to the patch when only plugins that stay hold them).
    bool applyPatchPlan = false;
    std::unordered_map<uint64_t, std::vector<std::string>> plannedChanges; // FormKey::packed() -> change ids
    // A reference that cannot be written faithfully (its master is missing, or it names a record a
    // compiled plugin does not contain): by default the plugin that stores it is kept out of compilation,
    // so it stays in the load order and the game reads its own data unchanged (fail closed). With this on
    // (opt-in, lossy), such references are written as null (00000000) instead and the plugin compiles; the
    // game's own reading of them is not verified, so this can lose data the original plugin had.
    bool brokenReferencesAsNull = false;
    // No longer used: values in undescribed data are never rewritten because they look like FormIDs
    // (kept so older settings load). Undescribed data must be proved safe to move, or is not compiled.
    bool rewriteHeuristicReferences = false;
    // Conflict resolution patch: merges override chains made only of retained plugins
    // into one override-only plugin (like a Bashed Patch). patchOnly compiles no plugins.
    bool buildPatch = false;
    bool patchOnly = false;
    std::string patchName = "Multiplexer_Patch.esp";
    size_t patchMasterLimit = 254; // a patch is split into several plugins above this many masters
    std::unordered_set<uint64_t> skipPatchRecords; // FormKey::packed(): left to last-plugin-wins
    // Keep compiled plugins' archives loading at their original priority through empty ESL plugins
    // of the same name (archive loaders) instead of the INI list. Only possible when the output
    // folder is a mod-manager mod that overrides the original files (Mod Organizer 2).
    bool archiveLoaders = false;
    // Placeholders for unused masters (opt-in; needs archive loaders): see AnalysisOptions::masterPlaceholders.
    bool unusedMasterPlaceholders = false;
    // Master Redirection (advanced, off): a plugin that stays and lists masters that compile gets a copy in
    // the output, loading after the outputs, with those masters replaced by the outputs and every FormID
    // translated, so its masters compile. Mod Organizer 2 with the output inside the mods folder only (the
    // copy must win over the original). `noRedirect`: plugins kept as they are.
    bool redirectMasters = false;
    std::vector<std::string> noRedirect;
    // Advanced: compile plugins that add new quests, dialogue, scenes or NPCs (their voice files and FaceGen
    // move with them). Off: such plugins stay in the load order; overrides of those types still compile.
    bool compileNewQuestsAndNpcs = true;
    // Plugins an earlier build compiled (now switched off) that the user allows back into the load order
    // when this build cannot compile them. Any other such plugin stops the compilation (Pipeline).
    std::vector<std::string> allowReturn;
    // Compile even though records the previous build placed would get other FormIDs while that build is in
    // use (one build only: the GUI and CLI never save it). Off: such a build stops (Pipeline).
    bool acceptFormIdChanges = false;
    // Advanced Users Auto-Merger (opt-in): compile plugins whose names appear in scripts or configs
    // when every such file can be relinked, and write relinked copies into the output (AutoMerger).
    bool autoMerge = false;

    std::string author = "Fallout 4 Multiplexer Renewed";
    std::filesystem::path previousFormMap; // reuse FormIDs from an earlier compilation
    std::unordered_map<uint64_t, PluginIndex> forcedWinners; // FormKey::packed() -> plugin
    unsigned threads = 0;

    // Output plugins are the most the compiler may use: small load orders fill fewer (each output
    // takes at least 2 MB of records before the next one starts). Every output that is not
    // ESL-flagged takes one of the game's 254 full plugin slots, shared with the plugins that stay in
    // the load order (validation checks the total), and outputs master each other, so a worldspace
    // output has 254 minus this many masters left for the plugins it needs.
    static constexpr int kMinOutputs = 1;
    static constexpr int kMaxOutputs = 64;
};

struct EmittedRecord {
    FormKey key;
    Signature sig;
    std::string editorId;
    bool isNew = false; // defined by a compiled plugin: receives a new FormID
    bool patch = false; // conflict resolution between retained plugins (goes to the patch)
    bool merged = false;
    PluginIndex winner = kInvalidPlugin;
    std::vector<PluginIndex> chain;
    std::vector<PluginIndex> contributors;
    FieldSet fields;
    std::vector<DecodeIssue> issues;
    std::vector<uint32_t> deps; // indices of new records this one references
    uint64_t size = 0;
    int output = -1;
    uint32_t objectId = 0;
    uint32_t rawFormId = 0;
    bool formIdChanged = false; // differs from the previous compilation's map
    // Cells, worldspaces and placed references: where the chosen version sits, and its parent cell
    // or worldspace. `container`: copied (the game's current version) only to hold compiled records.
    Placement::Kind placement = Placement::Kind::Top;
    bool persistent = false;
    FormKey parent;
    bool container = false;
    bool world = false; // a cell, worldspace or cell child: all go to one output
    // Cell configuration merge: a cell or worldspace no output would write otherwise (its winning
    // version stays), written only when the merge changes it (configChanged).
    bool configOnly = false;
    bool configChanged = false;
};

struct OutputPlugin {
    int index = 0;
    std::string name;
    bool light = false;
    bool patch = false; // the conflict resolution patch rather than a compiled-state output
    std::vector<PluginIndex> retainedMasters;
    std::vector<int> outputMasters;
    std::vector<std::string> masterNames;
    std::vector<uint32_t> records; // indices into CompiledOutput::records
    uint32_t newRecords = 0;
    uint32_t overrides = 0;
    uint64_t bytes = 0;
    PluginHeader header;
    std::vector<Record> built;
    std::vector<Placement> placements; // parallel to `built`
    // Start-game-enabled quests the game must find in seq\<plugin>.seq: new ones, and overrides that
    // make a quest start with the game when its defining plugin does not (FormIDs as written here).
    std::vector<uint32_t> seqFormIds;
    // A build reopened from its export (Multiplexer_Build.json): the records are not in memory, but the
    // plugin on disk holds them (newRecords and overrides say how many).
    bool restoredWritten = false;
    // Whether this output holds records (is written and switched on).
    bool hasRecords() const { return !records.empty() || restoredWritten; }
};

struct FormMapping {
    FormKey source;
    std::string sourcePlugin;
    uint32_t sourceObjectId = 0;
    int output = 0;
    std::string outputPlugin;
    uint32_t objectId = 0;
    Signature sig;
    std::string editorId;
};

struct CompileIssue {
    Severity severity = Severity::Warning;
    std::string category;
    std::string message;
    FormKey form;
    std::string plugin;
    std::string cause; // likely cause, in plain language
};

// A previs file (.uvd) written loose into the output with its FormIDs translated into the numbering of
// the output that now holds its cell's winning record (P2b). The game reads a previs file's FormIDs in
// the numbering of the plugin whose cell record wins; writing the cell to an output changes that.
struct PrevisRewrite {
    std::string path;           // data-relative, lowercase (vis\<plugin>\<cell>.uvd)
    std::vector<uint8_t> bytes; // the source file with its ID table translated
    FormKey cell;
    std::string source;         // mod or archive providing the original
    std::string numbering;      // plugin whose numbering the original is read in today
    std::string output;         // output whose numbering the copy uses
    size_t formIds = 0;         // FormIDs in the file
    size_t changed = 0;         // FormIDs whose value changed
};

// Master Redirection: a plugin that stays, copied into the output with its compiled masters replaced by the
// outputs and every FormID translated (Redirection.cpp). Loads after the outputs.
struct RedirectedPlugin {
    PluginIndex plugin = kInvalidPlugin;
    std::string name;
    std::string after;                        // the plugin it loaded after (list order); empty: the first
    std::vector<std::string> replacedMasters; // compiled masters the copy no longer lists
    std::vector<std::string> masterNames;
    PluginHeader header;
    std::vector<Record> built;
    std::vector<Placement> placements; // parallel to `built`
    size_t ownVersions = 0;   // records written as the plugin has them (FormIDs translated)
    size_t winnerCopies = 0;  // cells and other containers written as the game shows them today
    size_t dropped = 0;       // overrides left to the outputs or later plugins, as today
    size_t translated = 0;    // FormIDs now naming a record in an output
    std::string failure;      // why it cannot be redirected (then nothing is written; its masters stay)
};

struct CompiledOutput {
    CompileOptions options;
    std::vector<PluginIndex> compiledPlugins;
    std::vector<PluginIndex> retainedPlugins;
    std::vector<EmittedRecord> records;
    std::vector<OutputPlugin> outputs;
    std::vector<FormMapping> mappings;
    std::vector<std::string> archivesToRegister; // archives that must be listed in the INI
    std::vector<PluginIndex> archiveLoaders;      // compiled plugins replaced by an archive loader
    std::vector<PluginIndex> masterPlaceholders;  // of those, the ones kept because plugins that stay list them
    // The F4SE archive loader supplies the archive list in full (no 1,023-character limit): export
    // ships it with the list, and Apply writes the whole list into the INI.
    bool f4seArchiveLoader = false;
    std::vector<CompileIssue> issues;
    std::vector<PrevisRewrite> previsFiles; // translated previs files, written loose on export
    std::vector<RedirectedPlugin> redirected; // Master Redirection copies (failure set: not written)
    // References that cannot be written faithfully, left unwritten (brokenReferencesAsNull off): the
    // plugin that stores each is kept out and the compilation runs again (Pipeline).
    struct BrokenReference {
        FormKey record;                      // the record holding it
        PluginIndex plugin = kInvalidPlugin; // the plugin that stores it (from the raw value), else the winner
        std::string message;
    };
    std::vector<BrokenReference> brokenReferences;
    std::unordered_map<FormKey, uint32_t> indexByKey;
    size_t sourceRecords = 0;      // instances in compiled plugins
    size_t retainedWins = 0;       // overrides left to retained plugins
    size_t identicalToRetained = 0;
    size_t mergedRecords = 0;
    size_t droppedDeleted = 0;
    size_t patchedRecords = 0; // records in the conflict resolution patch
    size_t mergesReused = 0;   // override chains restored from the build cache
    size_t mergesComputed = 0; // override chains merged in this compilation
    size_t containers = 0;     // cells and worldspaces copied to hold compiled references
    size_t configRecords = 0;  // cells and worldspaces written only for the cell configuration merge
    // Reopened from an export after a restart (no records in memory): enough to switch the load order
    // over (Apply to Game), never to export again.
    bool restored = false;
    bool empty() const { return records.empty() && !restored; }
    // Marks compiledPlugins as compiled for isCompiled() (a restored build; pluginCount: Session::plugins).
    void markCompiled(size_t pluginCount);
    bool isCompiled(PluginIndex p) const;
    bool hasArchiveLoader(PluginIndex p) const;
    std::string outputName(int index) const { return outputs.at(static_cast<size_t>(index)).name; }
    const OutputPlugin* patchOutput() const; // the first patch plugin, if any

    // FormID of `key` as written in output `output`; 0 when it cannot be expressed.
    uint32_t translate(const Session& session, FormKey key, int output, bool* ok = nullptr) const;

private:
    friend class CompilationEngine;
    std::vector<uint8_t> compiledFlag_; // by PluginIndex
};

// Stages Resolve / Merge / Rewrite / Compile: determines the final state of
// every record touched by a compiled plugin, merges compatible edits,
// allocates records to output plugins, assigns FormIDs and rewrites references.
class CompilationEngine {
public:
    static CompiledOutput compile(Session& session, const CompileOptions& options,
                                  ProgressSink& progress = nullProgress());

    static std::string outputFileName(const CompileOptions& options, int index);
    // Multiplexer_Patch.esp, Multiplexer_Patch_2.esp, ...
    static std::string patchFileName(const CompileOptions& options, int index);
    static std::vector<FormMapping> readFormMap(const std::filesystem::path& path);
};

} // namespace fo4mx
