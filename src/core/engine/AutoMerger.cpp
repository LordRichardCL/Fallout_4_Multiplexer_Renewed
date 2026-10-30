#include "engine/AutoMerger.hpp"

#include "papyrus/Pex.hpp"
#include "util/Binary.hpp"
#include "util/Platform.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* relinkKindName(RelinkKind k)
{
    switch (k) {
    case RelinkKind::Script: return "Script";
    case RelinkKind::McmConfig: return "MCM Config";
    case RelinkKind::PairConfig: return "Config";
    case RelinkKind::Unsupported: return "Not Relinkable";
    }
    return "";
}

namespace {

constexpr const char* kRelinkedList = "Multiplexer_Relinked.txt";

// A mention of a plugin file name in text, and the "|FormID" that may follow it.
struct Mention {
    size_t begin = 0, end = 0; // the plugin name
    bool pair = false;         // followed by |FormID
    size_t idBegin = 0, idEnd = 0;
    bool prefixed = false; // the FormID is written with 0x
    uint32_t formId = 0;
};

// Characters that end a plugin name in config text (names themselves may contain spaces).
bool delimiter(char c)
{
    return c == '"' || c == '\'' || c == '|' || c == '=' || c == ',' || c == ';' || c == ':' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '<' || c == '>' || c == '\t' || c == '\n' || c == '\r' || c == '\\' ||
           c == '/' || c == '~';
}

std::vector<Mention> mentions(std::string_view text, const std::string& lowerName)
{
    std::vector<Mention> out;
    const std::string lower = toLower(std::string(text));
    for (size_t at = lower.find(lowerName); at != std::string::npos; at = lower.find(lowerName, at + 1)) {
        const size_t end = at + lowerName.size();
        if (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            continue; // "My Mod.esp2"
        // The whole name up to the previous delimiter must be this plugin's: "Power Armor.esp" is not
        // a mention of "Armor.esp".
        size_t start = at;
        while (start > 0 && !delimiter(text[start - 1]))
            --start;
        while (start < at && text[start] == ' ')
            ++start;
        if (start != at)
            continue;
        Mention m{at, end};
        size_t p = end;
        if (p < text.size() && text[p] == '|') {
            size_t q = p + 1;
            bool prefixed = q + 1 < text.size() && text[q] == '0' && (text[q + 1] == 'x' || text[q + 1] == 'X');
            if (prefixed)
                q += 2;
            size_t digits = q;
            while (digits < text.size() && digits - q < 8 && std::isxdigit(static_cast<unsigned char>(text[digits])))
                ++digits;
            const bool longer = digits < text.size() && std::isxdigit(static_cast<unsigned char>(text[digits]));
            if (digits > q && !longer) {
                m.pair = true;
                m.prefixed = prefixed;
                m.idBegin = q;
                m.idEnd = digits;
                m.formId = static_cast<uint32_t>(std::stoul(std::string(text.substr(q, digits - q)), nullptr, 16));
            }
        }
        out.push_back(m);
    }
    return out;
}

std::vector<uint8_t> readAsset(const Session& session, const std::string& path)
{
    const auto* e = session.assets.find(path);
    if (!e || e->sources.empty())
        throw FormatError("not found in the load order's files");
    return session.assets.read(*e, e->sources.size() - 1, session.vfs);
}

bool isScript(const std::string& path)
{
    return iendsWith(path, ".pex");
}
bool isMcmConfig(const std::string& path)
{
    return istartsWith(path, "mcm\\config\\") && iendsWith(path, ".json");
}

// Where the records of a compiled plugin went: (plugin, object ID) -> (output, new object ID).
struct Relocation {
    std::unordered_map<uint64_t, std::pair<std::string, uint32_t>> byRecord;
    std::map<PluginIndex, std::string> primaryOutput; // output holding most of a plugin's records

    Relocation(const Session& session, const CompiledOutput& compiled)
    {
        std::map<PluginIndex, std::map<std::string, size_t>> perOutput;
        for (const auto& m : compiled.mappings) {
            if (!m.source.valid())
                continue;
            byRecord[(uint64_t(m.source.origin) << 32) | (m.sourceObjectId & 0xFFFFFF)] = {m.outputPlugin, m.objectId};
            perOutput[m.source.origin][m.outputPlugin]++;
        }
        for (const auto& [plugin, outputs] : perOutput) {
            auto best = std::max_element(outputs.begin(), outputs.end(),
                                         [](const auto& a, const auto& b) { return a.second < b.second; });
            primaryOutput[plugin] = best->first;
        }
        (void)session;
    }
    const std::pair<std::string, uint32_t>* find(const Session& session, PluginIndex plugin, uint32_t formId) const
    {
        const uint32_t mask = session.plugins[plugin].isLight() ? 0xFFF : 0xFFFFFF;
        auto it = byRecord.find((uint64_t(plugin) << 32) | (formId & mask));
        return it == byRecord.end() ? nullptr : &it->second;
    }
};

// Data-relative asset path ("scripts\\x.pex") as a filesystem path below `dir`.
std::filesystem::path below(const std::filesystem::path& dir, std::string relative)
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return dir / utf8ToPath(relative);
}

std::string hexLike(uint32_t value, size_t width)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%X", value);
    std::string s = buf;
    if (s.size() < width)
        s.insert(0, width - s.size(), '0');
    return s;
}

} // namespace

AutoMergeReport AutoMerger::analyse(const Session& session)
{
    AutoMergeReport report;
    report.enabled = true;
    std::map<std::string, std::vector<PluginIndex>> byFile;
    for (const auto& r : session.nameReferences)
        byFile[r.assetPath].push_back(r.plugin);

    for (auto& [path, plugins] : byFile) {
        std::sort(plugins.begin(), plugins.end());
        plugins.erase(std::unique(plugins.begin(), plugins.end()), plugins.end());
        RelinkFile f;
        f.path = path;
        f.plugins = plugins;
        if (const auto* e = session.assets.find(path); e && !e->sources.empty())
            f.mod = session.assets.modName(e->winner());

        if (iendsWith(path, ".dll")) {
            f.problem = "F4SE plugin (DLL): compiled code cannot be relinked";
        } else if (istartsWith(path, "f4se\\plugins\\transfersettlements\\")) {
            f.problem = "Transfer Settlements blueprint: saved settlement data, not relinked";
        } else {
            try {
                auto bytes = readAsset(session, path);
                if (isScript(path)) {
                    f.kind = RelinkKind::Script;
                    auto scan = pex::scan(bytes);
                    if (!scan.ok) {
                        f.problem = "script could not be read safely (" + scan.error + ")";
                    } else {
                        for (PluginIndex p : plugins) {
                            const std::string lower = session.plugins[p].key;
                            bool named = false;
                            for (const auto& s : scan.strings)
                                named |= toLower(s) == lower;
                            for (const auto& c : scan.getFormFromFile) {
                                if (c.fileLiteral && toLower(scan.strings[c.fileString]) == lower) {
                                    ++f.formCalls;
                                    if (!c.formIdLiteral)
                                        f.problem = "GetFormFromFile with a FormID computed at run time";
                                } else if (!c.fileLiteral && named) {
                                    f.problem = "the plugin name reaches GetFormFromFile through a variable";
                                }
                            }
                            for (size_t i = 0; i < scan.strings.size(); ++i)
                                if (toLower(scan.strings[i]) == lower && scan.usedElsewhere[i])
                                    ++f.mentions;
                        }
                    }
                } else {
                    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                    f.kind = isMcmConfig(path) ? RelinkKind::McmConfig : RelinkKind::PairConfig;
                    for (PluginIndex p : plugins)
                        for (const auto& m : mentions(text, session.plugins[p].key))
                            ++(m.pair ? f.pairs : f.mentions);
                    if (f.kind == RelinkKind::PairConfig && f.mentions)
                        f.problem = "names the plugin without a FormID (" + std::to_string(f.mentions) +
                                    "x): what the file does with the name is unknown";
                }
            } catch (const std::exception& e) {
                f.problem = std::string("could not be read: ") + e.what();
            }
        }
        if (!f.problem.empty())
            f.kind = RelinkKind::Unsupported;
        const size_t index = report.files.size();
        for (PluginIndex p : plugins)
            report.filesOf[p].push_back(index);
        report.files.push_back(std::move(f));
    }
    for (const auto& [plugin, files] : report.filesOf) {
        bool all = std::all_of(files.begin(), files.end(), [&](size_t i) { return report.files[i].problem.empty(); });
        if (all)
            report.relinkable.insert(plugin);
    }
    spdlog::info("Auto-Merger: {} files name {} plugins; {} plugins can be relinked", report.files.size(),
                 report.filesOf.size(), report.relinkable.size());
    return report;
}

void AutoMerger::relink(const Session& session, const CompiledOutput& compiled, const std::filesystem::path& outputDir,
                        AutoMergeReport& report)
{
    Relocation where(session, compiled);
    std::vector<std::string> writtenPaths;
    report.written.clear();
    report.errors.clear();

    for (auto& f : report.files) {
        f.written = false;
        f.changedCalls = f.changedPairs = f.renamed = 0;
        f.notes.clear();
        std::vector<PluginIndex> compiledHere;
        for (PluginIndex p : f.plugins)
            if (compiled.isCompiled(p))
                compiledHere.push_back(p);
        if (compiledHere.empty() || !f.problem.empty())
            continue;
        try {
            auto bytes = readAsset(session, f.path);
            std::vector<uint8_t> out;
            if (f.kind == RelinkKind::Script) {
                auto scan = pex::scan(bytes);
                if (!scan.ok)
                    throw FormatError(scan.error);
                auto strings = scan.strings;
                auto indexOf = [&](const std::string& s) {
                    for (size_t i = 0; i < strings.size(); ++i)
                        if (strings[i] == s)
                            return static_cast<uint16_t>(i);
                    strings.push_back(s);
                    return static_cast<uint16_t>(strings.size() - 1);
                };
                std::vector<pex::Patch> patches;
                for (PluginIndex p : compiledHere) {
                    const std::string lower = session.plugins[p].key;
                    // Each call gets its own file name: the record's output, or the original name when the
                    // record does not exist in the plugin (the game found nothing before either).
                    for (const auto& c : scan.getFormFromFile) {
                        if (!c.fileLiteral || toLower(scan.strings[c.fileString]) != lower)
                            continue;
                        uint16_t file;
                        if (auto to = where.find(session, p, static_cast<uint32_t>(c.formId))) {
                            file = indexOf(to->first);
                            pex::Patch id{c.formIdOffset, std::vector<uint8_t>(4)};
                            storeLE<uint32_t>(id.bytes.data(), to->second);
                            patches.push_back(std::move(id));
                            ++f.changedCalls;
                        } else {
                            file = indexOf(scan.strings[c.fileString] + std::string(1, '\0')); // placeholder, fixed below
                            f.notes.push_back("GetFormFromFile(" + formatFormId(static_cast<uint32_t>(c.formId)) +
                                              ") names no record of " + session.plugins[p].name + ": left as it was");
                        }
                        pex::Patch idx{c.fileStringOffset, std::vector<uint8_t>(2)};
                        storeLE<uint16_t>(idx.bytes.data(), file);
                        patches.push_back(std::move(idx));
                    }
                    // Every other mention of the name: the output holding most of the plugin's records.
                    auto primary = where.primaryOutput.find(p);
                    for (size_t i = 0; i < scan.strings.size(); ++i)
                        if (toLower(scan.strings[i]) == lower && primary != where.primaryOutput.end()) {
                            strings[i] = primary->second;
                            if (scan.usedElsewhere[i])
                                ++f.renamed;
                        }
                }
                // Placeholders keep the original name for calls whose record does not exist.
                for (size_t i = scan.strings.size(); i < strings.size(); ++i)
                    if (!strings[i].empty() && strings[i].back() == '\0')
                        strings[i].pop_back();
                out = pex::rewrite(bytes, scan, strings, patches);
                if (!pex::scan(out).ok)
                    throw FormatError("the relinked script did not read back");
            } else {
                std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                for (PluginIndex p : compiledHere) {
                    auto found = mentions(text, session.plugins[p].key);
                    auto primary = where.primaryOutput.find(p);
                    // Replace from the end so earlier offsets stay valid.
                    for (auto m = found.rbegin(); m != found.rend(); ++m) {
                        if (m->pair) {
                            auto to = where.find(session, p, m->formId);
                            if (!to) {
                                f.notes.push_back(text.substr(m->begin, m->idEnd - m->begin) +
                                                  " names no record of the plugin: left as it was");
                                continue;
                            }
                            text.replace(m->idBegin, m->idEnd - m->idBegin, hexLike(to->second, m->idEnd - m->idBegin));
                            text.replace(m->begin, m->end - m->begin, to->first);
                            ++f.changedPairs;
                        } else if (f.kind == RelinkKind::McmConfig && primary != where.primaryOutput.end()) {
                            text.replace(m->begin, m->end - m->begin, primary->second);
                            ++f.renamed;
                        }
                    }
                }
                out.assign(text.begin(), text.end());
            }
            auto target = below(outputDir, f.path);
            std::filesystem::create_directories(target.parent_path());
            std::ofstream(target, std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            f.written = true;
            writtenPaths.push_back(f.path);
            report.written.push_back(target);
        } catch (const std::exception& e) {
            report.errors.push_back(f.path + ": " + e.what());
            f.notes.push_back(std::string("not relinked: ") + e.what());
        }
    }

    // Remove relinked copies an earlier export wrote that this build no longer produces.
    const auto listPath = outputDir / kRelinkedList;
    if (std::ifstream in{listPath}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#' ||
                std::find(writtenPaths.begin(), writtenPaths.end(), line) != writtenPaths.end())
                continue;
            std::error_code ec;
            std::filesystem::remove(below(outputDir, line), ec);
        }
    }
    std::error_code ec;
    if (writtenPaths.empty()) {
        std::filesystem::remove(listPath, ec);
    } else {
        std::ofstream list(listPath, std::ios::binary | std::ios::trunc);
        list << "# Scripts and configs relinked by the Advanced Users Auto-Merger (they override the originals)\n";
        for (const auto& p : writtenPaths)
            list << p << '\n';
    }
    spdlog::info("Auto-Merger: relinked {} files ({} errors)", writtenPaths.size(), report.errors.size());
}

} // namespace fo4mx
