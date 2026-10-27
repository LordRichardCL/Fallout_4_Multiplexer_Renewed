#include "engine/Exporter.hpp"

#include "esp/PluginFile.hpp"
#include "esp/PluginWriter.hpp"
#include "util/Json.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write " + pathToUtf8(path));
    out << text;
}

bool sameContent(const fs::path& path, const std::vector<uint8_t>& image)
{
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::file_size(path, ec) != image.size())
        return false;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> current(image.size());
    in.read(reinterpret_cast<char*>(current.data()), static_cast<std::streamsize>(current.size()));
    return in.gcount() == static_cast<std::streamsize>(image.size()) && current == image;
}

void writeBinary(const fs::path& path, const std::vector<uint8_t>& image)
{
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + pathToUtf8(path));
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    fs::rename(tmp, path);
}

std::string timestamp()
{
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

} // namespace

namespace {

const char* kRegisteredFile = "Multiplexer_RegisteredArchives.txt";
const char* kLoadersFile = "Multiplexer_ArchiveLoaders.txt";

std::vector<std::string> readLines(const fs::path& path)
{
    std::vector<std::string> out;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        line = std::string(trim(line));
        if (!line.empty() && line[0] != '#')
            out.push_back(line);
    }
    return out;
}

} // namespace

std::vector<std::string> Exporter::archiveList(const Session& session, const CompiledOutput& c,
                                               const fs::path& outputDirectory)
{
    std::unordered_set<std::string> needed, stale;
    for (const auto& a : c.archivesToRegister)
        needed.insert(toLower(a));
    for (const auto& a : readLines(outputDirectory / kRegisteredFile))
        if (!needed.count(toLower(a)))
            stale.insert(toLower(a));

    std::vector<std::string> list;
    std::unordered_set<std::string> seen;
    for (auto& a : split(session.install.ini.get("Archive", "SResourceArchiveList2", "Fallout4 - Animations.ba2"), ','))
        if (!a.empty() && !stale.count(toLower(a)) && seen.insert(toLower(a)).second)
            list.push_back(a);
    for (const auto& a : c.archivesToRegister)
        if (seen.insert(toLower(a)).second)
            list.push_back(a);
    return list;
}

std::string Exporter::setIniValue(const std::string& text, const std::string& section, const std::string& key,
                                  const std::string& value)
{
    const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    const std::string entry = key + "=" + value;
    bool inSection = false, sectionSeen = false, done = false;
    size_t sectionEnd = 0;
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        std::string t(trim(lines[i]));
        if (!t.empty() && t.front() == '[') {
            if (inSection)
                break; // key not found inside the section
            inSection = iequals(trim(std::string_view(t).substr(1, t.find(']') == std::string::npos ? t.size() - 1 : t.find(']') - 1)), section);
            if (inSection) {
                sectionSeen = true;
                sectionEnd = i + 1;
            }
            continue;
        }
        if (!inSection)
            continue;
        if (!t.empty() && t[0] != ';' && t[0] != '#') {
            auto eq = t.find('=');
            if (eq != std::string::npos && iequals(trim(std::string_view(t).substr(0, eq)), key)) {
                lines[i] = entry;
                done = true;
                break;
            }
            sectionEnd = i + 1;
        } else if (!t.empty()) {
            sectionEnd = i + 1;
        }
    }
    if (!done) {
        if (sectionSeen) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), entry);
        } else {
            while (!lines.empty() && trim(lines.back()).empty())
                lines.pop_back();
            if (!lines.empty())
                lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(entry);
        }
    }
    bool endsWithNewline = text.empty() || text.back() == '\n';
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += eol;
        out += lines[i];
    }
    if (endsWithNewline)
        out += eol;
    return out;
}

std::vector<fs::path> Exporter::applyArchiveIni(const Session& session, const CompiledOutput& c,
                                                const fs::path& outputDirectory)
{
    std::vector<fs::path> written;
    const auto registeredPath = outputDirectory / kRegisteredFile;
    auto previously = readLines(registeredPath);
    if (c.archivesToRegister.empty() && previously.empty())
        return written; // nothing was ever registered and nothing needs to be

    fs::path dir = session.install.iniDirectory;
    if (dir.empty())
        throw std::runtime_error("the game's INI folder is unknown; set it in Settings");
    fs::create_directories(dir);
    fs::path custom = dir / "Fallout4Custom.ini";
    for (const auto& entry : fs::directory_iterator(dir))
        if (iequals(pathToUtf8(entry.path().filename()), "Fallout4Custom.ini"))
            custom = entry.path();

    std::string text;
    if (fs::exists(custom)) {
        std::ifstream in(custom, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        auto backup = custom;
        backup += ".multiplexer-backup-" + timestamp();
        fs::copy_file(custom, backup, fs::copy_options::overwrite_existing);
        spdlog::info("Backed up {} to {}", pathToUtf8(custom), pathToUtf8(backup));
    }
    std::string updated = setIniValue(text, "Archive", "SResourceArchiveList2", join(archiveList(session, c, outputDirectory), ", "));
    if (updated != text) {
        writeText(custom, updated);
        written.push_back(custom);
    }

    std::string record = "# Archives Fallout 4 Multiplexer Renewed added to SResourceArchiveList2\n";
    for (const auto& a : c.archivesToRegister)
        record += a + "\n";
    fs::create_directories(outputDirectory);
    writeText(registeredPath, record);
    return written;
}

fs::path Exporter::defaultOutputDirectory(const Session& session)
{
    if (session.install.config.kind == InstallKind::ModOrganizer2 && !session.install.modsPath.empty())
        return session.install.modsPath / "Fallout 4 Multiplexer Output";
    return session.install.dataPath;
}

std::string Exporter::proposedPluginsTxt(const Session& session, const CompiledOutput& c)
{
    std::string out = "# Generated by Fallout 4 Multiplexer Renewed\n";
    for (const auto& p : session.plugins) {
        if (p.implicit)
            continue;
        bool active = p.active && (!c.isCompiled(p.index) || c.hasArchiveLoader(p.index));
        out += (active ? "*" : "") + p.name + "\n";
    }
    for (const auto& op : c.outputs)
        if (!op.records.empty())
            out += "*" + op.name + "\n";
    return out;
}

ExportResult Exporter::write(const Session& session, const CompiledOutput& c, const ExportOptions& options,
                             const std::vector<std::string>& previousOutputs)
{
    ExportResult r;
    r.directory = options.outputDirectory.empty() ? defaultOutputDirectory(session) : options.outputDirectory;
    fs::create_directories(r.directory);

    for (const auto& op : c.outputs) {
        if (op.records.empty() && !options.writeEmptyOutputs)
            continue;
        PluginWriter w;
        w.setHeader(op.header);
        for (const auto& rec : op.built)
            w.addRecord(rec);
        auto path = r.directory / utf8ToPath(op.name);
        auto image = w.build(options.compressLargeRecords);
        r.pluginFiles.push_back(path);
        if (sameContent(path, image)) {
            r.unchangedOutputs.push_back(op.name);
            spdlog::info("{} is unchanged", op.name);
            continue;
        }
        writeBinary(path, image);
        r.writtenOutputs.push_back(op.name);
        spdlog::info("Wrote {} ({} records, {} masters)", op.name, op.built.size(), op.masterNames.size());
    }
    // Outputs from the previous build that this build no longer produces.
    for (const auto& name : previousOutputs) {
        bool current = std::any_of(r.pluginFiles.begin(), r.pluginFiles.end(),
                                   [&](const fs::path& p) { return iequals(pathToUtf8(p.filename()), name); });
        auto path = r.directory / utf8ToPath(name);
        std::error_code ec;
        if (!current && fs::exists(path, ec)) {
            fs::remove(path, ec);
            r.removedOutputs.push_back(name);
            spdlog::info("Removed {} (no longer produced)", name);
        }
    }

    // FormID map (also the input for stable FormIDs next time).
    r.formMap = r.directory / "Multiplexer_FormMap.tsv";
    std::string map = "# Fallout 4 Multiplexer Renewed FormID map v1\n"
                      "#source_plugin\tsource_object_id\toutput_plugin\toutput_object_id\tsignature\teditor_id\n";
    for (const auto& m : c.mappings)
        map += m.sourcePlugin + "\t" + formatFormId(m.sourceObjectId).substr(2) + "\t" + m.outputPlugin + "\t" +
               formatFormId(m.objectId).substr(2) + "\t" + m.sig.str() + "\t" + m.editorId + "\n";
    writeText(r.formMap, map);

    // Archive loaders: an empty ESL plugin with the compiled plugin's name, so the game keeps
    // loading that plugin's archives at exactly the same priority. The records are in the outputs.
    {
        std::unordered_set<std::string> current;
        for (auto pi : c.archiveLoaders) {
            const auto& p = session.plugins[pi];
            PluginHeader h;
            h.record.signature = sig::TES4;
            h.record.flags = RecordFlags::Light | (p.header.record.flags & RecordFlags::Master);
            h.record.formVersion = 131;
            h.version = 1.0f;
            h.author = kGeneratorAuthor;
            h.description = std::string(kArchiveLoaderTag) + ": keeps the archives of " + p.name +
                            " loading at the same priority. Its records are in the Fallout 4 Multiplexer output plugins.";
            PluginWriter w;
            w.setHeader(h);
            auto path = r.directory / utf8ToPath(p.name);
            auto image = w.build(false);
            if (!sameContent(path, image))
                writeBinary(path, image);
            r.archiveLoaders.push_back(path);
            current.insert(toLower(p.name));
        }
        // Loaders a previous build wrote that are no longer needed (only files this app wrote).
        for (const auto& name : readLines(r.directory / kLoadersFile)) {
            if (current.count(toLower(name)))
                continue;
            auto path = r.directory / utf8ToPath(name);
            std::error_code ec;
            if (!fs::exists(path, ec))
                continue;
            try {
                if (readPluginHeader(path).author == kGeneratorAuthor) {
                    fs::remove(path, ec);
                    spdlog::info("Removed archive loader {} (no longer needed)", name);
                }
            } catch (const std::exception&) {
            }
        }
        std::string list = "# Archive loaders written by Fallout 4 Multiplexer Renewed (empty ESL plugins)\n";
        for (auto pi : c.archiveLoaders)
            list += session.plugins[pi].name + "\n";
        writeText(r.directory / kLoadersFile, list);
    }

    // Archives of compiled plugins without a loader no longer load with their plugin.
    r.archiveIni = r.directory / "Multiplexer_Archives.ini";
    std::vector<std::string> list = archiveList(session, c, r.directory);
    std::string ini = "; Generated by Fallout 4 Multiplexer Renewed\n"
                      "; Archives below belonged to plugins that were compiled and disabled.\n"
                      "; Merge this section into Fallout4Custom.ini (or your MO2 profile's INI) so they keep loading.\n"
                      "[Archive]\n"
                      "SResourceArchiveList2=" +
                      join(list, ", ") + "\n";
    writeText(r.archiveIni, ini);

    r.disableList = r.directory / "Multiplexer_DisablePlugins.txt";
    std::string disable = "# Plugins compiled into the output; disable these\n";
    for (auto pi : c.compiledPlugins)
        if (!c.hasArchiveLoader(pi))
            disable += session.plugins[pi].name + "\n";
    if (!c.archiveLoaders.empty()) {
        disable += "# Keep these enabled: the output mod replaces them with archive loaders\n";
        for (auto pi : c.archiveLoaders)
            disable += "# " + session.plugins[pi].name + "\n";
    }
    writeText(r.disableList, disable);

    r.pluginsTxt = r.directory / "Multiplexer_plugins.txt";
    writeText(r.pluginsTxt, proposedPluginsTxt(session, c));

    json::Value summary;
    summary.set("generator", "Fallout 4 Multiplexer Renewed");
    summary.set("runtime", session.defs.profileName());
    summary.set("compiledPlugins", static_cast<uint64_t>(c.compiledPlugins.size()));
    summary.set("records", static_cast<uint64_t>(c.records.size()));
    summary.set("newRecords", static_cast<uint64_t>(c.mappings.size()));
    summary.set("mergedRecords", static_cast<uint64_t>(c.mergedRecords));
    summary.set("patchedRecords", static_cast<uint64_t>(c.patchedRecords));
    summary.set("strategy", allocationStrategyName(c.options.strategy));
    json::Value outputs;
    for (const auto& op : c.outputs) {
        if (op.records.empty())
            continue;
        json::Value o;
        o.set("name", op.name);
        o.set("records", static_cast<uint64_t>(op.records.size()));
        o.set("newRecords", op.newRecords);
        o.set("overrides", op.overrides);
        o.set("light", op.light);
        o.set("patch", op.patch);
        json::Value masters;
        for (const auto& m : op.masterNames)
            masters.push(m);
        o.set("masters", masters);
        outputs.push(o);
    }
    summary.set("outputs", outputs);
    r.summary = r.directory / "Multiplexer_Compilation.json";
    writeText(r.summary, summary.dump());
    return r;
}

std::vector<fs::path> Exporter::applyToProfile(const Session& session, const CompiledOutput& c, const fs::path& pluginsTxt)
{
    std::vector<fs::path> written;
    std::string stamp = timestamp();
    auto backup = [&](const fs::path& p) {
        if (fs::exists(p)) {
            auto b = p;
            b += ".multiplexer-backup-" + stamp;
            fs::copy_file(p, b, fs::copy_options::overwrite_existing);
            spdlog::info("Backed up {} to {}", pathToUtf8(p), pathToUtf8(b));
        }
    };
    backup(pluginsTxt);
    writeText(pluginsTxt, proposedPluginsTxt(session, c));
    written.push_back(pluginsTxt);

    auto loadOrder = pluginsTxt.parent_path() / "loadorder.txt";
    if (fs::exists(loadOrder)) {
        backup(loadOrder);
        std::string lo = "# Generated by Fallout 4 Multiplexer Renewed\n";
        for (const auto& p : session.plugins)
            lo += p.name + "\n";
        for (const auto& op : c.outputs)
            if (!op.records.empty())
                lo += op.name + "\n";
        writeText(loadOrder, lo);
        written.push_back(loadOrder);
    }
    return written;
}

} // namespace fo4mx
