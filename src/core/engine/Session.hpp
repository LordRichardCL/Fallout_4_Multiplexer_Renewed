#pragma once

#include "assets/AssetIndex.hpp"
#include "engine/EarlierBuilds.hpp"
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
#include <set>
#include <span>
#include <string>
#include <string_view>
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
// A plugin that stays in the load order, copied into the output with its compiled masters replaced by the
// outputs (Master Redirection). Its description starts with this tag and names the plugin it loaded after
// ("...; loads after: <name>|"), so a scan reads the original underneath and puts it back in its place.
inline constexpr const char* kRedirectTag = "Redirected copy";

// The suffix that hides a plugin from the game: "Example.esp.ghost" is the plugin Example.esp, not loaded.
inline constexpr std::string_view kGhostSuffix = ".ghost";

// An output of an earlier build found in the load order (enabled or not).
struct EarlierBuild {
    std::string output;
    bool active = false;                  // enabled in plugins.txt
    std::string source;                   // where the list of plugins it compiled was read; empty: none found
    std::vector<std::string> switchedOff; // plugins it compiled, installed and switched off: read as sources
    bool known() const { return !source.empty(); }
};

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
    bool behindRedirect = false;  // the file is the original underneath this app's redirected copy
    bool reactivated = false;     // compiled in the last build and disabled since: still a source
    // The file is "<name>.ghost" (renamed so the game does not load it): read from there, never
    // active in the game, still a source when the last build compiled it. `name` has no ".ghost".
    bool ghosted = false;
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
    // A plugin that stays for its own reasons (not only as a master): the plugins it alone keeps in the
    // load order as masters, directly or through them, which would compile if it did; `sharedHold` counts
    // those it keeps together with other plugins that stay. `heldBy`: another plugin that stays and keeps
    // this one as a master, so compiling it would need that one first (holdsBack is then empty).
    std::vector<PluginIndex> holdsBack;
    size_t sharedHold = 0;
    PluginIndex heldBy = kInvalidPlugin;
    // For a plugin that holds others back: whether Master Redirection would free them, or why not.
    std::string redirectNote;
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
    // Where it sits in the plugin's groups (Placement::Kind) and its parent cell, worldspace or
    // topic as the plugin stores the FormID (Session::resolve gives its FormKey).
    uint8_t placement = 0;
    bool persistent = false;
    uint32_t parentRaw = 0;
    // Version Control Info from the record header, as the plugin wrote it. The game compares a cell's
    // and its references' timestamps with the cell's precombine and previs data, so an output keeps
    // the values of the version it writes.
    uint32_t versionControl1 = 0;
    uint16_t versionControl2 = 0;
};

// All instances of one FormKey: records[begin, end), in load order.
struct FormGroup {
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t size() const { return end - begin; }
};

// What kind of thing names a plugin by its file name.
enum class ReferenceKind : uint8_t {
    PluginName, // the plugin name itself, where its use is known only as a name (e.g. a presence check)
    FilePath,   // a file whose name contains the plugin name
    Directory,  // a folder named after the plugin
    Papyrus,    // a script (compiled .pex or source .psc)
    Config,     // a text configuration file (INI, JSON, TOML, XML, TXT)
    Binary,     // compiled code or another binary file (F4SE DLL)
    Unknown,
};
const char* referenceKindName(ReferenceKind k);

// What compiling the plugin would mean for the reference. The most restrictive class of a plugin's
// references decides whether it can be compiled; their number never does.
enum class ReferenceClass : uint8_t {
    Safe,                  // unaffected: comments, Papyrus sources, names not used by code
    Rewritable,            // can be changed exactly and checked (Advanced Users Auto-Merger)
    CompatibilityRequired, // a known engine convention that would need the plugin's files moved: not implemented
    Unsafe,                // cannot be preserved: binary code, run-time FormIDs, saved settlement data
    Unknown,               // the name is used in a way whose meaning cannot be established
};
const char* referenceClassName(ReferenceClass c);

// One place a plugin's file name appears outside the plugin (ReferenceScanner).
struct PluginReference {
    PluginIndex plugin = kInvalidPlugin;
    std::string assetPath; // data-relative, lowercase (a folder ends with a backslash)
    std::string source;    // mod providing the referencing file
    ReferenceKind kind = ReferenceKind::Unknown;
    std::string format;    // PEX, PSC, INI, JSON, TOML, XML, TXT, DLL, Folder, File name
    std::string text;      // the reference as found
    std::string context;   // how it is used: "Game.GetFormFromFile", "comment", "Plugin|FormID pair", ...
    std::string location;  // "line 12", "Object.State.Function", file count for folders
    ReferenceClass cls = ReferenceClass::Unknown;
    std::string reason;
    // Exact place to change (Rewritable only). Text: byte spans of the name and the FormID.
    // Scripts: offset of the string index (and of the FormID for GetFormFromFile).
    size_t nameBegin = 0, nameEnd = 0;
    size_t idBegin = 0, idEnd = 0;
    size_t decBegin = 0, decEnd = 0; // the same FormID written again in decimal (Transfer Settlements)
    size_t baseBegin = 0, baseEnd = 0; // a settlement workbench's base object, in decimal (baseid_dec)
    bool hasFormId = false;
    uint32_t formId = 0;
    size_t pexStringOffset = 0, pexFormIdOffset = 0;
    // A name passed to another script's function (resolved once every script is read): the callee
    // ("type.function"), the argument, and each argument's constant integer and offset (0: not constant).
    std::string callee;
    int callArgument = -1;
    std::vector<std::pair<int32_t, size_t>> callLiterals;
};
using PluginNameReference = PluginReference; // earlier name

// Evidence on plugin name and FormID pairs in script properties (structs with sPluginName and iFormID,
// as Sim Settlements 2 writes them): how every script that reads such a member uses it. Report only: the
// pairs stay "Compatibility Required" whatever the evidence says.
struct PairEvidence {
    struct Read {
        std::string script;   // data-relative path
        std::string where;    // "Object.State.Function"
        std::string member;   // "spluginname", "iformid", "concat:spluginname"
        std::string object;   // "<struct type>|<variable>"
        std::string use;      // what the script does with it, in words
        bool ok = false;      // a use a rewrite of the pair keeps working
    };
    struct Holder {
        size_t pairs = 0;                // pairs naming a plugin of the load order
        std::set<PluginIndex> named;     // the plugins they name
        std::set<PluginIndex> inPlugins; // the plugins whose records hold them
    };
    bool scanned = false;
    std::vector<Read> reads;                     // every read of the two members, and text joined from them
    std::vector<std::string> unreadable;         // scripts naming a member that could not be read back
    std::map<std::string, Holder> holders;       // "script.property" (lower case) -> its pairs
    std::map<PluginIndex, size_t> pairsNaming;   // plugin -> pairs naming it
    size_t pairs = 0;
    size_t notOk() const;
    bool clean() const { return scanned && pairs && unreadable.empty() && notOk() == 0; }
};

// The combined verdict on a plugin's references.
struct ReferenceDecision {
    ReferenceClass cls = ReferenceClass::Safe; // most restrictive class present
    size_t counts[5] = {};                     // per ReferenceClass
    size_t total = 0;
    std::string summary; // "7 filename references: 2 rewritable, 5 unknown"
    std::string reason;  // why the class decides as it does (eligibility)
    std::string action;  // what the user can do
};

// An override whose record type differs from the record it overrides (a plugin made for another
// version of its master, e.g. a recipe "overriding" what is now a placed light). The game keeps the
// first definition and ignores the override (checked in game: prid shows the original reference,
// last changed by its own plugin), so the scan drops it from the override chain.
struct TypeMismatch {
    FormKey key;
    PluginIndex plugin = kInvalidPlugin;  // the plugin with the mismatched override
    Signature signature;                  // its record type
    PluginIndex definer = kInvalidPlugin; // the plugin of the first definition
    Signature original;                   // the first definition's record type
    uint32_t editorId = 0;                // the override's Editor ID (Session::strings id)
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
    std::vector<TypeMismatch> typeMismatches; // overrides of another record type, left out of `records`
    std::vector<FormGroup> groups;
    StringPool strings;

    std::vector<PluginReference> nameReferences; // every place a plugin's file name appears (ReferenceScanner)
    PairEvidence pairEvidence;                   // plugin name and FormID pairs, how scripts use them (ReferenceScanner)
    // Advanced Users Auto-Merger on: plugins whose references are only safe or rewritable are compiled.
    bool autoMergeEnabled = false;
    std::map<PluginIndex, ReferenceDecision> referenceDecisions;
    // Which of a plugin's masters its records refer to (per entry of its header's master list, 1 = used,
    // or cannot be shown unused); filled on demand by DependencyEngine.
    std::map<PluginIndex, std::vector<uint8_t>> masterUse;
    // (plugin that stays, master it lists but uses none of the records of), from the last eligibility pass.
    std::set<std::pair<PluginIndex, PluginIndex>> unusedMasters;
    // Masters that compile while plugins that stay still list them: an empty placeholder plugin with their
    // name (an archive loader, with or without archives) keeps those plugins loading.
    std::set<PluginIndex> masterPlaceholders;
    // Master Redirection: plugins that stay whose copy in the output lists the outputs instead of their
    // compiled masters (from the last eligibility pass); why each plugin that was checked cannot be
    // redirected (its own content; empty: it can); and plugins a compilation could not redirect (lowercase
    // name -> why), rebuilt by every compilation.
    std::set<PluginIndex> redirected;
    std::map<PluginIndex, std::string> redirectCheck;
    std::map<std::string, std::string> redirectFailed;
    // The plugin list in the order plugins.txt means it: as read, with redirected copies' originals moved
    // back to where they loaded before (empty: the order of `plugins`).
    std::vector<PluginIndex> listOrder;
    std::vector<PluginIndex> pluginsInListOrder() const;
    // Copies the Auto-Merger wrote into the output folder (path -> content hash, from its manifest).
    // They are never read as originals.
    std::unordered_map<std::string, uint64_t> relinkedCopies;
    // Loose copies an earlier export wrote to keep archive winners (path -> content hash): not sources.
    std::unordered_map<std::string, uint64_t> archiveWinnerCopies;
    // Where the previous build's records were read from: the output folder, or the installed package.
    std::filesystem::path buildRecordsDirectory;
    // Archives an earlier build added to the INI list (lowercase). The scan loads them as their plugin's
    // archives, where the game loaded them before that build: the load order as it was.
    std::unordered_set<std::string> registeredArchives;
    size_t restoredArchives = 0; // INI archives treated as their plugin's because an earlier build added them // per plugin with references (eligibility)
    std::vector<RecordEdge> recordEdges; // references of winning compilable records
    std::unordered_set<std::string> userRetained; // lowercase plugin names the user excluded
    // Plugins kept out of compilation because validation found records it cannot write safely
    // (lowercase name -> the finding that caused it). Rebuilt by every compilation.
    std::map<std::string, std::string> autoRetained;
    // Cells kept out for their precombine or previs data -> what their previs file and precombined
    // meshes hold (PrevisCheck, read-only evidence). Rebuilt by every compilation.
    std::map<FormKey, std::string> previsChecks;
    // New cells kept out for that data -> whether stage PC2 of docs/PRECOMBINED_CELLS_DESIGN.md could move
    // their files under an output's name and their new FormID, and why not (read only, rebuilt each compile).
    struct NewCellAssessment {
        bool movable = false;
        std::string text;
    };
    std::map<FormKey, NewCellAssessment> newCellChecks;
    // Plugins whose navmesh data does not decode (plugin -> the first such record and why); worked
    // out once per scan, when navmeshes are compiled.
    std::optional<std::map<PluginIndex, std::string>> navmeshFaults;
    // Plugins compiled by the last build (lowercase). After switching over they are disabled, or
    // hidden behind an archive loader; scans still treat them as active sources in their position.
    std::unordered_set<std::string> reactivate;
    // Records of earlier builds (export folders, the database), set before the scan; the list installed
    // with the outputs is read by the scan itself.
    std::vector<BuildRecord> buildRecords;
    // Every output of an earlier build in the load order (this app's plugins), with what it compiled.
    std::vector<EarlierBuild> earlierBuilds;

    BuildCache* cache = nullptr; // optional, owned by the pipeline
    std::vector<std::vector<bool>> masterClosure; // [plugin][master], see computeMasterClosure

    bool scanned = false;
    bool assetsScanned = false;
    bool analysed = false;

    PluginInfo* findPlugin(std::string_view name);
    const PluginInfo* findPlugin(std::string_view name) const;
    PluginIndex addPlugin(PluginInfo info);
    // Closes the plugin files kept open for reading, so they can be renamed (ghosted) on Windows.
    void releaseFiles() { reader_.closeAll(); }
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

    // True when `master` is a direct or indirect master of `plugin`.
    bool dependsOn(PluginIndex plugin, PluginIndex master) const;
    // Precomputes dependsOn for every pair; call after the plugins' masters are resolved.
    void computeMasterClosure();
    // For each instance of a record: the earlier instance it was made against, i.e. the last
    // earlier instance whose plugin is one of its masters (0, the defining record, otherwise).
    // Entry 0 maps to itself. This is the version xEdit compares an override with.
    std::vector<size_t> overrideParents(std::span<const RecordInstance> chain) const;

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
