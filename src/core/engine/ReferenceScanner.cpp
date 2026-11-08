#include "engine/ReferenceScanner.hpp"

#include "assets/AssetScanner.hpp"
#include "engine/ParallelFor.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"
#include "papyrus/Pex.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <cctype>
#include <map>
#include <set>
#include <mutex>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* referenceKindName(ReferenceKind k)
{
    switch (k) {
    case ReferenceKind::PluginName: return "Plugin name";
    case ReferenceKind::FilePath: return "File path";
    case ReferenceKind::Directory: return "Directory";
    case ReferenceKind::Papyrus: return "Papyrus";
    case ReferenceKind::Config: return "Config";
    case ReferenceKind::Binary: return "Binary";
    case ReferenceKind::Unknown: return "Unknown";
    }
    return "";
}

const char* referenceClassName(ReferenceClass c)
{
    switch (c) {
    case ReferenceClass::Safe: return "Safe";
    case ReferenceClass::Rewritable: return "Rewritable";
    case ReferenceClass::CompatibilityRequired: return "Compatibility Required";
    case ReferenceClass::Unsafe: return "Unsafe";
    case ReferenceClass::Unknown: return "Unknown";
    }
    return "";
}

namespace {

// ---- Classifier --------------------------------------------------------------------------------
// What a detector found. The table below is the only place that decides what each fact means.
enum class Fact {
    PexGetFormFixed,     // Game.GetFormFromFile(<constant>, "Plugin.esp")
    PexGetFormRuntime,   // Game.GetFormFromFile(<variable>, "Plugin.esp")
    PexPluginInstalled,  // Game.IsPluginInstalled("Plugin.esp")
    PexOtherUse,         // any other use of the name by the script's code
    PexConstantProperty, // the value of a property's read handler: classified by the scripts reading it
    PexUnused,           // in the string table, not used by code
    PexUnreadable,       // the script does not read back exactly
    PscSource,           // Papyrus source
    ConfigComment,       // a comment line
    ConfigPair,          // "Plugin.esp|FormID"
    McmRequirement,      // MCM config.json "pluginRequirements"
    ConfigPlain,         // a config naming the plugin without a FormID
    SettlementBlueprint, // Transfer Settlements blueprint: the settlement itself (workshop, worldspace)
    BlueprintItem,       // Transfer Settlements blueprint item: plugin_name with its FormID (hex and decimal)
    BlueprintRequired,   // Transfer Settlements blueprint: an entry of plugins_required
    BinaryString,        // F4SE DLL
    EngineFolder,        // FaceGen / face textures / LOD billboards folder named after the plugin
    VoiceFolder,         // sound\voice\<plugin>\: relocated with the plugin's responses at export
    CellDataFolder,      // meshes\precombined\<plugin>\, vis\<plugin>\: read only for the plugin's own cells
    OtherFolder,         // any other folder named after the plugin
    RecordPathFolder,    // a folder named after the plugin whose every file its records name by path
    AssetFolder,         // a folder named after the plugin under meshes, textures, materials, sound or music
    EngineFileName,      // seq\<plugin>.seq (start-game-enabled quests)
    ToolFileName,        // RobCo Patcher INI named after the plugin
    FormIdIni,           // bcr\<plugin>.ini: FormIDs relative to the plugin, rewritten under the output's name
    FormIdIniUnknown,    // bcr\<plugin>.ini with lines that are not "[section]" or "key=<FormID>"
    OtherFileName,       // any other file named after the plugin
    RecordScriptString,  // a script property in a plugin's record holds the file name (VMAD string)
    PluginIni,           // Data\<plugin>.ini: read by the game only while that plugin is active
};

struct Rule {
    ReferenceKind kind;
    ReferenceClass cls;
    const char* context;
    const char* reason;
};

Rule rule(Fact f)
{
    using K = ReferenceKind;
    using C = ReferenceClass;
    switch (f) {
    case Fact::PexGetFormFixed:
        return {K::Papyrus, C::Rewritable, "Game.GetFormFromFile",
                "GetFormFromFile with a fixed FormID: the call can name the output plugin and the record's new FormID"};
    case Fact::PexGetFormRuntime:
        return {K::Papyrus, C::Unsafe, "Game.GetFormFromFile",
                "GetFormFromFile with a FormID computed at run time: the record's new FormID cannot be supplied"};
    case Fact::PexPluginInstalled:
        return {K::PluginName, C::Rewritable, "Game.IsPluginInstalled",
                "presence check: can name the output plugin, which is installed"};
    case Fact::PexConstantProperty:
        return {K::Papyrus, C::Unknown, "constant property",
                "the value of a named constant (a property): what it means depends on the scripts that read it"};
    case Fact::PexOtherUse:
        return {K::Papyrus, C::Unknown, "script code",
                "used by the script in a way whose meaning cannot be established (it may reach GetFormFromFile "
                "through a variable or another script)"};
    case Fact::PexUnused:
        return {K::Papyrus, C::Safe, "string table", "in the script's string table but not used by its code"};
    case Fact::PexUnreadable:
        return {K::Papyrus, C::Unknown, "unreadable script",
                "the script could not be read completely, so its use of the name cannot be established"};
    case Fact::PscSource:
        return {K::Papyrus, C::Safe, "Papyrus source", "Papyrus source: the game runs the compiled .pex, not the source"};
    case Fact::ConfigComment:
        return {K::Config, C::Safe, "comment", "comment line: not read by the tool"};
    case Fact::ConfigPair:
        return {K::Config, C::Rewritable, "Plugin|FormID pair",
                "Plugin|FormID pair: can name the output plugin and the record's new FormID"};
    case Fact::McmRequirement:
        return {K::PluginName, C::Rewritable, "MCM pluginRequirements",
                "MCM plugin requirement (presence check): can name the output plugin"};
    case Fact::ConfigPlain:
        return {K::Config, C::Unknown, "plugin name",
                "names the plugin without a FormID: what the tool does with the name is unknown"};
    case Fact::BlueprintItem:
        return {K::Config, C::Rewritable, "Transfer Settlements item",
                "an item of a settlement blueprint: its plugin name and FormID (hex and decimal) are rewritten "
                "together"};
    case Fact::BlueprintRequired:
        return {K::PluginName, C::Rewritable, "Transfer Settlements required plugin",
                "a plugin the blueprint requires: it can name the output plugin, which is installed"};
    case Fact::SettlementBlueprint:
        return {K::Config, C::Unsafe, "Transfer Settlements blueprint", "saved settlement data: not changed"};
    case Fact::BinaryString:
        return {K::Binary, C::Unsafe, "compiled code",
                "an F4SE plugin (DLL) contains the name: what its code does with it cannot be established or changed"};
    case Fact::VoiceFolder:
        return {K::Directory, C::Safe, "relocated folder",
                "copied at export into the output's folder, named after the records' new FormIDs, so the game "
                "finds them as before"};
    case Fact::CellDataFolder:
        return {K::Directory, C::Safe, "cell data folder",
                "read by the game only for new cells of this plugin that carry precombine or previs data (PCMB, "
                "VISI, XCRI, XPRI, or a previs file named by RVIS); the compiler keeps a plugin with such a cell out "
                "by itself (Precombined Cell), so when the plugin compiles no cell uses these files, before or after"};
    case Fact::EngineFolder:
        return {K::Directory, C::CompatibilityRequired, "engine folder",
                "the game finds these files by the defining plugin's name; moving them to the output is not "
                "implemented"};
    case Fact::RecordPathFolder:
        return {K::Directory, C::Safe, "folder of record paths",
                "every file in this folder is named by its full path in the plugin's records (models, sounds): the "
                "game loads them by that path, which compiling keeps unchanged"};
    case Fact::AssetFolder:
        return {K::Directory, C::Safe, "asset folder",
                "the game finds files under meshes, textures, materials, sound and music only by the paths records "
                "and meshes name (apart from the FaceGen, voice, precombine, previs and LOD folders, checked "
                "separately), never by the plugin's name; compiling keeps those paths"};
    case Fact::OtherFolder:
        return {K::Directory, C::Unknown, "folder",
                "a folder named after the plugin: something may find these files by the plugin's name"};
    case Fact::EngineFileName:
        return {K::FilePath, C::Safe, "SEQ file",
                "the SEQ file of the plugin's start-game-enabled quests: when its quests are compiled, each output "
                "gets its own SEQ file and this one is no longer read"};
    case Fact::ToolFileName:
        return {K::FilePath, C::Safe, "file name",
                "RobCo Patcher reads every INI in its folders: the file name is not used to find the plugin"};
    case Fact::FormIdIni:
        return {K::FilePath, C::Rewritable, "per-plugin FormID INI",
                "a tool INI named after the plugin, listing FormIDs relative to it: the Auto-Merger writes the same "
                "entries, with the records' new FormIDs, into an INI named after the output plugin"};
    case Fact::FormIdIniUnknown:
        return {K::FilePath, C::Unknown, "per-plugin FormID INI",
                "a tool INI named after the plugin, with lines that are not \"[section]\" or \"key=<FormID>\": "
                "what the tool reads from them is unknown"};
    case Fact::OtherFileName:
        return {K::FilePath, C::Unknown, "file name",
                "a file named after the plugin: something may find it by the plugin's name"};
    case Fact::PluginIni:
        return {K::FilePath, C::CompatibilityRequired, "plugin INI",
                "Fallout 4 reads Data\\<plugin>.ini (game settings for that mod) only while the plugin is active: "
                "compiled and disabled, its settings would no longer apply"};
    case Fact::RecordScriptString:
        return {K::PluginName, C::CompatibilityRequired, "script property in a record",
                "a record's script property holds the plugin's file name: its scripts can look records up in that "
                "file (Sim Settlements 2 add-on configs check GetFormFromFile(own FormID, this name) and reject "
                "the add-on when it does not match), so the plugin must keep its name"};
    }
    return {K::Unknown, C::Unknown, "", ""};
}

PluginReference make(Fact f, PluginIndex plugin, const std::string& path, const std::string& format,
                     const std::string& text, const std::string& location)
{
    Rule r = rule(f);
    PluginReference ref;
    ref.plugin = plugin;
    ref.assetPath = path;
    ref.kind = r.kind;
    ref.format = format;
    ref.text = text;
    ref.context = r.context;
    ref.location = location;
    ref.cls = r.cls;
    ref.reason = r.reason;
    return ref;
}

// ---- Helpers ----------------------------------------------------------------------------------
bool delimiter(char c)
{
    return c == '"' || c == '\'' || c == '|' || c == '=' || c == ',' || c == ';' || c == ':' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '<' || c == '>' || c == '\t' || c == '\n' || c == '\r' || c == '\\' ||
           c == '/' || c == '~';
}

std::string extensionFormat(const std::string& path)
{
    auto dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? std::string("txt") : path.substr(dot + 1);
    for (auto& c : ext)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext;
}

size_t lineOf(std::string_view text, size_t pos)
{
    return static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1;
}

bool commentLine(std::string_view text, size_t pos, const std::string& format)
{
    size_t start = text.rfind('\n', pos == 0 ? 0 : pos - 1);
    start = start == std::string_view::npos ? 0 : start + 1;
    while (start < pos && (text[start] == ' ' || text[start] == '\t'))
        ++start;
    if (start >= text.size())
        return false;
    if (format == "INI" || format == "TOML" || format == "TXT")
        return text[start] == ';' || text[start] == '#' || text.substr(start, 2) == "//";
    return false; // JSON and XML: no line comments the tools would skip
}

bool inPluginRequirements(std::string_view lower, size_t pos)
{
    size_t key = lower.rfind("\"pluginrequirements\"", pos);
    if (key == std::string_view::npos)
        return false;
    size_t open = lower.find('[', key);
    if (open == std::string_view::npos || open > pos)
        return false;
    size_t close = lower.find(']', open);
    return close == std::string_view::npos || close > pos;
}

// Plugin names in binary data: ASCII and UTF-16LE strings ending in .esp/.esm/.esl.
std::vector<std::string> binaryNames(const std::vector<uint8_t>& b)
{
    std::vector<std::string> out;
    auto printable = [](uint8_t c) { return c >= 0x20 && c < 0x7F && !delimiter(static_cast<char>(c)); };
    for (size_t step : {size_t(1), size_t(2)}) {
        auto at = [&](size_t i) -> int { return i < b.size() ? b[i] : -1; };
        auto zeroAfter = [&](size_t i) { return step == 1 || at(i + 1) == 0; };
        for (size_t i = 0; i + 4 * step <= b.size(); ++i) {
            if (b[i] != '.' || !zeroAfter(i))
                continue;
            std::string ext;
            bool ok = true;
            for (size_t k = 1; k <= 3; ++k) {
                int c = at(i + k * step);
                if (c < 0 || !zeroAfter(i + k * step)) {
                    ok = false;
                    break;
                }
                ext += static_cast<char>(std::tolower(c));
            }
            if (!ok || (ext != "esp" && ext != "esm" && ext != "esl"))
                continue;
            int next = at(i + 4 * step);
            if (next >= 0 && std::isalnum(next))
                continue;
            size_t start = i;
            while (start >= step && printable(b[start - step]) && zeroAfter(start - step))
                start -= step;
            std::string name;
            for (size_t k = start; k < i + 4 * step; k += step)
                name += static_cast<char>(std::tolower(b[k]));
            name = trim(name);
            if (name.size() > 4)
                out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Folders found by the plugin's name: by the game, or by a tool that loads them only for an
// installed plugin of that name.
struct NamedFolder {
    const char* prefix;
    const char* what;
};
const NamedFolder kEngineFolders[] = {
    {"meshes\\actors\\character\\facegendata\\facegeom\\", "FaceGen meshes"},
    {"textures\\actors\\character\\facecustomization\\", "FaceGen textures"},
    {"sound\\voice\\", "voice files"},
    {"textures\\terrain\\lodgen\\", "LOD billboards"},
    {"meshes\\precombined\\", "precombined meshes"},
    {"vis\\", "previs data"},
    {"f4se\\plugins\\f4ee\\overlays\\", "LooksMenu overlays (loaded only for an installed plugin of this name)"},
};

// Every asset path (model, texture, sound, ...) the plugin's records name, normalised as the asset
// index has them; read once per plugin.
const std::unordered_set<std::string>& assetPathsOf(const Session& session, PluginIndex plugin,
                                                    std::map<PluginIndex, std::unordered_set<std::string>>& cache)
{
    if (auto it = cache.find(plugin); it != cache.end())
        return it->second;
    auto& out = cache[plugin];
    for (const auto& r : session.records) {
        if (r.plugin != plugin)
            continue;
        try {
            const Record rec = const_cast<Session&>(session).loadRecord(r);
            for (const auto& sub : rec.subrecords) {
                const AssetRole role = session.defs.assetRole(r.signature, sub.signature);
                if (role == AssetRole::None || sub.data.empty())
                    continue;
                std::string value(reinterpret_cast<const char*>(sub.data.data()),
                                  strnlen(reinterpret_cast<const char*>(sub.data.data()), sub.data.size()));
                std::string path = normalizeAssetPath(value);
                if (path.empty())
                    continue;
                const std::string root = assetRoot(role);
                if (!istartsWith(path, root))
                    path = root + path;
                out.insert(toLower(path));
            }
        } catch (const std::exception&) {
        }
    }
    return out;
}

bool contentCandidate(const std::string& p, const ReferenceScanOptions& o)
{
    if (o.scripts && (iendsWith(p, ".pex") || iendsWith(p, ".psc")))
        return true;
    if (o.configs && (istartsWith(p, "f4se\\plugins\\") || istartsWith(p, "mcm\\config\\") || istartsWith(p, "mcm\\settings\\")) &&
        (iendsWith(p, ".ini") || iendsWith(p, ".json") || iendsWith(p, ".txt") || iendsWith(p, ".toml") ||
         iendsWith(p, ".xml")))
        return true;
    return o.binaries && istartsWith(p, "f4se\\plugins\\") && iendsWith(p, ".dll");
}

} // namespace

std::vector<ReferenceScanner::Mention> ReferenceScanner::mentions(std::string_view text, const std::string& lowerName)
{
    std::vector<Mention> out;
    if (lowerName.empty())
        return out;
    const std::string lower = toLower(std::string(text));
    for (size_t at = lower.find(lowerName); at != std::string::npos; at = lower.find(lowerName, at + 1)) {
        const size_t end = at + lowerName.size();
        if (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            continue; // "My Mod.esp2"
        // The name starts at a word boundary ("MyModS.esp" is not "ModS.esp"). A longer active plugin
        // name ending at the same place ("Power Armor.esp" for "Armor.esp") is preferred by inspect().
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' ||
                       text[at - 1] == '-' || text[at - 1] == '.' || static_cast<unsigned char>(text[at - 1]) >= 0x80))
            continue;
        Mention m{at, end};
        if (end < text.size() && text[end] == '|') {
            size_t q = end + 1;
            if (q + 1 < text.size() && text[q] == '0' && (text[q + 1] == 'x' || text[q + 1] == 'X'))
                q += 2;
            size_t digits = q;
            while (digits < text.size() && digits - q < 8 && std::isxdigit(static_cast<unsigned char>(text[digits])))
                ++digits;
            const bool longer = digits < text.size() && std::isxdigit(static_cast<unsigned char>(text[digits]));
            if (digits > q && !longer) {
                m.pair = true;
                m.idBegin = q;
                m.idEnd = digits;
                m.formId = static_cast<uint32_t>(std::stoul(std::string(text.substr(q, digits - q)), nullptr, 16));
            }
        }
        // "0x<FormID>:Plugin.esp" (X-Cell and other configs): the FormID written before the name.
        if (!m.pair && at >= 4 && text[at - 1] == ':') {
            size_t digits = at - 1;
            while (digits > 0 && at - 1 - digits < 8 && std::isxdigit(static_cast<unsigned char>(text[digits - 1])))
                --digits;
            if (digits < at - 1 && digits >= 2 && text[digits - 1] != 'x' && text[digits - 1] != 'X')
                digits = at - 1; // only with the 0x prefix
            if (digits < at - 1 && digits >= 2 && (text[digits - 1] == 'x' || text[digits - 1] == 'X') &&
                text[digits - 2] == '0' && (digits < 3 || !std::isalnum(static_cast<unsigned char>(text[digits - 3])))) {
                m.pair = true;
                m.idBegin = digits;
                m.idEnd = at - 1;
                m.formId = static_cast<uint32_t>(std::stoul(std::string(text.substr(digits, at - 1 - digits)), nullptr, 16));
            }
        }
        out.push_back(m);
    }
    return out;
}

namespace {
// Transfer Settlements blueprint (JSON): the key whose string value holds the name at `begin`, or
// "plugins_required" for an entry of that array.
std::string blueprintKey(std::string_view text, size_t begin)
{
    if (begin == 0 || text[begin - 1] != '"')
        return {};
    size_t i = begin - 1;
    while (i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t' || text[i - 1] == '\r' || text[i - 1] == '\n'))
        --i;
    if (i > 0 && text[i - 1] == ':') {
        size_t k = i - 1;
        while (k > 0 && text[k - 1] == ' ')
            --k;
        if (k == 0 || text[k - 1] != '"')
            return {};
        const size_t close = k - 1;
        const size_t open = text.rfind('"', close - 1);
        if (open == std::string_view::npos)
            return {};
        std::string key(text.substr(open + 1, close - open - 1));
        if (key == "plugin") {
            // "workshop": { ... "plugin": "<name>" ... }: the settlement's workbench.
            const size_t brace = text.rfind('{', open);
            if (brace != std::string_view::npos && text.find('}', brace) > begin) {
                size_t q = brace;
                while (q > 0 && (text[q - 1] == ' ' || text[q - 1] == '\t' || text[q - 1] == '\r' || text[q - 1] == '\n'))
                    --q;
                if (q > 0 && text[q - 1] == ':') {
                    const size_t kq = text.rfind('"', q - 1);
                    if (kq != std::string_view::npos && kq >= 10 && text.substr(kq - 9, 10) == "\"workshop\"")
                        return "workshop";
                }
            }
        }
        return key;
    }
    const size_t array = text.rfind("\"plugins_required\"", begin);
    if (array != std::string_view::npos && text.find(']', array) > begin)
        return "plugins_required";
    return {};
}
// The keys holding the FormID that goes with a plugin-name key, in hex (may be empty: not written)
// and in decimal; both empty for a key that is not such a pair.
//   plugin_name -> FormID, FormID_dec            (items)
//   workshop    -> id, id_dec                    (the settlement's workbench)
//   cell_plugin -> cell_id, cell_id_dec          (any "<x>_plugin")
//   PlotPlanPlugin -> PlotPlanFormID, PlotPlanFormID_dec   (any "<x>Plugin")
std::pair<std::string, std::string> blueprintIdKeys(const std::string& key)
{
    if (key == "plugin_name")
        return {"FormID", "FormID_dec"};
    if (key == "workshop")
        return {"id", "id_dec"};
    if (key.size() > 7 && key.ends_with("_plugin")) {
        const std::string x = key.substr(0, key.size() - 7);
        return {x + "_id", x + "_id_dec"};
    }
    if (key.size() > 6 && key.ends_with("Plugin")) {
        const std::string x = key.substr(0, key.size() - 6);
        return {x + "FormID", x + "FormID_dec"};
    }
    return {};
}
// The value span of "key": "..." (string) or "key": 123 (number) within [from, to).
std::pair<size_t, size_t> jsonValue(std::string_view text, size_t from, size_t to, const std::string& key)
{
    const size_t k = text.find("\"" + key + "\"", from);
    if (k == std::string_view::npos || k >= to)
        return {0, 0};
    size_t v = text.find(':', k) + 1;
    while (v < to && (text[v] == ' ' || text[v] == '\t'))
        ++v;
    if (v >= to)
        return {0, 0};
    if (text[v] == '"') {
        const size_t e = text.find('"', v + 1);
        return e < to ? std::make_pair(v + 1, e) : std::make_pair(size_t(0), size_t(0));
    }
    size_t e = v;
    while (e < to && std::isdigit(static_cast<unsigned char>(text[e])))
        ++e;
    return {v, e};
}
} // namespace

std::vector<uint8_t> ReferenceScanner::readOriginal(const Session& session, const AssetEntry& entry, size_t maxBytes)
{
    if (entry.sources.empty())
        throw std::runtime_error("no source");
    const auto copy = session.relinkedCopies.find(session.assets.path(entry));
    for (size_t slot = entry.sources.size(); slot-- > 0;) {
        auto bytes = session.assets.read(entry, slot, session.vfs, maxBytes);
        const bool ours = copy != session.relinkedCopies.end() &&
                          (copy->second == 0 ? slot + 1 == entry.sources.size() // earlier version: no hash recorded
                                             : fnv1a64(bytes.data(), bytes.size()) == copy->second);
        if (!ours)
            return bytes;
    }
    throw std::runtime_error("only the Auto-Merger's own copy provides this file");
}

ReferenceScanner::PluginNames ReferenceScanner::pluginNames(const Session& session)
{
    PluginNames names;
    for (auto pi : session.loadOrder)
        if (!session.plugins[pi].baseGame)
            names[session.plugins[pi].key] = pi;
    return names;
}

namespace {
// A plugin name that enters a variable, classified by where the value goes (pex::ValueFlow): only
// GetFormFromFile with constant FormIDs and presence checks (and log text) is rewritable, one
// reference per use, all sharing the constant to rewrite.
void flowReferences(std::vector<PluginReference>& out, const pex::ValueFlow& flow, PluginIndex plugin,
                    const std::string& path, const std::string& format, const std::string& text,
                    const pex::StringUsage& use)
{
    using K = pex::ValueSink::Kind;
    const pex::ValueSink* other = nullptr;
    const pex::ValueSink* runtime = nullptr;
    std::vector<const pex::ValueSink*> lookups;
    const pex::ValueSink* presence = nullptr;
    for (const auto& s : flow.sinks) {
        if (s.kind == K::Other && !other)
            other = &s;
        else if (s.kind == K::GetFormFromFile && !s.formIdLiteral && !runtime)
            runtime = &s;
        else if (s.kind == K::GetFormFromFile && s.formIdLiteral)
            lookups.push_back(&s);
        else if (s.kind == K::IsPluginInstalled && !presence)
            presence = &s;
    }
    const std::string how = std::string(pex::stringUseName(use.use));
    if (other) {
        auto r = make(Fact::PexOtherUse, plugin, path, format, text, other->where);
        r.context = how + ", then " + other->what;
        r.pexStringOffset = flow.offset;
        out.push_back(std::move(r));
        return;
    }
    if (runtime) {
        auto r = make(Fact::PexGetFormRuntime, plugin, path, format, text, runtime->where);
        r.pexStringOffset = flow.offset;
        out.push_back(std::move(r));
        return;
    }
    if (lookups.empty() && !presence && flow.overridable) {
        // An Auto property: other scripts can read it, and the plugin's script data can set it.
        auto r = make(Fact::PexOtherUse, plugin, path, format, text, use.where);
        r.context = "Auto property default";
        r.pexStringOffset = flow.offset;
        out.push_back(std::move(r));
        return;
    }
    if (lookups.empty() && !presence) {
        auto r = make(Fact::PexUnused, plugin, path, format, text, flow.sinks.empty() ? use.where : flow.sinks[0].where);
        r.context = flow.sinks.empty() ? how + ", never used" : how + ", used only in log text";
        r.reason = flow.sinks.empty() ? "stored by the script but never used: nothing depends on it"
                                      : "used only to write log text: nothing in the game depends on it";
        out.push_back(std::move(r));
        return;
    }
    if (flow.overridable) {
        auto r = make(Fact::PexOtherUse, plugin, path, format, text, use.where);
        r.cls = ReferenceClass::CompatibilityRequired;
        r.context = "Auto property default";
        r.reason = "the default value of an Auto property, used for GetFormFromFile or IsPluginInstalled; the "
                   "plugin's script data can set the property to another value, which this app does not check yet";
        out.push_back(std::move(r));
        return;
    }
    for (const auto* s : lookups) {
        auto r = make(Fact::PexGetFormFixed, plugin, path, format, text, s->where);
        r.hasFormId = true;
        r.formId = static_cast<uint32_t>(s->formId);
        r.pexFormIdOffset = s->formIdOffset;
        r.pexStringOffset = flow.offset;
        r.context = "Game.GetFormFromFile(" + formatFormId(r.formId) + ") via " + how;
        out.push_back(std::move(r));
    }
    if (presence) {
        auto r = make(Fact::PexPluginInstalled, plugin, path, format, text, presence->where);
        r.pexStringOffset = flow.offset;
        r.context = "Game.IsPluginInstalled via " + how;
        out.push_back(std::move(r));
    }
}
} // namespace

std::vector<PluginReference> ReferenceScanner::inspect(const PluginNames& names, const std::string& path,
                                                       const std::vector<uint8_t>& bytes, const std::string& source,
                                                       std::vector<pex::PropertyRead>* propertyReads,
                                                       std::pair<std::string, std::string>* extends,
                                                       std::vector<std::string>* members)
{
    std::vector<PluginReference> out;
    const std::string format = extensionFormat(path);

    if (format == "PEX") {
        auto scan = pex::scan(bytes);
        if (extends && scan.ok)
            *extends = {scan.objectName, scan.parentName};
        if (members && scan.ok)
            *members = scan.members;
        if (propertyReads && scan.ok)
            *propertyReads = std::move(scan.propertyReads);
        // A script that does not read back may read any property: each of its strings counts as a read
        // of unknown meaning, so no constant it could name is taken for unread.
        if (propertyReads && !scan.ok)
            for (const auto& str : AssetScanner::pexStrings(bytes)) {
                pex::PropertyRead pr;
                pr.property = toLower(str);
                pr.opcode = "unreadable script";
                propertyReads->push_back(std::move(pr));
            }
        if (!scan.ok) {
            for (const auto& s : AssetScanner::pexStrings(bytes))
                if (auto it = names.find(toLower(s)); it != names.end()) {
                    auto r = make(Fact::PexUnreadable, it->second, path, format, s, scan.error);
                    out.push_back(std::move(r));
                }
        } else {
            std::map<size_t, const pex::ValueFlow*> flowAt; // constant offset -> where its value goes
            for (const auto& f : scan.flows)
                flowAt[f.offset] = &f;
            for (size_t i = 0; i < scan.strings.size(); ++i) {
                auto it = names.find(toLower(scan.strings[i]));
                if (it == names.end())
                    continue;
                bool used = false;
                for (const auto& u : scan.uses) {
                    if (u.string != i)
                        continue;
                    used = true;
                    if (auto fl = flowAt.find(u.offset);
                        fl != flowAt.end() && (u.use == pex::StringUse::Default || u.use == pex::StringUse::Assignment ||
                                               u.use == pex::StringUse::CallArgument)) {
                        flowReferences(out, *fl->second, it->second, path, format, scan.strings[i], u);
                        continue;
                    }
                    Fact f = u.use == pex::StringUse::GetFormFromFile
                                 ? (u.formIdLiteral ? Fact::PexGetFormFixed : Fact::PexGetFormRuntime)
                             : u.use == pex::StringUse::IsPluginInstalled ? Fact::PexPluginInstalled
                             : u.use == pex::StringUse::PropertyValue     ? Fact::PexConstantProperty
                                                                          : Fact::PexOtherUse;
                    auto r = make(f, it->second, path, format, scan.strings[i], u.where);
                    if (f == Fact::PexOtherUse)
                        r.context = std::string(pex::stringUseName(u.use)) +
                                    (!u.function.empty()                          ? " (" + u.function + ")"
                                     : u.opcode.empty() || u.use != pex::StringUse::Other ? std::string()
                                                                                   : " (" + u.opcode + ")");
                    if (f == Fact::PexGetFormFixed) {
                        r.hasFormId = true;
                        r.formId = static_cast<uint32_t>(u.formId);
                        r.pexFormIdOffset = u.formIdOffset;
                        r.context = "Game.GetFormFromFile(" + formatFormId(r.formId) + ")";
                    }
                    r.pexStringOffset = u.offset;
                    out.push_back(std::move(r));
                }
                if (!used)
                    out.push_back(make(Fact::PexUnused, it->second, path, format, scan.strings[i], {}));
            }
        }
    } else if (format == "DLL") {
        for (const auto& n : binaryNames(bytes))
            if (auto it = names.find(n); it != names.end())
                out.push_back(make(Fact::BinaryString, it->second, path, format, n, {}));
    } else {
        // Text: Papyrus sources and configuration files.
        std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::string lower = toLower(std::string(text));
        // Every active plugin name that ends at an ".esp/.esm/.esl" in the text (mentions() then
        // checks that it is the whole name).
        std::set<size_t> lengths;
        for (const auto& kv : names)
            lengths.insert(kv.first.size());
        std::set<std::string> candidates;
        for (size_t dot = lower.find(".es"); dot != std::string::npos; dot = lower.find(".es", dot + 1)) {
            const size_t end = dot + 4;
            if (end > lower.size() || (lower[dot + 3] != 'p' && lower[dot + 3] != 'm' && lower[dot + 3] != 'l'))
                continue;
            for (size_t len : lengths)
                if (len <= end && names.count(lower.substr(end - len, len)))
                    candidates.insert(lower.substr(end - len, len));
        }
        const bool mcm = istartsWith(path, "mcm\\config\\") && iendsWith(path, ".json");
        const bool blueprint = istartsWith(path, "f4se\\plugins\\transfersettlements\\");
        // The longest candidate ending at each place wins.
        std::map<size_t, size_t> longest; // end -> length
        for (const auto& name : candidates)
            for (const auto& m : mentions(text, name))
                longest[m.end] = std::max(longest[m.end], name.size());
        for (const auto& name : candidates) {
            auto it = names.find(name);
            if (it == names.end())
                continue;
            for (const auto& m : mentions(text, name)) {
                if (longest[m.end] != name.size())
                    continue;
                Fact f;
                std::string bpKey;
                if (format == "PSC")
                    f = Fact::PscSource;
                else if (commentLine(text, m.begin, format))
                    f = Fact::ConfigComment;
                else if (blueprint) {
                    bpKey = blueprintKey(text, m.begin);
                    f = bpKey == "plugins_required"         ? Fact::BlueprintRequired
                        : blueprintIdKeys(bpKey).second.empty() ? Fact::SettlementBlueprint
                                                                : Fact::BlueprintItem;
                }
                else if (m.pair)
                    f = Fact::ConfigPair;
                else if (mcm && inPluginRequirements(lower, m.begin))
                    f = Fact::McmRequirement;
                else
                    f = Fact::ConfigPlain;
                // A pair reads "Plugin|FormID" whichever order the file writes it in.
                const std::string found =
                    !m.pair                ? std::string(text.substr(m.begin, m.end - m.begin))
                    : m.idBegin > m.begin ? std::string(text.substr(m.begin, m.idEnd - m.begin))
                                           : std::string(text.substr(m.begin, m.end - m.begin)) + "|" +
                                                 std::string(text.substr(m.idBegin, m.idEnd - m.idBegin));
                auto r = make(f, it->second, path, format, found,
                              "line " + std::to_string(lineOf(text, m.begin)));
                r.nameBegin = m.begin;
                r.nameEnd = m.end;
                if (m.pair) {
                    r.hasFormId = true;
                    r.formId = m.formId;
                    r.idBegin = m.idBegin;
                    r.idEnd = m.idEnd;
                }
                if (f == Fact::BlueprintItem) {
                    // The object the name belongs to: its FormID in hex (when the blueprint writes it)
                    // and the same number in decimal, keys of the same JSON object.
                    const auto [hexKey, decKey] = blueprintIdKeys(bpKey);
                    const size_t open = text.rfind('{', m.begin);
                    const size_t close = text.find('}', m.end);
                    const size_t from = open == std::string_view::npos ? 0 : open;
                    const auto hex = hexKey.empty() ? std::pair<size_t, size_t>{0, 0} : jsonValue(text, from, close, hexKey);
                    const auto dec = jsonValue(text, from, close, decKey);
                    // Items always write both; the other pairs may write only the decimal.
                    bool ok = dec.second > dec.first && dec.second - dec.first <= 10 && hex.second - hex.first <= 8 &&
                              (bpKey != "plugin_name" || hex.second > hex.first);
                    for (size_t c = dec.first; ok && c < dec.second; ++c)
                        ok = std::isdigit(static_cast<unsigned char>(text[c])) != 0;
                    for (size_t c = hex.first; ok && c < hex.second; ++c)
                        ok = std::isxdigit(static_cast<unsigned char>(text[c])) != 0;
                    uint64_t value = 0;
                    if (ok)
                        value = std::stoull(std::string(text.substr(dec.first, dec.second - dec.first)));
                    // Both written: they must agree, or which one the tool reads is unknown.
                    if (ok && hex.second > hex.first &&
                        std::stoull(std::string(text.substr(hex.first, hex.second - hex.first)), nullptr, 16) != value)
                        ok = false;
                    if (ok && value <= 0xFFFFFF) {
                        r.hasFormId = true;
                        r.formId = static_cast<uint32_t>(value);
                        r.idBegin = hex.first;
                        r.idEnd = hex.second;
                        r.decBegin = dec.first;
                        r.decEnd = dec.second;
                        r.text = std::string(text.substr(m.begin, m.end - m.begin)) + "|" + formatFormId(r.formId).substr(2);
                        if (bpKey == "workshop") {
                            const auto base = jsonValue(text, from, close, "baseid_dec");
                            r.baseBegin = base.first;
                            r.baseEnd = base.second;
                            // Its base object (baseid_dec) is stored without a plugin name: the Auto-Merger
                            // rewrites the workbench only when that base does not move (checked against the
                            // workbench's record).
                            r.context = "Transfer Settlements workbench";
                            r.reason = "the settlement's workbench: its plugin name and FormID (hex and decimal) are "
                                       "rewritten together when its base object is from a plugin that is not compiled";
                        } else if (bpKey != "plugin_name") {
                            r.context = "Transfer Settlements " + bpKey;
                            r.reason = "a plugin name with its FormID (" + bpKey + "): rewritten together";
                        }
                    } else {
                        r.cls = ReferenceClass::Unknown;
                        r.reason = "a blueprint entry (" + bpKey + ") whose FormID could not be read";
                    }
                }
                out.push_back(std::move(r));
            }
        }
    }
    for (auto& r : out)
        r.source = source;
    return out;
}

bool ReferenceScanner::isFormIdIniPath(const std::string& lowerPath, const std::string& lowerPlugin)
{
    return lowerPath == "bcr\\" + lowerPlugin + ".ini";
}

ReferenceScanner::FormIdIni ReferenceScanner::parseFormIdIni(std::string_view text)
{
    FormIdIni out;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3); // UTF-8 byte order mark
    out.crlf = text.find("\r\n") != std::string_view::npos;
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
            v.remove_suffix(1);
        return v;
    };
    std::string section;
    size_t lineNo = 0;
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos)
            nl = text.size();
        const auto line = trim(text.substr(at, nl - at));
        at = nl + 1;
        ++lineNo;
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        auto fail = [&](const std::string& why) {
            out.problem = "line " + std::to_string(lineNo) + ": " + why;
            return out;
        };
        if (line.front() == '[') {
            if (line.back() != ']' || line.size() < 3)
                return fail("not a section header");
            section = std::string(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || section.empty())
            return fail(section.empty() ? "an entry outside a section" : "not \"key=value\"");
        FormIdIni::Entry e;
        e.section = section;
        e.key = std::string(trim(line.substr(0, eq)));
        auto value = trim(line.substr(eq + 1));
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
            e.prefixed = true;
            value.remove_prefix(2);
        }
        if (e.key.empty() || value.empty() || value.size() > 6)
            return fail("not a FormID of up to 6 hex digits");
        for (char c : value) {
            if (!std::isxdigit(static_cast<unsigned char>(c)))
                return fail("not a FormID of up to 6 hex digits");
            e.uppercase |= c >= 'A' && c <= 'F';
        }
        e.formId = static_cast<uint32_t>(std::stoul(std::string(value), nullptr, 16));
        out.entries.push_back(std::move(e));
    }
    if (out.entries.empty())
        return out.problem = "no FormIDs", out;
    out.ok = true;
    return out;
}

ReferenceDecision ReferenceScanner::decide(const std::vector<const PluginReference*>& references)
{
    ReferenceDecision d;
    for (const auto* r : references) {
        d.counts[static_cast<size_t>(r->cls)]++;
        d.total++;
        if (static_cast<int>(r->cls) > static_cast<int>(d.cls))
            d.cls = r->cls;
    }
    std::vector<std::string> parts;
    for (auto c : {ReferenceClass::Rewritable, ReferenceClass::CompatibilityRequired, ReferenceClass::Unsafe,
                   ReferenceClass::Unknown, ReferenceClass::Safe})
        if (size_t n = d.counts[static_cast<size_t>(c)])
            parts.push_back(std::to_string(n) + " " + toLower(referenceClassName(c)));
    d.summary = std::to_string(d.total) + " filename reference" + (d.total == 1 ? "" : "s") +
                (parts.empty() ? std::string() : ": " + join(parts, ", "));
    return d;
}

void ReferenceScanner::scan(Session& session, const ReferenceScanOptions& options, ProgressSink& progress)
{
    session.nameReferences.clear();
    const auto names = pluginNames(session);
    const auto& idx = session.assets;

    std::vector<uint32_t> candidates;
    std::map<std::pair<PluginIndex, std::string>, std::pair<size_t, std::string>> folders; // -> (files, mod)
    std::map<std::pair<PluginIndex, std::string>, std::vector<std::string>> folderFiles;
    for (uint32_t i = 0; i < idx.entries.size(); ++i) {
        const auto& p = idx.path(idx.entries[i]);
        if (contentCandidate(p, options))
            candidates.push_back(i);
        if (!options.paths)
            continue;
        if (istartsWith(p, "seq\\") && iendsWith(p, ".seq")) {
            // SEQ files are named after the plugin without its extension.
            const std::string base = toLower(p.substr(4, p.size() - 8));
            for (const auto& [name, plugin] : names)
                if (session.plugins[plugin].baseName().size() == base.size() &&
                    toLower(session.plugins[plugin].baseName()) == base) {
                    auto r = make(Fact::EngineFileName, plugin, toLower(p), "File name", p.substr(4), {});
                    r.source = idx.modName(idx.entries[i].winner());
                    session.nameReferences.push_back(std::move(r));
                }
            continue;
        }
        if (p.find(".es") == std::string::npos && p.find(".ES") == std::string::npos)
            continue;
        // Folders and the file name.
        size_t begin = 0;
        for (size_t slash = p.find('\\'); slash != std::string::npos; slash = p.find('\\', begin)) {
            const std::string folder = toLower(p.substr(begin, slash - begin));
            if (auto it = names.find(folder); it != names.end()) {
                auto& f = folders[{it->second, toLower(p.substr(0, slash + 1))}];
                if (f.first++ == 0)
                    f.second = idx.modName(idx.entries[i].winner());
                folderFiles[{it->second, toLower(p.substr(0, slash + 1))}].push_back(toLower(p));
            }
            begin = slash + 1;
        }
        const std::string file = p.substr(begin);
        for (const auto& n : AssetScanner::pluginNamesInText(file)) {
            auto it = names.find(n);
            if (it == names.end() || toLower(file) == n)
                continue;
            Fact f = istartsWith(p, "f4se\\plugins\\robco_patcher\\") ? Fact::ToolFileName : Fact::OtherFileName;
            std::string problem;
            if (isFormIdIniPath(toLower(p), n)) {
                try {
                    const auto bytes = readOriginal(session, idx.entries[i], 1 << 20);
                    const auto ini = parseFormIdIni(
                        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                    f = ini.ok ? Fact::FormIdIni : Fact::FormIdIniUnknown;
                    problem = ini.problem;
                } catch (const std::exception& e) {
                    f = Fact::FormIdIniUnknown;
                    problem = std::string("could not be read: ") + e.what();
                }
            }
            auto r = make(f, it->second, toLower(p), "File name", file, {});
            if (!problem.empty())
                r.reason += " (" + problem + ")";
            r.source = idx.modName(idx.entries[i].winner());
            session.nameReferences.push_back(std::move(r));
        }
    }
    // Data\<plugin>.ini: loose INI files in the Data folder named after an active plugin (without its
    // extension). The game reads them with its own INIs, but only for plugins that are active.
    if (options.paths)
        for (const auto* f : session.vfs.rootFiles()) {
            const std::string& rel = f->relativePath;
            if (!iendsWith(rel, ".ini") || rel.size() <= 4)
                continue;
            const std::string base = toLower(rel.substr(0, rel.size() - 4));
            for (const auto& [name, plugin] : names) {
                if (toLower(session.plugins[plugin].baseName()) != base)
                    continue;
                auto r = make(Fact::PluginIni, plugin, toLower(rel), "INI", rel, {});
                r.source = session.vfs.winnerName(*f);
                session.nameReferences.push_back(std::move(r));
            }
        }
    std::map<PluginIndex, std::unordered_set<std::string>> pathCache;
    for (const auto& [key, value] : folders) {
        const auto& [plugin, folder] = key;
        const NamedFolder* engine = nullptr;
        for (const auto& nf : kEngineFolders)
            if (folder == std::string(nf.prefix) + session.plugins[plugin].key + "\\")
                engine = &nf;
        // Voice files and FaceGen move with their records at export (Exporter relocations).
        const std::string pre = engine ? engine->prefix : "";
        const bool voice = pre == "sound\\voice\\" || pre == "meshes\\actors\\character\\facegendata\\facegeom\\" ||
                           pre == "textures\\actors\\character\\facecustomization\\";
        // Any other folder: Safe when the plugin's records name every file in it by path (static
        // collection meshes in meshes\scol\<plugin>\, sound files): those are found by path, not by name.
        bool byPath = false;
        bool assetRoot = false;
        if (!engine)
            for (const char* root : {"meshes\\", "textures\\", "materials\\", "sound\\", "music\\"})
                assetRoot |= istartsWith(folder, root);
        if (!engine && !assetRoot) {
            const auto& named = assetPathsOf(session, plugin, pathCache);
            const auto& files = folderFiles[key];
            byPath = !files.empty() && std::all_of(files.begin(), files.end(),
                                                   [&](const std::string& f) { return named.count(f) > 0; });
        }
        // Precombined meshes and previs files are named after the cell's defining plugin and its
        // FormID, and only cells with precombine or previs data read them: the compiler refuses such
        // new cells (Precombined Cell), which decides by the records themselves.
        const bool cellData = pre == "meshes\\precombined\\" || pre == "vis\\";
        auto r = make(voice       ? Fact::VoiceFolder
                      : cellData  ? Fact::CellDataFolder
                      : engine    ? Fact::EngineFolder
                      : assetRoot ? Fact::AssetFolder
                      : byPath    ? Fact::RecordPathFolder
                                  : Fact::OtherFolder,
                      plugin, folder, "Folder", session.plugins[plugin].key, std::to_string(value.first) + " files");
        if (voice) {
            r.context = engine->what;
            r.reason = std::string(engine->what) + ": copied at export into the output's folder, named after the "
                                                   "records' new FormIDs, so the game finds them as before";
        }
        if (cellData)
            r.context = engine->what;
        if (engine && !voice && !cellData) {
            r.context = engine->what;
            r.reason = std::string(engine->what) + " are found by the plugin's file name; moving them to the output "
                                                   "is not implemented";
        }
        r.source = value.second;
        session.nameReferences.push_back(std::move(r));
    }

    progress.stage("Scanning scripts and configs for plugin names");
    std::vector<std::vector<PluginReference>> found(candidates.size());
    std::vector<std::vector<pex::PropertyRead>> reads(candidates.size());
    std::vector<std::pair<std::string, std::string>> extends(candidates.size()); // (script, parent)
    std::vector<std::vector<std::string>> members(candidates.size());            // its own functions, properties
    std::atomic<size_t> scanned{0};
    parallelFor(
        candidates.size(),
        [&](size_t i) {
            const auto& e = idx.entries[candidates[i]];
            const auto& path = idx.path(e);
            try {
                auto bytes = readOriginal(session, e);
                found[i] = inspect(names, toLower(path), bytes, idx.modName(e.winner()), &reads[i], &extends[i],
                                   &members[i]);
            } catch (const std::exception& ex) {
                spdlog::debug("Could not read {}: {}", path, ex.what());
            }
            size_t n = ++scanned;
            if ((n & 0xFF) == 0 || n == candidates.size())
                progress.progress(n, candidates.size(), path);
        },
        progress, options.threads);
    for (auto& v : found)
        for (auto& r : v)
            session.nameReferences.push_back(std::move(r));

    // Named constants (a property whose read handler returns the plugin's name, such as a mod's table
    // of plugin names): what the name means is what the scripts reading the property do with it.
    {
        struct Reader {
            const pex::PropertyRead* read;
            std::string script;
            uint32_t entry;
        };
        std::vector<PluginReference> readerRefs; // added after the loop (it walks nameReferences)
        std::unordered_map<std::string, std::vector<Reader>> readers; // property -> its reads
        for (size_t i = 0; i < candidates.size(); ++i)
            for (const auto& pr : reads[i])
                readers[pr.property].push_back({&pr, idx.path(idx.entries[candidates[i]]), candidates[i]});
        // Script inheritance across the load order: a read reaches a constant of script X when it is
        // read from X, from a script extending X (inherited), or from a script X extends (X's version
        // may run). A read whose type is unknown, or names a script not in the load order, counts.
        std::unordered_map<std::string, std::string> parentOf;
        std::unordered_map<std::string, std::unordered_set<std::string>> membersOf;
        for (size_t i = 0; i < extends.size(); ++i)
            if (!extends[i].first.empty()) {
                parentOf[extends[i].first] = extends[i].second;
                membersOf[extends[i].first].insert(members[i].begin(), members[i].end());
            }
        // A read that certainly reaches the owner's member: made on the owner, or on a script extending
        // it where no script in between declares a member of that name (it would be that one's).
        auto certain = [&](const std::string& readFrom, const std::string& owner, const std::string& member) {
            std::string s = readFrom;
            for (int depth = 0; depth < 64; ++depth) {
                if (s == owner)
                    return true;
                auto it = parentOf.find(s);
                if (it == parentOf.end() || membersOf[s].count(member))
                    return false;
                s = it->second;
            }
            return false;
        };
        auto ancestors = [&](std::string s) {
            std::set<std::string> out;
            for (int depth = 0; depth < 64 && !s.empty() && out.insert(s).second; ++depth) {
                auto it = parentOf.find(s);
                if (it == parentOf.end())
                    break;
                s = it->second;
            }
            return out;
        };
        auto reaches = [&](const std::string& readFrom, const std::string& owner) {
            if (readFrom.empty() || !parentOf.count(readFrom))
                return true;
            return ancestors(readFrom).count(owner) > 0 || ancestors(owner).count(readFrom) > 0;
        };
        for (auto& r : session.nameReferences) {
            if (r.context != "constant property")
                continue;
            const auto dot = r.location.rfind('.');
            const std::string property = toLower(dot == std::string::npos ? r.location : r.location.substr(dot + 1));
            const std::string owner = toLower(r.location.substr(0, r.location.find('.')));
            auto it = readers.find(property);
            std::vector<Reader> own;
            if (it != readers.end())
                for (const auto& rd : it->second)
                    if (reaches(rd.read->object, owner))
                        own.push_back(rd);
            if (own.empty()) {
                r.cls = ReferenceClass::Safe;
                r.reason = "the value of the constant " + r.location + ", which no script in the load order reads";
                continue;
            }
            size_t gfff = 0, presence = 0, other = 0;
            const Reader* firstGfff = nullptr;
            const Reader* firstOther = nullptr;
            for (const auto& rd : own) {
                if (rd.read->use == pex::PropertyRead::Use::GetFormFromFile) {
                    ++gfff;
                    if (!firstGfff)
                        firstGfff = &rd;
                } else if (rd.read->use == pex::PropertyRead::Use::IsPluginInstalled) {
                    ++presence;
                } else {
                    ++other;
                    if (!firstOther)
                        firstOther = &rd;
                }
            }
            const std::string counts = std::to_string(own.size()) + " reads: " + std::to_string(gfff) +
                                       " GetFormFromFile, " + std::to_string(presence) + " IsPluginInstalled, " +
                                       std::to_string(other) + " other";
            // Read only by lookups with a constant FormID, made on the owner script itself (not a script
            // it extends or one extending it, whose version may differ), and presence checks: the
            // constant can name the output, and each lookup's FormID becomes the record's new one.
            bool exact = gfff > 0 && other == 0;
            std::string blocker; // the first read that is not rewritable, for the reason
            for (const auto& rd : own) {
                std::string why;
                if (rd.read->use == pex::PropertyRead::Use::Other)
                    why = "used by " + rd.read->opcode;
                else if (rd.read->use == pex::PropertyRead::Use::GetFormFromFile && !rd.read->formIdLiteral)
                    why = "GetFormFromFile with a FormID computed at run time";
                else if (rd.read->use == pex::PropertyRead::Use::GetFormFromFile &&
                         !certain(rd.read->object, owner, property))
                    why = rd.read->object.empty() ? "GetFormFromFile on an object whose script type is unknown"
                                                  : "GetFormFromFile on " + rd.read->object +
                                                        ", whose member of that name may not be this one";
                if (why.empty())
                    continue;
                exact = false;
                if (blocker.empty())
                    blocker = rd.script + " (" + rd.read->where + "): " + why;
            }
            if (exact) {
                r.cls = ReferenceClass::Rewritable;
                r.context = "constant property read by GetFormFromFile";
                r.reason = "the value of the constant " + r.location + " is only used by lookups with a constant FormID "
                           "on " + r.location.substr(0, r.location.find('.')) + " and presence checks (" + counts +
                           "): it can name the output plugin, and each lookup gets the record's new FormID";
                for (const auto& rd : own) {
                    if (rd.read->use != pex::PropertyRead::Use::GetFormFromFile)
                        continue;
                    PluginReference rr;
                    rr.plugin = r.plugin;
                    rr.assetPath = toLower(rd.script);
                    rr.source = idx.modName(idx.entries[rd.entry].winner());
                    rr.kind = ReferenceKind::Papyrus;
                    rr.format = "PEX";
                    rr.text = r.location; // the constant it reads
                    rr.hasFormId = true;
                    rr.formId = static_cast<uint32_t>(rd.read->formId);
                    rr.pexFormIdOffset = rd.read->formIdOffset;
                    rr.context = "Game.GetFormFromFile(" + formatFormId(rr.formId) + ") through the constant";
                    rr.location = rd.read->where;
                    rr.cls = ReferenceClass::Rewritable;
                    rr.reason = "a lookup of a record of the plugin, its name read from the constant " + r.location +
                                ": its FormID becomes the record's new one in the output the constant names";
                    readerRefs.push_back(std::move(rr));
                }
            } else if (gfff) {
                r.cls = ReferenceClass::CompatibilityRequired;
                r.context = "constant property read by GetFormFromFile";
                r.reason = "the value of the constant " + r.location + " is the file name " + firstGfff->script +
                           " (" + firstGfff->read->where + ") passes to Game.GetFormFromFile" +
                           (firstGfff->read->formIdLiteral ? " with FormID " + formatFormId(static_cast<uint32_t>(firstGfff->read->formId))
                                                           : std::string(" with a FormID computed at run time")) +
                           " (" + counts + "); not rewritable because of " + blocker;
            } else if (other) {
                r.cls = ReferenceClass::Unknown;
                r.context = "constant property read by other code";
                r.reason = "the value of the constant " + r.location + " is used by " + firstOther->script + " (" +
                           firstOther->read->where + ", " + firstOther->read->opcode + ") in a way whose meaning cannot be "
                           "established (" + counts + ")";
            } else {
                // Only presence checks read it: naming the output plugin, which is installed, keeps every
                // check true, as the original was. The Auto-Merger rewrites the constant.
                r.cls = ReferenceClass::Rewritable;
                r.kind = ReferenceKind::PluginName;
                r.context = "constant property read by IsPluginInstalled";
                r.reason = "the value of the constant " + r.location + " is only used for presence checks "
                           "(Game.IsPluginInstalled, " + counts + "): it can name the output plugin, which is installed";
            }
        }
        for (auto& rr : readerRefs)
            session.nameReferences.push_back(std::move(rr));
    }

    // Script properties in the plugins' own records (VMAD strings) holding a plugin's file name.
    if (options.records) {
        progress.stage("Scanning script properties for plugin names");
        std::vector<PluginIndex> scan;
        for (const auto& p : session.plugins)
            if (p.active && !p.generated && p.error.empty() && !p.baseGame)
                scan.push_back(p.index);
        std::vector<std::vector<PluginReference>> perPlugin(scan.size());
        auto hasPluginExtension = [](std::span<const uint8_t> d) {
            for (size_t i = 0; i + 4 <= d.size(); ++i) {
                if (d[i] != '.' || (d[i + 1] | 0x20) != 'e' || (d[i + 2] | 0x20) != 's')
                    continue;
                const uint8_t c = d[i + 3] | 0x20;
                if (c == 'p' || c == 'm' || c == 'l')
                    return true;
            }
            return false;
        };
        std::atomic<size_t> done{0};
        parallelFor(
            scan.size(),
            [&](size_t i) {
                const auto& p = session.plugins[scan[i]];
                try {
                    const auto file = PluginFile::load(p.path);
                    file.forEachRecord([&](const RecordLocation& loc) {
                        const Signature sig = loc.header.signature;
                        // Records that never carry scripts, and the largest ones.
                        if (sig == Signature("LAND") || sig == Signature("NAVM") || sig == Signature("NAVI") ||
                            sig == Signature("CELL") || sig == Signature("WRLD"))
                            return true;
                        std::vector<uint8_t> inflated;
                        std::span<const uint8_t> data = loc.rawData;
                        if (loc.header.compressed()) {
                            inflated = Record::decompress(loc.header, loc.rawData);
                            data = inflated;
                        }
                        if (!hasPluginExtension(data))
                            return true;
                        const auto subs = Record::parseSubrecords(data);
                        if (std::none_of(subs.begin(), subs.end(), [](const Subrecord& s) { return s.signature == Signature("VMAD"); }))
                            return true;
                        const auto layout = decodeLayout(session.defs, sig, subs);
                        std::string edid;
                        for (const auto& s : subs)
                            if (s.signature == Signature("EDID") && !s.data.empty()) {
                                edid.assign(reinterpret_cast<const char*>(s.data.data()),
                                            strnlen(reinterpret_cast<const char*>(s.data.data()), s.data.size()));
                                break;
                            }
                        for (const auto& value : layout.scriptPluginNames) {
                            auto it = names.find(toLower(value));
                            if (it == names.end())
                                continue;
                            char id[16];
                            std::snprintf(id, sizeof(id), "%08X", loc.header.formId);
                            auto r = make(Fact::RecordScriptString, it->second, p.key, "Plugin record", value,
                                          sig.str() + " " + (edid.empty() ? std::string() : edid + " ") + "[" + id + "]");
                            r.source = p.sourceMod;
                            perPlugin[i].push_back(std::move(r));
                        }
                        return true;
                    });
                } catch (const std::exception& ex) {
                    spdlog::debug("Script properties of {} not scanned: {}", p.name, ex.what());
                }
                size_t n = ++done;
                progress.progress(n, scan.size(), p.name);
            },
            progress, options.threads);
        size_t count = 0;
        for (auto& v : perPlugin)
            for (auto& r : v) {
                session.nameReferences.push_back(std::move(r));
                ++count;
            }
        if (count)
            spdlog::info("{} script properties in plugin records hold a plugin's file name", count);
    }

    std::stable_sort(session.nameReferences.begin(), session.nameReferences.end(),
                     [](const PluginReference& a, const PluginReference& b) {
                         return std::tie(a.plugin, a.assetPath, a.nameBegin) < std::tie(b.plugin, b.assetPath, b.nameBegin);
                     });
    spdlog::info("{} references to plugin file names in {} scripts, configs and paths", session.nameReferences.size(),
                 candidates.size());
}

} // namespace fo4mx
