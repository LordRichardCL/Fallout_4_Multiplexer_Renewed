#pragma once

#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <string>
#include <utility>
#include <vector>

namespace fo4mx {

// What an inactive plugin would bring if it were enabled, read apart from the load order. It never enters
// the session's records, conflicts or build: it answers "should I enable this mod?", nothing else.
struct InactivePreviewRow {
    PluginIndex plugin = kInvalidPlugin;
    std::string name;
    size_t records = 0;
    size_t newRecords = 0;
    size_t overrides = 0;
    // Records it overrides that an active plugin other than the record's own plugin also changes: if it were
    // enabled, it would conflict with these plugins (the one that wins each record today, with a count).
    size_t conflicts = 0;
    std::vector<std::pair<std::string, size_t>> conflictsWith; // most first
    std::vector<std::string> missingMasters; // masters that are absent or not active
    // An estimate from its masters and record types only: the full check (scripts, assets, references)
    // runs once the plugin is enabled and scanned.
    bool likelyCompiles = false;
    std::string reason;
    std::string error;
};

class InactivePreview {
public:
    // Reads every inactive plugin (not one this app wrote) and compares it with the scanned load order.
    // Needs PluginScanner::scan to have run.
    static std::vector<InactivePreviewRow> run(const Session& session, ProgressSink& progress = nullProgress(),
                                               unsigned threads = 0);
};

} // namespace fo4mx
