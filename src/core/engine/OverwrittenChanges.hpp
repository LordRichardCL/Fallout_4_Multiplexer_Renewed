#pragma once

#include "engine/ConflictEngine.hpp"
#include "engine/Session.hpp"

#include <unordered_set>

#include <map>
#include <string>
#include <vector>

namespace fo4mx {

// "A mod doesn't seem to work": the changes a plugin (and the plugins made for it) makes that the winning
// versions do not hold, grouped by the later plugin that overwrites them. Read from the conflict analysis
// (the change provenance), so it describes the load order as scanned, before any build: a build keeps the
// same winning versions.

struct OverwriteExample {
    std::string form;   // "Gunslinger01 [PERK:0004A09F]"
    std::string field;  // "Record Flags"
    std::string change; // "0x00000000 → 0x00000004"
    std::string madeBy; // the plugin that made the change (the checked one, or a plugin made for it)
};

struct Overwriter {
    PluginIndex plugin = kInvalidPlugin;
    size_t records = 0;      // records in which it overwrites changes
    size_t changes = 0;      // changes it overwrites, made by the checked plugin
    size_t patchChanges = 0; // ... made by plugins made for the checked plugin
    // It was made with the plugin that made the change as a master, and changed or removed it: usually on
    // purpose (a patch). Otherwise it never had the change: it loads later without knowing about it.
    size_t deliberate = 0;
    std::map<std::string, size_t> types;           // record type -> records
    std::map<PluginIndex, size_t> keptBefore;      // plugins that still held the change before it ("kept by")
    std::vector<OverwriteExample> examples;        // the first few
};

struct OverwrittenChanges {
    bool found = false;     // the plugin is active in the scanned load order
    bool analysed = false;  // the conflict analysis ran
    PluginIndex plugin = kInvalidPlugin;
    std::vector<PluginIndex> madeFor; // active plugins with it as a master (its patches and add-ons)
    size_t records = 0;     // records it, or a plugin made for it, changes that another plugin also changes
    size_t changes = 0;     // the changes those plugins make in them
    size_t lost = 0;        // of those, the ones the winning version does not hold
    size_t lostRecords = 0; // records with at least one of them
    // Not in the winning version, but in the merged version a build writes (lists and other fields it merges):
    // lost only without a build. Not counted in `lost`.
    size_t merged = 0;
    size_t mergedRecords = 0;
    // Not in the winning version, but carried into the last build by the Compatibility Patch option. Not
    // counted in `lost` either.
    size_t carried = 0;
    size_t carriedRecords = 0;
    std::vector<Overwriter> overwriters; // most changes first
};

struct PatchPlan;
struct CompiledOutput;

// The change ids (TracedChange::changeId) the last build carried with the Compatibility Patch option: the
// carried changes of the plan's Ready records, less the records the compiler wrote without the plan (it
// warns when the plan no longer matches). Empty when the option is off or nothing was compiled.
std::unordered_set<std::string> changesCarriedByBuild(const PatchPlan* plan, const CompiledOutput* compiled,
                                                      bool applyPatchPlan);

// `includeMadeFor`: count the changes of plugins made for it (with it as a master) too. `carried`: changes
// the build carried (changesCarriedByBuild), counted apart from the lost ones.
OverwrittenChanges findOverwrittenChanges(const Session& session, const ConflictReport* conflicts,
                                          std::string_view pluginName, bool includeMadeFor = true,
                                          const std::unordered_set<std::string>* carried = nullptr);
std::string formatOverwrittenChanges(const Session& session, const OverwrittenChanges& result,
                                     std::string_view pluginName, size_t examplesPerPlugin = 3);

} // namespace fo4mx
