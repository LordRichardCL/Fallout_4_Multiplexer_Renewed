#pragma once

#include "engine/Session.hpp"
#include "util/Progress.hpp"

namespace fo4mx {

struct ScanOptions {
    unsigned threads = 0; // 0 = automatic
};

// Stage 1: discovers plugins in the virtual Data folder, determines the load
// order, resolves masters and indexes every record of every active plugin.
class PluginScanner {
public:
    static void scan(Session& session, const ScanOptions& options = {}, ProgressSink& progress = nullProgress());

    // Builds plugin entries, load order and master links from headers only.
    static void discoverPlugins(Session& session, ProgressSink& progress = nullProgress());
    // Indexes records of all active plugins and groups them by FormKey.
    static void indexRecords(Session& session, const ScanOptions& options, ProgressSink& progress = nullProgress());
};

} // namespace fo4mx
