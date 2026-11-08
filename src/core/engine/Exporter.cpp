#include "engine/Exporter.hpp"

#include "engine/ValidationEngine.hpp"
#include "esp/PluginFile.hpp"
#include "esp/PluginWriter.hpp"
#include "util/FileTransaction.hpp"
#include "util/Zip.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <cstring>
#include <map>
#include <tuple>
#include <set>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text)
{
    atomicWriteText(path, text);
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

std::vector<std::string> Exporter::registeredArchives(const fs::path& outputDirectory)
{
    std::vector<std::string> out;
    for (const auto& a : readLines(outputDirectory / kRegisteredFile))
        out.push_back(toLower(a));
    return out;
}

std::vector<std::string> Exporter::archiveList(const Session& session, const CompiledOutput& c,
                                               const fs::path& outputDirectory)
{
    std::unordered_set<std::string> needed, stale;
    for (const auto& a : c.archivesToRegister)
        needed.insert(toLower(a));
    // Archives an earlier build registered: the record in the output folder and what the scan
    // gathered (the previous output may have been moved to a backup by this export).
    for (const auto& a : readLines(outputDirectory / kRegisteredFile))
        if (!needed.count(toLower(a)))
            stale.insert(toLower(a));
    for (const auto& a : session.registeredArchives)
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

void Exporter::planArchiveIni(FileTransaction& tx, const Session& session, const CompiledOutput& c,
                              const fs::path& outputDirectory)
{
    const auto registeredPath = outputDirectory / kRegisteredFile;
    auto previously = readLines(registeredPath);
    if (c.archivesToRegister.empty() && previously.empty() && session.registeredArchives.empty())
        return; // nothing was ever registered and nothing needs to be

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
    }
    // backupWhenMissing: an empty backup records that there was no Fallout4Custom.ini, so the
    // oldest backup is always the state before this app's first change (the scan's archive baseline).
    tx.write(custom, setIniValue(text, "Archive", "SResourceArchiveList2", join(archiveList(session, c, outputDirectory), ", ")),
             "the archive list (SResourceArchiveList2) the compiled mods need", true);

    std::string record = "# Archives Fallout 4 Multiplexer Renewed added to SResourceArchiveList2\n";
    for (const auto& a : c.archivesToRegister)
        record += a + "\n";
    fs::create_directories(outputDirectory);
    tx.write(registeredPath, record, "the archives this build added to the INI");
}

std::vector<fs::path> Exporter::applyArchiveIni(const Session& session, const CompiledOutput& c,
                                                const fs::path& outputDirectory)
{
    FileTransaction tx("Archive INI");
    planArchiveIni(tx, session, c, outputDirectory);
    tx.apply();
    std::vector<fs::path> written;
    for (const auto& f : tx.changedFiles())
        if (f.parent_path() != outputDirectory)
            written.push_back(f);
    return written;
}

// Where earlier versions wrote their single output folder (now only read for their build records; see
// output::locate for where exports go). One installable mod for every mod manager: "Multiplexer Output\Data\..." next to the game, outside
// Data and outside any mod manager's folders, zipped as "Multiplexer Output.zip" (Data\ at its root)
// for Vortex or Mod Organizer 2 to install.
fs::path Exporter::defaultOutputDirectory(const Session& session)
{
    fs::path game = session.install.gamePath;
    if (game.empty())
        game = session.install.dataPath.parent_path();
    return game / "Multiplexer Output" / "Data";
}

fs::path Exporter::packageRoot(const fs::path& outputDirectory)
{
    return iequals(pathToUtf8(outputDirectory.filename()), "Data") ? outputDirectory.parent_path() : outputDirectory;
}

std::string Exporter::proposedPluginsTxt(const Session& session, const CompiledOutput& c)
{
    std::string out = "# Generated by Fallout 4 Multiplexer Renewed\n";
    std::unordered_set<std::string> outputs;
    for (const auto& op : c.outputs)
        if (!op.records.empty())
            outputs.insert(toLower(op.name));
    for (const auto& p : session.plugins) {
        if (p.implicit || outputs.count(p.key)) // an output of an earlier build: listed once, below
            continue;
        bool active = p.active && (!c.isCompiled(p.index) || c.hasArchiveLoader(p.index));
        out += (active ? "*" : "") + p.name + "\n";
    }
    for (const auto& op : c.outputs)
        if (!op.records.empty())
            out += "*" + op.name + "\n";
    return out;
}

std::unordered_map<std::string, uint64_t> Exporter::archiveWinnerCopies(const std::filesystem::path& outputDir)
{
    std::unordered_map<std::string, uint64_t> out;
    std::ifstream in(outputDir / "Multiplexer_ArchiveWinners.tsv");
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        out[line.substr(0, tab)] = std::strtoull(line.c_str() + tab + 1, nullptr, 16);
    }
    return out;
}

ExportResult Exporter::write(const Session& session, const CompiledOutput& c, const ExportOptions& options,
                             const std::vector<std::string>& previousOutputs)
{
    ExportResult r;
    r.directory = options.outputDirectory.empty() ? defaultOutputDirectory(session) : options.outputDirectory;
    fs::create_directories(r.directory);
    // Notes for the user (lists, the INI line, reports) go next to Data, not into it, so the
    // package installs only what the game uses; the build's own records (FormID map, copy lists) stay
    // in Data, where the next scan finds them even once the package is installed.
    r.packageRoot = packageRoot(r.directory);
    const fs::path notes = r.packageRoot;

    // Every output is built in memory first: a record that cannot be written stops the export before
    // any file changes, instead of leaving some outputs of this build beside others of the last one.
    std::vector<std::pair<const OutputPlugin*, std::vector<uint8_t>>> images;
    for (const auto& op : c.outputs) {
        if (op.records.empty() && !options.writeEmptyOutputs)
            continue;
        PluginWriter w;
        w.setHeader(op.header);
        for (size_t i = 0; i < op.built.size(); ++i)
            w.addRecord(op.built[i], i < op.placements.size() ? op.placements[i] : Placement{});
        try {
            images.emplace_back(&op, w.build(options.compressLargeRecords));
        } catch (const std::exception& e) {
            throw std::runtime_error(op.name + " cannot be written (nothing was written): " + e.what());
        }
    }
    for (auto& [opPtr, image] : images) {
        const auto& op = *opPtr;
        auto path = r.directory / utf8ToPath(op.name);
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
    // SEQ files of the outputs (start-game-enabled quests); an output that no longer needs one loses it.
    for (const auto& op : c.outputs) {
        auto stem = op.name.substr(0, op.name.rfind('.'));
        auto seq = r.directory / "seq" / utf8ToPath(stem + ".seq");
        std::error_code ec;
        if (op.seqFormIds.empty() || (op.records.empty() && !options.writeEmptyOutputs)) {
            if (fs::exists(seq, ec))
                fs::remove(seq, ec);
            continue;
        }
        BinaryWriter w;
        for (uint32_t id : op.seqFormIds)
            w.write<uint32_t>(id);
        fs::create_directories(seq.parent_path(), ec);
        writeBinary(seq, w.buffer());
        r.seqFiles.push_back(seq);
        spdlog::info("Wrote {} ({} start-game-enabled quests)", pathToUtf8(seq.filename()), op.seqFormIds.size());
    }
    // Files the game finds by a record's defining plugin and FormID (FormID without its load order
    // index, 8 hex digits): a compiled new record is defined by its output with a new FormID, so its
    // files are copied there under the new name. Voice files (D3) for responses, FaceGen meshes and
    // face textures (N2) for NPCs. The FaceGen mesh keeps naming its textures by their original
    // path; those files stay installed with the original mod, and copies under the new name are
    // written as well.
    struct Relocation {
        const char* prefix;   // up to the plugin folder
        Signature sig;
        int folders;          // folders between the plugin folder and the file (voice type)
        const char* manifest;
        size_t* counter;
    };
    const Relocation relocations[] = {
        {"sound\\voice\\", Signature("INFO"), 1, "Multiplexer_Voices.tsv", &r.voiceFiles},
        {"meshes\\actors\\character\\facegendata\\facegeom\\", Signature("NPC_"), 0, "Multiplexer_FaceGen.tsv",
         &r.faceGenFiles},
        {"textures\\actors\\character\\facecustomization\\", Signature("NPC_"), 0, nullptr, &r.faceGenFiles},
    };
    std::map<std::string, std::set<std::string>> previousCopies, currentCopies; // manifest -> files
    std::map<std::string, std::string> manifests;
    for (const auto& rel : relocations)
        if (rel.manifest && !previousCopies.count(rel.manifest)) {
            std::ifstream in(r.directory / rel.manifest);
            auto& prev = previousCopies[rel.manifest];
            for (std::string line; std::getline(in, line);)
                if (!line.empty() && line[0] != '#')
                    prev.insert(line.substr(0, line.find('\t')));
            manifests[rel.manifest] = "# Fallout 4 Multiplexer Renewed relocated files v1\n#output_file\tsource_file\n";
        }
    {
        std::map<std::tuple<Signature, std::string, uint32_t>, const FormMapping*> moved; // (type, plugin, id)
        for (const auto& m : c.mappings)
            if (m.sig == Signature("INFO") || m.sig == Signature("NPC_"))
                moved[{m.sig, toLower(m.sourcePlugin), m.sourceObjectId}] = &m;
        const auto& idx = session.assets;
        const char* lastManifest = nullptr;
        for (const auto& rel : relocations) {
            const char* manifest = rel.manifest ? rel.manifest : lastManifest;
            lastManifest = manifest;
            if (moved.empty())
                continue;
            const size_t plen = std::strlen(rel.prefix);
            for (const auto& e : idx.entries) {
                const std::string& path = idx.path(e);
                if (!istartsWith(path, rel.prefix))
                    continue;
                // <prefix><plugin>\[<folder>\]<8 hex digits>(_|.)<rest>
                const size_t pluginEnd = path.find('\\', plen);
                if (pluginEnd == std::string::npos)
                    continue;
                size_t fileStart = pluginEnd + 1;
                bool ok = true;
                for (int f = 0; f < rel.folders && ok; ++f) {
                    const size_t next = path.find('\\', fileStart);
                    ok = next != std::string::npos;
                    fileStart = ok ? next + 1 : fileStart;
                }
                if (!ok || path.find('\\', fileStart) != std::string::npos || path.size() < fileStart + 9 ||
                    (path[fileStart + 8] != '_' && path[fileStart + 8] != '.'))
                    continue;
                const std::string hex = path.substr(fileStart, 8);
                if (!std::all_of(hex.begin(), hex.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)); }))
                    continue;
                auto it = moved.find({rel.sig, path.substr(plen, pluginEnd - plen),
                                      static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16))});
                if (it == moved.end())
                    continue;
                char id[16];
                std::snprintf(id, sizeof(id), "%08x", it->second->objectId);
                const std::string dest = std::string(rel.prefix) + toLower(it->second->outputPlugin) +
                                         path.substr(pluginEnd, fileStart - pluginEnd) + id + path.substr(fileStart + 8);
                std::string destFs = dest;
                std::replace(destFs.begin(), destFs.end(), '\\', '/');
                const auto target = r.directory / utf8ToPath(destFs);
                try {
                    auto bytes = idx.read(e, e.sources.size() - 1, session.vfs);
                    std::error_code ec;
                    fs::create_directories(target.parent_path(), ec);
                    if (!sameContent(target, bytes))
                        writeBinary(target, bytes);
                    ++*rel.counter;
                    currentCopies[manifest].insert(dest);
                    manifests[manifest] += dest + "\t" + path + "\n";
                } catch (const std::exception& ex) {
                    spdlog::error("Could not copy {}: {}", path, ex.what());
                }
            }
        }
    }
    for (const auto& [manifest, prev] : previousCopies) {
        const auto& now = currentCopies[manifest];
        for (const auto& old : prev)
            if (!now.count(old)) {
                std::string relFs = old;
                std::replace(relFs.begin(), relFs.end(), '\\', '/');
                std::error_code ec;
                fs::remove(r.directory / utf8ToPath(relFs), ec);
            }
        std::error_code ec;
        if (now.empty())
            fs::remove(r.directory / manifest, ec);
        else
            writeText(r.directory / manifest, manifests[manifest]);
    }
    if (r.voiceFiles)
        r.voiceManifest = r.directory / "Multiplexer_Voices.tsv";
    spdlog::info("Relocated files: {} voice files, {} FaceGen files", r.voiceFiles, r.faceGenFiles);
    // Archive winners: a compiled plugin's archive loads from the INI list after switching over, before
    // every plugin's own archives, so a file it serves today could be served by another archive. Each
    // such file is written loose into the output folder (a loose file wins over every archive), so the
    // game shows the same version.
    {
        const auto manifestPath = r.directory / "Multiplexer_ArchiveWinners.tsv";
        const auto previous = archiveWinnerCopies(r.directory);
        std::set<std::string> current;
        std::string manifest = "# Fallout 4 Multiplexer Renewed archive winners v1\n#file\tcontent_hash\tarchive\n";
        const auto& idx = session.assets;
        if (options.archiveWinnerCopies)
            for (const auto& [entry, slot] : ValidationEngine::archiveWinners(session, c)) {
                const auto& e = idx.entries[entry];
                const auto& path = idx.path(e);
                std::string relFs = path;
                std::replace(relFs.begin(), relFs.end(), '\\', '/');
                const auto dest = r.directory / utf8ToPath(relFs);
                try {
                    auto bytes = idx.read(e, slot, session.vfs);
                    std::error_code ec;
                    fs::create_directories(dest.parent_path(), ec);
                    if (!sameContent(dest, bytes))
                        writeBinary(dest, bytes);
                    char hash[24];
                    std::snprintf(hash, sizeof(hash), "%016llx",
                                  static_cast<unsigned long long>(fnv1a64(bytes.data(), bytes.size())));
                    manifest += path + "\t" + hash + "\t" + idx.archives[e.sources[slot].archive].name + "\n";
                    current.insert(path);
                    ++r.archiveWinnerFiles;
                } catch (const std::exception& ex) {
                    spdlog::error("Could not keep {} as a loose copy: {}", path, ex.what());
                }
            }
        for (const auto& [old, hash] : previous)
            if (!current.count(old)) {
                std::string relFs = old;
                std::replace(relFs.begin(), relFs.end(), '\\', '/');
                std::error_code ec;
                fs::remove(r.directory / utf8ToPath(relFs), ec);
            }
        std::error_code ec;
        if (current.empty()) {
            fs::remove(manifestPath, ec);
        } else {
            writeText(manifestPath, manifest);
            spdlog::info("Archive winners: {} files kept as loose copies", r.archiveWinnerFiles);
        }
    }
    // Outputs from the previous build that this build no longer produces.
    for (const auto& name : previousOutputs) {
        bool current = std::any_of(r.pluginFiles.begin(), r.pluginFiles.end(),
                                   [&](const fs::path& p) { return iequals(pathToUtf8(p.filename()), name); });
        auto path = r.directory / utf8ToPath(name);
        std::error_code ec;
        if (!current && fs::exists(path, ec)) {
            fs::remove(path, ec);
            fs::remove(r.directory / "seq" / utf8ToPath(name.substr(0, name.rfind('.')) + ".seq"), ec);
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
    r.archiveIni = notes / "Multiplexer_Archives.ini";
    std::vector<std::string> list = archiveList(session, c, r.directory);
    std::string ini = "; Generated by Fallout 4 Multiplexer Renewed\n"
                      "; The game does NOT read this file. It is a copy of the line \"Apply to Game\" writes into\n"
                      "; Fallout4Custom.ini (Documents\\My Games\\Fallout4, or the MO2 profile's INI), so the archives\n"
                      "; of plugins that were compiled and disabled keep loading. Apply to Game does this for you;\n"
                      "; to do it by hand, put this section into that file instead.\n"
                      "[Archive]\n"
                      "SResourceArchiveList2=" +
                      join(list, ", ") + "\n";
    writeText(r.archiveIni, ini);

    r.disableList = notes / "Multiplexer_DisablePlugins.txt";
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

    r.pluginsTxt = notes / "Multiplexer_plugins.txt";
    writeText(r.pluginsTxt, proposedPluginsTxt(session, c));

    // A stale summary from an earlier build (no longer written: the Compilation Report has it all).
    {
        std::error_code ec;
        fs::remove(notes / "Multiplexer_Compilation.json", ec);
        fs::remove(r.directory / "Multiplexer_Compilation.json", ec);
    }
    return r;
}

void Exporter::planProfile(FileTransaction& tx, const Session& session, const CompiledOutput& c,
                           const fs::path& pluginsTxt)
{
    tx.write(pluginsTxt, proposedPluginsTxt(session, c), "compiled plugins disabled, outputs enabled at the end");
    auto loadOrder = pluginsTxt.parent_path() / "loadorder.txt";
    if (fs::exists(loadOrder)) {
        std::string lo = "# Generated by Fallout 4 Multiplexer Renewed\n";
        for (const auto& p : session.plugins)
            lo += p.name + "\n";
        for (const auto& op : c.outputs)
            if (!op.records.empty())
                lo += op.name + "\n";
        tx.write(loadOrder, lo, "every plugin in its order, outputs at the end");
    }
}

std::vector<fs::path> Exporter::applyToProfile(const Session& session, const CompiledOutput& c, const fs::path& pluginsTxt)
{
    FileTransaction tx("Profile load order");
    planProfile(tx, session, c, pluginsTxt);
    tx.apply();
    return tx.changedFiles();
}

} // namespace fo4mx

namespace fo4mx {

std::filesystem::path Exporter::writePackageZip(ExportResult& r, const std::vector<fs::path>& neverPackage)
{
    // This app's own working files (database, build cache) are never packaged, wherever they are.
    std::vector<std::string> own;
    for (const auto& p : neverPackage)
        if (!p.empty())
            own.push_back(toLower(pathToUtf8(fs::absolute(p).lexically_normal().generic_u8string())));
    const fs::path base = fs::absolute(r.directory).lexically_normal();
    auto isOwn = [&](const fs::path& rel) {
        if (own.empty())
            return false;
        const std::string full = toLower(pathToUtf8((base / rel).lexically_normal().generic_u8string()));
        for (const auto& o : own)
            if (o == full)
                return true;
        return false;
    };
    const auto zip = r.packageRoot / "Multiplexer Output.zip";
    ZipStats stats;
    if (r.packageRoot != r.directory) {
        stats = writeZip(zip, r.directory, "Data/", isOwn);
    } else {
        // An output folder chosen by hand (not named Data) holds the notes and reports too: the zip
        // gets only what belongs in the game's Data folder, and never itself.
        static const char* const kNotes[] = {"multiplexer output.zip",          "multiplexer_archives.ini",
                                             "multiplexer_disableplugins.txt",  "multiplexer_plugins.txt",
                                             "multiplexer_instructions.txt",    "multiplexer_archivepriority.txt",
                                             "multiplexer_build_failed.txt",    "multiplexer_compilation.json",
                                             "multiplexer_run.txt"};
        static const char* const kFolders[] = {"multiplexer reports", "diagnostic recorder"};
        stats = writeZip(zip, r.directory, "Data/", [&](const fs::path& rel) {
            if (isOwn(rel))
                return true;
            const std::string first = toLower(pathToUtf8(*rel.begin()));
            for (const char* f : kFolders)
                if (first == f)
                    return true;
            if (first.rfind("multiplexer outputs backup ", 0) == 0)
                return true; // never package a backup
            if (std::distance(rel.begin(), rel.end()) == 1 && first == "meta.ini")
                return true; // Mod Organizer 2's description of the mod folder, not part of the export
            if (std::distance(rel.begin(), rel.end()) != 1)
                return false;
            const std::string name = toLower(pathToUtf8(rel.filename()));
            for (const char* n : kNotes)
                if (name == n)
                    return true;
            return iendsWith(name, ".zip") || iendsWith(name, ".multiplexer-tmp");
        });
    }
    r.zipPath = zip;
    spdlog::info("Wrote {} ({} files, {} bytes before compression)", pathToUtf8(zip), stats.files, stats.bytes);
    return zip;
}

} // namespace fo4mx
