#pragma once

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <string>
#include <vector>

namespace fo4mx {

// One asset a compiled record uses, and where the game will load it from.
struct TracedAsset {
    uint32_t record = 0;       // index into CompiledOutput::records
    std::string path;          // normalised game path
    std::string role;          // Mesh, Icon, Material, Texture
    std::string via;           // parent asset for materials/textures found inside meshes/materials
    bool found = false;
    bool loose = false;
    std::string mod;           // mod providing the winning copy
    std::string archive;       // BA2 name when packed
    std::string archiveOwner;  // plugin whose archive it is (empty for INI-listed archives)
    std::string physical;      // file on disk (archive path when packed)
    bool needsRegistration = false; // archive belongs to a compiled plugin: loads only via Multiplexer_Archives.ini
};

struct AssetTrace {
    std::vector<TracedAsset> assets;
    size_t records = 0;
    size_t missing = 0;
    size_t viaRegisteredArchives = 0;
};

// Asset traceability: compiled records keep referencing the original assets;
// this resolves each reference (meshes, then their materials and textures)
// to the mod, archive and path that will serve it at runtime. Nothing is
// copied, extracted or repacked.
class AssetTracer {
public:
    static AssetTrace trace(Session& session, const CompiledOutput& compiled, bool followMeshes = true,
                            ProgressSink& progress = nullProgress());
};

} // namespace fo4mx
