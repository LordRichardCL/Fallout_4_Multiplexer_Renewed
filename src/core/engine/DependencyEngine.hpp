#pragma once

#include "engine/Session.hpp"
#include "esp/FieldDecoder.hpp"
#include "util/Progress.hpp"

#include <string>
#include <vector>

namespace fo4mx {

struct AnalysisOptions {
    std::vector<std::string> excludedPlugins; // never compile these
    // Only the plugins the user chose are compiled (opt-in); every other plugin that could compile stays in
    // the load order ("Not chosen"). Off: every eligible plugin not excluded is compiled.
    bool onlySelected = false;
    std::vector<std::string> selectedPlugins;
    // The plugin names that identify the selection, for change detection ("only:<name>" entries).
    std::vector<std::string> selectionKey() const;
    bool compileCreationClub = false;
    unsigned threads = 0;
    // Placeholders for unused masters (CompileOptions::unusedMasterPlaceholders, possible only where archive
    // loaders are): a master that plugins staying in the load order list but use none of the records of is
    // compiled, and an empty plugin with its name keeps them loading. `placeholdersOff` says why not, when
    // they are wanted but not possible.
    bool masterPlaceholders = false;
    std::string placeholdersOff;
    // CompileOptions::compileNewQuestsAndNpcs.
    bool newQuestsAndNpcs = true;
    // Master Redirection (CompileOptions::redirectMasters, possible only where archive loaders are): a plugin
    // that stays and lists compiled masters gets a copy in the output naming the outputs instead, so its
    // masters compile. `redirectOff` says why not, when wanted but not possible; `noRedirect`: plugins
    // (lowercase or not) the user keeps as they are.
    bool redirectMasters = false;
    std::string redirectOff;
    std::vector<std::string> noRedirect;
};

// Why `plugin` cannot be copied with its masters redirected, from its own content (every record decodes
// completely into described fields of a compiled type; no precombine or previs data): empty when it can.
std::string redirectionBlock(Session& session, PluginIndex plugin, std::span<const uint32_t> instances);

struct DependencyIssue {
    Severity severity = Severity::Warning;
    std::string category; // "Missing Master", "Missing Reference", ...
    PluginIndex plugin = kInvalidPlugin;
    FormKey form;
    std::string message;
    std::string cause; // likely cause, in plain language
    bool cleaning = false; // a mod-cleaning finding (ITM, deleted record, injected record, ...)
};

// A record type that keeps plugins out of compilation because it is not supported yet.
struct TypeBlocker {
    Signature sig;
    std::string name;
    std::string category;   // catalogue category; "Unknown" for signatures that are not Fallout 4 record types
    std::string support;    // RecordDefinitions::supportSummary
    bool known = true;      // listed in the record catalogue
    size_t plugins = 0;     // plugins containing the type (otherwise eligible or not)
    size_t records = 0;     // records of the type in those plugins
    size_t soleBlocker = 0; // plugins for which it is the only unsupported type
    size_t unlocks = 0;     // plugins that would become eligible if only this type were supported
};

struct DependencyReport {
    std::vector<DependencyIssue> issues;
    size_t pluginEdges = 0;
    size_t recordEdges = 0;
    size_t danglingReferences = 0;
    size_t eligiblePlugins = 0;
    size_t eligibleRecords = 0;
    // Unsupported record types, most plugins unlocked first.
    std::vector<TypeBlocker> blockers;
    size_t blockedByTypes = 0;  // active, otherwise compilable plugins held back by record types
    size_t unlockedByAll = 0;   // plugins that supporting every blocking type would unlock
};

// Stage 3: plugin- and record-level dependency graphs, missing requirements,
// potential breakages, and which plugins can safely be compiled.
class DependencyEngine {
public:
    static DependencyReport analyse(Session& session, const AnalysisOptions& options = {},
                                    ProgressSink& progress = nullProgress());
    static void evaluateEligibility(Session& session, const AnalysisOptions& options, DependencyReport* report = nullptr);

    // Plugins that list `plugin` as a master.
    static std::vector<PluginIndex> dependents(const Session& session, PluginIndex plugin);

    // Which of `plugin`'s masters its records refer to, one entry per master in its header (1 = used). A
    // master counts as used when the plugin overrides one of its records, holds a record in one of its
    // groups, has a FormID naming it where the definitions locate (or may locate) FormIDs, or has a byte
    // equal to its index in a subrecord with data the definitions do not describe. A plugin that does not
    // read uses every master (fail closed).
    static std::vector<uint8_t> mastersInUse(const Session& session, const PluginInfo& plugin);
};

} // namespace fo4mx
