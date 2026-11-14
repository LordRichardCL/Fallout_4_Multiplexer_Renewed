#include "engine/EarlierBuilds.hpp"

#include "engine/Exporter.hpp"
#include "engine/Session.hpp"
#include "util/Text.hpp"

#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx::earlier {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line(trim(std::string_view(text).substr(at, end - at)));
        if (!line.empty() && line[0] != '#')
            out.push_back(std::move(line));
        at = end + 1;
    }
    return out;
}

bool isPluginName(const std::string& name)
{
    return iendsWith(name, ".esp") || iendsWith(name, ".esm") || iendsWith(name, ".esl");
}

// An export written before Multiplexer_CompiledPlugins.txt existed: its notes and its Data folder.
std::optional<BuildRecord> legacyRecord(const fs::path& dataDir)
{
    const fs::path notes = Exporter::packageRoot(dataDir);
    const fs::path disable = notes / "Multiplexer_DisablePlugins.txt";
    std::error_code ec;
    if (!fs::is_regular_file(disable, ec))
        return std::nullopt;
    BuildRecord r;
    r.source = pathToUtf8(disable);
    r.compiled = lines(slurp(disable));
    std::unordered_set<std::string> loaders;
    for (const auto& l : lines(slurp(dataDir / "Multiplexer_ArchiveLoaders.txt")))
        loaders.insert(toLower(l));
    for (const auto& e : fs::directory_iterator(dataDir, ec)) {
        if (!e.is_regular_file(ec))
            continue;
        const std::string name = pathToUtf8(e.path().filename());
        if (isPluginName(name) && !loaders.count(toLower(name)))
            r.outputs.push_back(name);
    }
    if (r.outputs.empty())
        return std::nullopt;
    return r;
}

std::optional<BuildRecord> recordIn(const fs::path& dataDir)
{
    if (auto r = readCompiledList(dataDir / kCompiledListFile))
        return r;
    return legacyRecord(dataDir);
}

} // namespace

std::string formatCompiledList(const std::vector<std::string>& outputs, const std::vector<std::string>& compiled)
{
    std::string t = "# Fallout 4 Multiplexer Renewed: the plugins this build compiled into its outputs.\n"
                    "# The next build reads them as sources while they are switched off, so they are never\n"
                    "# left out because an earlier build disabled them. Do not edit.\n";
    for (const auto& o : outputs)
        t += "output: " + o + "\n";
    for (const auto& c : compiled)
        t += c + "\n";
    return t;
}

std::optional<BuildRecord> parseCompiledList(const std::string& text, std::string source)
{
    BuildRecord r;
    r.source = std::move(source);
    for (const auto& l : lines(text)) {
        if (l.size() > 7 && iequals(l.substr(0, 7), "output:"))
            r.outputs.emplace_back(trim(std::string_view(l).substr(7)));
        else
            r.compiled.push_back(l);
    }
    if (r.outputs.empty())
        return std::nullopt;
    return r;
}

std::optional<BuildRecord> readCompiledList(const fs::path& file)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec))
        return std::nullopt;
    return parseCompiledList(slurp(file), pathToUtf8(file));
}

std::vector<BuildRecord> recordsFromExports(const output::Location& location)
{
    std::vector<BuildRecord> out;
    if (location.root.empty())
        return out;
    if (location.layout == output::Layout::GameData) {
        if (auto r = recordIn(location.root))
            out.push_back(std::move(*r));
        return out;
    }
    for (const auto& run : output::finishedRuns(location))
        if (auto r = recordIn(output::dataDir(run, location.layout)))
            out.push_back(std::move(*r));
    return out;
}

void apply(Session& session, const std::vector<BuildRecord>& given)
{
    session.earlierBuilds.clear();
    // The list installed with the outputs (the game's Data folder, or an enabled mod) comes first: it
    // is the one that belongs to the plugin files the game sees.
    std::vector<BuildRecord> records;
    if (const auto* f = session.vfs.find(toLower(kCompiledListFile)))
        if (auto r = readCompiledList(session.vfs.winnerPath(*f)))
            records.push_back(std::move(*r));
    records.insert(records.end(), given.begin(), given.end());

    std::unordered_map<std::string, bool> listed; // lowercase -> active in plugins.txt
    for (const auto& e : session.install.plugins)
        listed[toLower(e.name)] = e.active;

    for (const auto& p : session.plugins) {
        if (!p.generated || p.header.description.rfind(kArchiveLoaderTag, 0) == 0)
            continue;
        EarlierBuild b;
        b.output = p.name;
        auto it = listed.find(p.key);
        b.active = it != listed.end() && it->second;
        const BuildRecord* match = nullptr;
        for (const auto& r : records) {
            for (const auto& o : r.outputs)
                if (iequals(o, p.name)) {
                    match = &r;
                    break;
                }
            if (match)
                break;
        }
        if (match) {
            b.source = match->source;
            for (const auto& name : match->compiled) {
                const std::string key = toLower(name);
                session.reactivate.insert(key);
                const auto* q = session.findPlugin(key);
                // What the scanner reads as a source again: installed, readable, switched off.
                if (q && !q->active && !q->generated && !q->implicit && q->error.empty())
                    b.switchedOff.push_back(q->name);
            }
        }
        spdlog::info("{} is the output of an earlier build ({}): {}", p.name, b.active ? "enabled" : "switched off",
                     match ? std::to_string(b.switchedOff.size()) + " of the plugins it compiled are switched off and "
                                                                    "read as sources (" + b.source + ")"
                           : std::string("no record of the plugins it compiled was found"));
        session.earlierBuilds.push_back(std::move(b));
    }
}

} // namespace fo4mx::earlier
