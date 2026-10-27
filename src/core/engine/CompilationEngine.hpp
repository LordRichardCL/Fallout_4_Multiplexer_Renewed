#pragma once

#include "engine/FieldModel.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginHeader.hpp"
#include "util/Progress.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
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
};

struct CompiledOutput {
    CompileOptions options;
    std::vector<PluginIndex> compiledPlugins;
    std::vector<PluginIndex> retainedPlugins;
    std::vector<EmittedRecord> records;
    std::vector<OutputPlugin> outputs;
    std::vector<FormMapping> mappings;
    std::vector<std::string> archivesToRegister;
    std::vector<CompileIssue> issues;
    std::unordered_map<FormKey, uint32_t> indexByKey;
    size_t sourceRecords = 0;      // instances in compiled plugins
    size_t retainedWins = 0;       // overrides left to retained plugins
    size_t identicalToRetained = 0;
    size_t mergedRecords = 0;
    size_t droppedDeleted = 0;
    size_t mergesReused = 0;   // override chains restored from the build cache
    size_t mergesComputed = 0; // override chains merged in this compilation
    bool empty() const { return records.empty(); }
    bool isCompiled(PluginIndex p) const;
    std::string outputName(int index) const { return outputs.at(static_cast<size_t>(index)).name; }

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
    static std::vector<FormMapping> readFormMap(const std::filesystem::path& path);
};

} // namespace fo4mx
