#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fo4mx {

struct ValidationIssue {
    Severity severity = Severity::Warning;
    std::string category; // Missing References, Broken FormIDs, Duplicate IDs, ...
    std::string message;
    std::string plugin;
    std::string formId;
    FormKey record; // the generated record the finding is about, when there is one
    std::string cause; // likely cause, in plain language (filled from the category when empty)
};

struct ValidationReport {
    uint64_t checks = 0;
    uint64_t passed = 0;
    uint64_t warnings = 0;
    uint64_t errors = 0;
    std::vector<ValidationIssue> issues;
    FormKey context; // record being checked; copied into findings made meanwhile

    bool critical() const { return errors > 0; }
    void pass(uint64_t n = 1)
    {
        checks += n;
        passed += n;
    }
    void fail(Severity s, std::string category, std::string message, std::string plugin = {}, std::string formId = {});
    void merge(const ValidationReport& other);
    void fillCauses(); // general explanations for findings without a specific likely cause
};

// How one subrecord of one record type decodes across the whole load order.
struct DefinitionStat {
    Signature record;
    Signature sub;
    bool described = false;
    uint64_t occurrences = 0;
    uint64_t sizeIssues = 0;
    uint64_t formIds = 0;   // FormIDs read at described offsets (non-null)
    uint64_t resolved = 0;  // ... that name an existing record
    uint64_t softRefs = 0;  // values in undescribed bytes that look like FormIDs
    std::string example;    // a record showing the problem
    std::map<uint32_t, uint64_t> softOffsets;       // byte offset -> values there that look like FormIDs
    std::map<uint32_t, uint64_t> unresolvedOffsets; // byte offset -> described FormIDs there that name nothing
};

struct DefinitionCheck {
    std::vector<DefinitionStat> stats;
    std::map<Signature, uint64_t> recordsByType;
    std::vector<Signature> readyCandidates; // Candidate types the load order confirms (no problems found)
    ValidationReport report; // findings, never errors: the check does not block export
};

// A file whose winning archive changes once compiled plugins' archives load from the INI list.
struct ArchivePriorityChange {
    std::string path;
    std::string before, beforeMod; // archive that wins today
    std::string after, afterMod;   // archive that wins after switching to the compiled output
};

// Stage Validate: proves the compiled output is internally consistent before
// anything is written, and re-reads the written files afterwards.
class ValidationEngine {
public:
    static ValidationReport validate(const Session& session, const CompiledOutput& compiled,
                                     ProgressSink& progress = nullProgress());
    static ValidationReport validateWrittenFiles(const CompiledOutput& compiled,
                                                 const std::vector<std::filesystem::path>& files);

    // Archives listed in SResourceArchiveList2 load before every plugin's own archives. Once compiled
    // plugins are disabled, their archives move into that list, so an archive of a plugin that stays
    // can start winning files it used to lose. Loose files are unaffected (they beat all archives).
    static std::vector<ArchivePriorityChange> checkArchivePriority(const Session& session,
                                                                   const CompiledOutput& compiled);

    // Decodes every record of the compilable types (or `types`) in the load order, base game
    // included, and reports layouts that look wrong: FormIDs at described offsets that do not
    // resolve, unexpected sizes, and undescribed data that looks like FormIDs.
    static DefinitionCheck checkDefinitions(Session& session, const std::vector<Signature>& types = {},
                                            ProgressSink& progress = nullProgress(), unsigned threads = 0);
};

} // namespace fo4mx
