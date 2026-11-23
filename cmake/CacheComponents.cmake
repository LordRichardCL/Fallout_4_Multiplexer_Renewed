# What each build-cache component's results depend on, for its code fingerprint (cmake/CacheFingerprint.cmake).
#
# A cached result is reused only while the code that produced it is unchanged: the fingerprint of the files
# listed here is part of the component's version, so editing any of them invalidates that component (and only
# it). Paths are relative to src/core. A file in a list that includes another project file which is neither
# listed nor a boundary stops the build: add the file (when its code can change the result) or a boundary
# (when it cannot, with the reason).

# Every component: reading plugins and records, the record definitions, the session, the cache store itself.
set(FO4MX_CACHE_COMMON
    "esp/*.cpp" "esp/*.hpp" "esp/*.inc" "util/*.cpp" "util/*.hpp"
    engine/Session.cpp engine/Session.hpp
    engine/BuildCache.cpp engine/BuildCache.hpp
    engine/CacheVersions.cpp engine/CacheVersions.hpp
    engine/ParallelFor.hpp
    db/Database.cpp db/Database.hpp
    ../../resources/record_catalogue.csv)

# Headers Session.hpp includes for state the cached work never reads: the asset view, the virtual Data folder
# and the records of earlier builds. Which file is read for a plugin is in every key (path, size, date and a
# sampled hash; for chains, each plugin's name, masters and record hashes).
set(FO4MX_CACHE_COMMON_BOUNDARIES
    assets/AssetIndex.hpp engine/EarlierBuilds.hpp vfs/GameInstall.hpp vfs/VirtualFileSystem.hpp)

# Plugin record index (PluginScanner::indexRecords, BuildCache plugin index format).
set(FO4MX_CACHE_PLUGINS engine/PluginScanner.cpp engine/PluginScanner.hpp)
set(FO4MX_CACHE_PLUGINS_BOUNDARIES
    # BuildCache.hpp names the other components' types to serialise them; the index uses none of them.
    engine/ConflictEngine.hpp engine/FieldModel.hpp
    # PluginScanner.cpp: which file a redirected plugin is read from (in the key), not how it is read.
    engine/Redirection.hpp)

# References of each winning record (DependencyEngine: normalise, split fields, collect references).
set(FO4MX_CACHE_EDGES
    engine/DependencyEngine.cpp engine/DependencyEngine.hpp
    engine/Normalizer.cpp engine/Normalizer.hpp
    engine/FieldModel.cpp engine/FieldModel.hpp)
set(FO4MX_CACHE_EDGES_BOUNDARIES
    engine/ConflictEngine.hpp # BuildCache.hpp, for serialising conflicts
    # DependencyEngine.cpp: eligibility and reference classification, after the cached step.
    engine/Diagnosis.hpp engine/ReferenceScanner.hpp)

# Conflict analysis of each override chain (ConflictEngine::analyseGroup and what it calls).
set(FO4MX_CACHE_CONFLICTS
    engine/ConflictEngine.cpp engine/ConflictEngine.hpp
    engine/Normalizer.cpp engine/Normalizer.hpp
    engine/FieldModel.cpp engine/FieldModel.hpp
    engine/MergeEngine.cpp engine/MergeEngine.hpp
    engine/Compatibility.cpp engine/Compatibility.hpp
    engine/Provenance.cpp engine/Provenance.hpp)
set(FO4MX_CACHE_CONFLICTS_BOUNDARIES)

# Merge of each override chain (CompilationEngine's merge step: normalise, split fields, MergeEngine::merge).
# The whole of CompilationEngine.cpp counts: any change to it invalidates the merges.
set(FO4MX_CACHE_MERGES
    engine/CompilationEngine.cpp engine/CompilationEngine.hpp
    engine/Normalizer.cpp engine/Normalizer.hpp
    engine/FieldModel.cpp engine/FieldModel.hpp
    engine/MergeEngine.cpp engine/MergeEngine.hpp)
set(FO4MX_CACHE_MERGES_BOUNDARIES
    engine/ConflictEngine.hpp # BuildCache.hpp, for serialising conflicts; and the conflict types the compiler reads
    # CompilationEngine.cpp: allocation, redirection, previs and asset checks, after the cached merge.
    engine/Redirection.hpp engine/PrevisCheck.hpp engine/Diagnosis.hpp assets/AssetScanner.hpp)

# Rows the scan writes to the results database (records, assets, references...): written by many parts of the
# core, so all of it counts. Its fingerprint replaces the app version in the scan key.
set(FO4MX_CACHE_SCAN_TABLES "*/*.cpp" "*/*.hpp" "*/*.inc" ../../resources/record_catalogue.csv)
set(FO4MX_CACHE_SCAN_TABLES_NO_GUARD ON)

set(FO4MX_CACHE_COMPONENTS PLUGINS EDGES CONFLICTS MERGES SCAN_TABLES)
