#include "engine/CacheVersions.hpp"

#include "CacheFingerprints.hpp" // generated (cmake/CacheFingerprint.cmake)

#include <sqlite3.h>
#include <zlib.h>

namespace fo4mx {

namespace {

// Stored formats. Raise one when its layout or meaning changes in a way the code fingerprint would not show
// (the fingerprint already changes with any edit to the files of the component, BuildCache.cpp included).
constexpr int kPluginIndexFormat = 1;
constexpr int kEdgesFormat = 1;
constexpr int kConflictsFormat = 1;
constexpr int kMergesFormat = 1;

std::string compose(int format, const char* code)
{
    // zlib inflates the stored blobs and the compressed records; SQLite stores the cache.
    return "f" + std::to_string(format) + "-" + code + "-z" + zlibVersion() + "-s" + sqlite3_libversion();
}

} // namespace

CacheVersions CacheVersions::current()
{
    CacheVersions v;
    v.pluginIndex = compose(kPluginIndexFormat, cachecode::k_plugins);
    v.edges = compose(kEdgesFormat, cachecode::k_edges);
    v.conflicts = compose(kConflictsFormat, cachecode::k_conflicts);
    v.merges = compose(kMergesFormat, cachecode::k_merges);
    return v;
}

std::string scanTablesVersion()
{
    return compose(1, cachecode::k_scan_tables);
}

} // namespace fo4mx
