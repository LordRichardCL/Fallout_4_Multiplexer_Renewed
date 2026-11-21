#pragma once

#include "engine/CompilationEngine.hpp"

#include <string>
#include <vector>

namespace fo4mx {

// What the next build changes compared with the previous one (the build you play with): which records it
// adds or drops, which keep their FormIDs and which get others (moved to another output, or renumbered),
// and which plugins it compiles for the first time or returns to the load order. FormIDs are what saves
// hold, so every changed or dropped one is something saves made with the previous build lose.
struct BuildComparison {
    struct Change {
        FormMapping before;
        FormMapping after;
    };
    std::string previousSource; // where the previous build's records were read
    size_t kept = 0;            // same output plugin and FormID
    std::vector<Change> moved;       // to another output plugin
    std::vector<Change> renumbered;  // same output plugin, another FormID
    std::vector<FormMapping> added;   // new in this build
    std::vector<FormMapping> removed; // the previous build had them, this one does not
    std::vector<std::string> newlyCompiled; // compiled now, not by the previous build
    std::vector<std::string> returned;      // compiled by the previous build, not now (back in the load order)
    std::vector<std::string> outputsAdded, outputsRemoved;

    bool same() const
    {
        return moved.empty() && renumbered.empty() && added.empty() && removed.empty() && newlyCompiled.empty() &&
               returned.empty() && outputsAdded.empty() && outputsRemoved.empty();
    }
    size_t formIdChanges() const { return moved.size() + renumbered.size() + removed.size(); }
};

struct BuildSide {
    std::vector<FormMapping> mappings;
    std::vector<std::string> compiled; // plugins compiled
    std::vector<std::string> outputs;  // output plugins with records
};

BuildComparison compareBuilds(const BuildSide& before, const BuildSide& after);

// The previous build's side from its records folder (Multiplexer_FormMap.tsv, Multiplexer_CompiledPlugins.txt).
// False when the folder holds no FormID map.
bool readBuildSide(const std::filesystem::path& recordsDir, BuildSide& out);

// Plain text, every list cut after `limit` lines.
std::string formatBuildComparison(const BuildComparison& c, size_t limit = 200);

} // namespace fo4mx
