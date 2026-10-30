#pragma once

#include "engine/FieldModel.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"
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
    std::string outputPrefix = "Dummy_Plugin_";
    AllocationStrategy strategy = AllocationStrategy::Balanced;
    std::vector<AllocationRule> rules;
    bool mergeCompatible = true;
    bool lightOutputsWhenPossible = false;
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
    // Advanced Users Auto-Merger (opt-in): compile plugins whose names appear in scripts or configs
    // when every such file can be relinked, and write relinked copies into the output (AutoMerger).
    bool autoMerge = false;

    std::string author = "Fallout 4 Multiplexer Renewed";
    std::filesystem::path previousFormMap; // reuse FormIDs from an earlier compilation
    std::unordered_map<uint64_t, PluginIndex> forcedWinners; // FormKey::packed() -> plugin
    unsigned threads = 0;

    static constexpr int kMinOutputs = 10;
    static constexpr int kMaxOutputs = 20;
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

struct CompiledOutput {
    CompileOptions options;
    std::vector<PluginIndex> compiledPlugins;
    std::vector<PluginIndex> retainedPlugins;
    std::vector<EmittedRecord> records;
    std::vector<OutputPlugin> outputs;
    std::vector<FormMapping> mappings;
    std::vector<std::string> archivesToRegister; // archives that must be listed in the INI
    std::vector<PluginIndex> archiveLoaders;      // compiled plugins replaced by an archive loader
    std::vector<CompileIssue> issues;
    std::unordered_map<FormKey, uint32_t> indexByKey;
    size_t sourceRecords = 0;      // instances in compiled plugins
    size_t retainedWins = 0;       // overrides left to retained plugins
    size_t identicalToRetained = 0;
    size_t mergedRecords = 0;
    size_t droppedDeleted = 0;
    size_t patchedRecords = 0; // records in the conflict resolution patch
    size_t mergesReused = 0;   // override chains restored from the build cache
    size_t mergesComputed = 0; // override chains merged in this compilation
    bool empty() const { return records.empty(); }
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
