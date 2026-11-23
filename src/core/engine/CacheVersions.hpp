#pragma once

#include <string>

namespace fo4mx {

// The version of each build-cache component: its stored format, the fingerprint of the code that produces it
// (cmake/CacheComponents.cmake, computed at build time) and the compiler and libraries. A cache entry is reused
// only under the version it was written with; a component whose version changed is emptied on its own, the
// others are kept. Not the app version: an update that leaves a component's code alone keeps its entries.
struct CacheVersions {
    std::string pluginIndex;
    std::string edges;
    std::string conflicts;
    std::string merges;

    static CacheVersions current();
    bool operator==(const CacheVersions&) const = default;
};

// Fingerprint of the code that writes the scan's rows of the results database (all of the core): with the mod
// space, it decides whether those rows are written again.
std::string scanTablesVersion();

} // namespace fo4mx
