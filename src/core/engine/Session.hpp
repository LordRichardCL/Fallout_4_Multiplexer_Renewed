#pragma once

#include "assets/AssetIndex.hpp"
#include "esp/FormId.hpp"
#include "esp/PluginFile.hpp"
#include "esp/PluginHeader.hpp"
#include "esp/RecordDefinitions.hpp"
#include "esp/StringTables.hpp"
#include "util/StringPool.hpp"
#include "vfs/GameInstall.hpp"
#include "vfs/VirtualFileSystem.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fo4mx {

enum class PluginKind : uint8_t { ESM, ESP, ESL, LightESP, LightESM };
const char* pluginKindName(PluginKind k);

// Whether a plugin can be folded into the compiled output.
enum class Eligibility : uint8_t {
    Eligible,   // will be compiled and can be disabled afterwards
    Retained,   // base game, Creation Club or user-excluded: stays in the load order
    Ineligible, // contains something the compiler cannot reproduce
    Inactive,   // not enabled in the load order
    Error,      // unreadable or missing masters
};
const char* eligibilityName(Eligibility e);

// Author written into every plugin this app generates; scans recognise them by it.
inline constexpr const char* kGeneratorAuthor = "Fallout 4 Multiplexer Renewed";
// Description prefix of archive loaders: empty ESL plugins named after a compiled plugin so its
// archives keep loading at the same priority.
inline constexpr const char* kArchiveLoaderTag = "Archive loader";

struct PluginInfo {
    PluginIndex index = kInvalidPlugin;
    std::string name;
    std::string key; // lowercase name
    std::filesystem::path path;
    std::string sourceMod;
    PluginHeader header;
    PluginKind kind = PluginKind::ESP;

    bool active = false;
    bool implicit = false;
    bool baseGame = false;
    bool creationClub = false;
    bool generated = false;       // a Dummy plugin, patch or loader this app wrote: never a source
    bool behindLoader = false;    // the file is the original underneath this app's archive loader
    bool reactivated = false;     // compiled in the last build and disabled since: still a source
    int32_t loadOrder = -1;     // position among active plugins
    std::string loadOrderHex;   // "0A" or "FE 001"

    std::vector<PluginIndex> masters; // resolved, kInvalidPlugin when missing
    std::vector<std::string> missingMasters;

    uint64_t fileSize = 0;
    int64_t modified = 0;   // Unix seconds
    uint64_t fileHash = 0;  // FNV-1a of the whole file (set by the scanner)
    bool fromCache = false; // record index restored from the build cache
    uint32_t recordCount = 0;
    uint32_t newRecords = 0;
    uint32_t overrides = 0;
    uint32_t injected = 0;
    uint32_t itmCount = 0;
    uint32_t deletedCount = 0;
    uint32_t conflictCount = 0;
    uint32_t hardConflictCount = 0;
    std::map<Signature, uint32_t> typeCounts;

    bool scanned = false;
    std::string error;

    Eligibility eligibility = Eligibility::Inactive;
    std::vector<std::string> reasons;
    std::vector<std::string> archives; // "<plugin> - *.ba2" archives that load with it

    bool isMaster() const; // ESM flag or .esm / .esl extension
    bool isLight() const;
    std::string baseName() const; // without extension
};

struct RecordInstance {
    FormKey key;
    PluginIndex plugin = kInvalidPlugin;
    Signature signature;
    uint32_t flags = 0;
    uint32_t dataSize = 0;
    uint32_t rawFormId = 0;
    uint64_t offset = 0;
    uint64_t hash = 0;
    uint32_t editorId = 0; // Session::strings id
    int32_t loadPos = 0;   // load order position of `plugin`
};

// All instances of one FormKey: records[begin, end), in load order.
struct FormGroup {
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t size() const { return end - begin; }
};

// A script or config file that refers to a plugin by file name. Such plugins
// cannot be renamed away, so they must stay in the load order.
struct PluginNameReference {
    PluginIndex plugin = kInvalidPlugin;
    std::string assetPath;
    std::string source; // mod providing the referencing file
};

struct RecordEdge {
    FormKey from;
    FormKey to;
    Signature via; // subrecord holding the reference
};

class BuildCache;

class Session {
public:
    explicit Session(const RecordDefinitions& defs = RecordDefinitions::get());
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const RecordDefinitions& defs;
    ResolvedInstall install;
    VirtualFileSystem vfs;
    AssetIndex assets;

    std::vector<PluginInfo> plugins;
    std::vector<PluginIndex> loadOrder; // active plugins, final order
    std::vector<RecordInstance> records; // sorted by FormKey, then load order
    std::vector<FormGroup> groups;
    StringPool strings;

    std::vector<PluginNameReference> nameReferences;
    std::vector<RecordEdge> recordEdges; // references of winning compilable records
    std::unordered_set<std::string> userRetained; // lowercase plugin names the user excluded
    // Plugins kept out of compilation because validation found records it cannot write safely
    // (lowercase name -> the finding that caused it). Rebuilt by every compilation.
    std::map<std::string, std::string> autoRetained;
    // Plugins compiled by the last build (lowercase). After switching over they are disabled, or
    // hidden behind an archive loader; scans still treat them as active sources in their position.
    std::unordered_set<std::string> reactivate;

    BuildCache* cache = nullptr; // optional, owned by the pipeline

    bool scanned = false;
    bool assetsScanned = false;
    bool analysed = false;

    PluginInfo* findPlugin(std::string_view name);
    const PluginInfo* findPlugin(std::string_view name) const;
    PluginIndex addPlugin(PluginInfo info);
    void rebuildPluginIndex();

    std::span<const RecordInstance> instances(const FormGroup& g) const
    {
        return std::span<const RecordInstance>(records.data() + g.begin, g.size());
    }
    const FormGroup* findGroup(FormKey key) const;
    // True for records in the load order and for the engine's hard-coded forms
    // (Fallout4.esm object IDs below 0x800, e.g. PlayerRef 0x14), which no plugin stores.
    bool exists(FormKey key) const;
    const RecordInstance* winner(FormKey key) const;

    // Global identity of a FormID as written in plugin `p`.
    FormKey resolve(const PluginInfo& p, uint32_t rawFormId) const;

    Record loadRecord(const RecordInstance& inst);
    const StringTables* stringTables(PluginIndex plugin);
    std::vector<uint8_t> readAsset(std::string_view normalizedPath, size_t maxBytes = 0);

    // "EditorID [WEAP:0001F669]" (load order FormID)
    std::string formLabel(FormKey key) const;
    std::string editorIdOf(FormKey key) const;
    std::string loadOrderFormId(FormKey key) const;
    std::string pluginName(PluginIndex p) const;

    size_t activePluginCount() const { return loadOrder.size(); }

private:
    std::unordered_map<std::string, PluginIndex> byKey_;
    RecordReader reader_;
    std::mutex stringsMutex_;
    std::unordered_map<PluginIndex, std::unique_ptr<StringTables>> stringTables_;
};

} // namespace fo4mx
