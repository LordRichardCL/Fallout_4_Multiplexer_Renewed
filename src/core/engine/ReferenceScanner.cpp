#include "engine/ReferenceScanner.hpp"

#include "assets/AssetScanner.hpp"
#include "engine/ParallelFor.hpp"
#include "papyrus/Pex.hpp"
#include "util/Text.hpp"

#include <algorithm>
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
    PexUnused,           // in the string table, not used by code
    PexUnreadable,       // the script does not read back exactly
    PscSource,           // Papyrus source
    ConfigComment,       // a comment line
    ConfigPair,          // "Plugin.esp|FormID"
    McmRequirement,      // MCM config.json "pluginRequirements"
    ConfigPlain,         // a config naming the plugin without a FormID
    SettlementBlueprint, // Transfer Settlements blueprint
    BinaryString,        // F4SE DLL
    EngineFolder,        // FaceGen / face textures / voice / LOD billboards folder named after the plugin
    OtherFolder,         // any other folder named after the plugin
    EngineFileName,      // seq\<plugin>.seq (start-game-enabled quests)
    ToolFileName,        // RobCo Patcher INI named after the plugin
    OtherFileName,       // any other file named after the plugin
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
    case Fact::SettlementBlueprint:
        return {K::Config, C::Unsafe, "Transfer Settlements blueprint", "saved settlement data: not changed"};
    case Fact::BinaryString:
        return {K::Binary, C::Unsafe, "compiled code",
                "an F4SE plugin (DLL) contains the name: what its code does with it cannot be established or changed"};
    case Fact::EngineFolder:
        return {K::Directory, C::CompatibilityRequired, "engine folder",
                "the game finds these files by the defining plugin's name; moving them to the output is not "
                "implemented"};
    case Fact::OtherFolder:
        return {K::Directory, C::Unknown, "folder",
                "a folder named after the plugin: something may find these files by the plugin's name"};
    case Fact::EngineFileName:
        return {K::FilePath, C::CompatibilityRequired, "engine file",
                "the game finds this file by the plugin's name (SEQ file of start-game-enabled quests); writing one "
                "for the output is not implemented"};
    case Fact::ToolFileName:
        return {K::FilePath, C::Safe, "file name",
                "RobCo Patcher reads every INI in its folders: the file name is not used to find the plugin"};
    case Fact::OtherFileName:
        return {K::FilePath, C::Unknown, "file name",
                "a file named after the plugin: something may find it by the plugin's name"};
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
        out.push_back(m);
    }
    return out;
}

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

std::vector<PluginReference> ReferenceScanner::inspect(const PluginNames& names, const std::string& path,
                                                       const std::vector<uint8_t>& bytes, const std::string& source)
{
    std::vector<PluginReference> out;
    const std::string format = extensionFormat(path);

    if (format == "PEX") {
        auto scan = pex::scan(bytes);
        if (!scan.ok) {
            for (const auto& s : AssetScanner::pexStrings(bytes))
                if (auto it = names.find(toLower(s)); it != names.end()) {
                    auto r = make(Fact::PexUnreadable, it->second, path, format, s, scan.error);
                    out.push_back(std::move(r));
                }
        } else {
            for (size_t i = 0; i < scan.strings.size(); ++i) {
                auto it = names.find(toLower(scan.strings[i]));
                if (it == names.end())
                    continue;
                bool used = false;
                for (const auto& u : scan.uses) {
                    if (u.string != i)
                        continue;
                    used = true;
                    Fact f = u.use == pex::StringUse::GetFormFromFile
                                 ? (u.formIdLiteral ? Fact::PexGetFormFixed : Fact::PexGetFormRuntime)
                             : u.use == pex::StringUse::IsPluginInstalled ? Fact::PexPluginInstalled
                                                                          : Fact::PexOtherUse;
                    auto r = make(f, it->second, path, format, scan.strings[i], u.where);
                    if (f == Fact::PexOtherUse)
                        r.context = std::string(pex::stringUseName(u.use)) + (u.function.empty() ? "" : " (" + u.function + ")");
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
                if (format == "PSC")
                    f = Fact::PscSource;
                else if (commentLine(text, m.begin, format))
                    f = Fact::ConfigComment;
                else if (blueprint)
                    f = Fact::SettlementBlueprint;
                else if (m.pair)
                    f = Fact::ConfigPair;
                else if (mcm && inPluginRequirements(lower, m.begin))
                    f = Fact::McmRequirement;
                else
                    f = Fact::ConfigPlain;
                auto r = make(f, it->second, path, format,
                              std::string(text.substr(m.begin, (m.pair ? m.idEnd : m.end) - m.begin)),
                              "line " + std::to_string(lineOf(text, m.begin)));
                r.nameBegin = m.begin;
                r.nameEnd = m.end;
                if (m.pair) {
                    r.hasFormId = true;
                    r.formId = m.formId;
                    r.idBegin = m.idBegin;
                    r.idEnd = m.idEnd;
                }
                out.push_back(std::move(r));
            }
        }
    }
    for (auto& r : out)
        r.source = source;
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
            }
            begin = slash + 1;
        }
        const std::string file = p.substr(begin);
        for (const auto& n : AssetScanner::pluginNamesInText(file)) {
            auto it = names.find(n);
            if (it == names.end() || toLower(file) == n)
                continue;
            Fact f = istartsWith(p, "f4se\\plugins\\robco_patcher\\") ? Fact::ToolFileName : Fact::OtherFileName;
            auto r = make(f, it->second, toLower(p), "File name", file, {});
            r.source = idx.modName(idx.entries[i].winner());
            session.nameReferences.push_back(std::move(r));
        }
    }
    for (const auto& [key, value] : folders) {
        const auto& [plugin, folder] = key;
        const NamedFolder* engine = nullptr;
        for (const auto& nf : kEngineFolders)
            if (folder == std::string(nf.prefix) + session.plugins[plugin].key + "\\")
                engine = &nf;
        auto r = make(engine ? Fact::EngineFolder : Fact::OtherFolder, plugin, folder, "Folder",
                      session.plugins[plugin].key, std::to_string(value.first) + " files");
        if (engine) {
            r.context = engine->what;
            r.reason = std::string(engine->what) + " are found by the plugin's file name; moving them to the output "
                                                   "is not implemented";
        }
        r.source = value.second;
        session.nameReferences.push_back(std::move(r));
    }

    progress.stage("Scanning scripts and configs for plugin names");
    std::vector<std::vector<PluginReference>> found(candidates.size());
    std::atomic<size_t> scanned{0};
    parallelFor(
        candidates.size(),
        [&](size_t i) {
            const auto& e = idx.entries[candidates[i]];
            const auto& path = idx.path(e);
            try {
                auto bytes = readOriginal(session, e);
                found[i] = inspect(names, toLower(path), bytes, idx.modName(e.winner()));
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

    std::stable_sort(session.nameReferences.begin(), session.nameReferences.end(),
                     [](const PluginReference& a, const PluginReference& b) {
                         return std::tie(a.plugin, a.assetPath, a.nameBegin) < std::tie(b.plugin, b.assetPath, b.nameBegin);
                     });
    spdlog::info("{} references to plugin file names in {} scripts, configs and paths", session.nameReferences.size(),
                 candidates.size());
}

} // namespace fo4mx
