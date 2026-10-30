#include "assets/AssetScanner.hpp"

#include "engine/ParallelFor.hpp"
#include "engine/ReferenceScanner.hpp"
#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <tuple>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

std::vector<std::string> AssetScanner::pluginArchiveNames(const PluginInfo& plugin, const std::string& language)
{
    std::string base = plugin.baseName();
    return {base + " - Main.ba2", base + " - Textures.ba2", base + " - Voices_" + language + ".ba2"};
}

std::vector<std::string> AssetScanner::pexStrings(const std::vector<uint8_t>& pex)
{
    std::vector<std::string> out;
    try {
        BinaryReader r(pex);
        if (r.read<uint32_t>() != 0xFA57C0DEu)
            return out; // not a little-endian (Fallout 4) script
        r.skip(2 + 2 + 8); // version, game id, compile time
        r.readWString();   // source
        r.readWString();   // user
        r.readWString();   // machine
        uint16_t count = r.read<uint16_t>();
        out.reserve(count);
        for (uint16_t i = 0; i < count; ++i)
            out.push_back(r.readWString());
    } catch (const FormatError&) {
        // truncated read: keep what we have
    }
    return out;
}

std::vector<std::string> AssetScanner::pluginNamesInText(std::string_view text)
{
    std::vector<std::string> out;
    std::string lower = toLower(text);
    for (const char* ext : {".esp", ".esm", ".esl"}) {
        size_t pos = 0;
        while ((pos = lower.find(ext, pos)) != std::string::npos) {
            size_t end = pos + 4;
            bool boundary = end >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[end]));
            if (boundary) {
                size_t start = pos;
                while (start > 0) {
                    char c = lower[start - 1];
                    if (c == '"' || c == '\'' || c == '=' || c == '|' || c == ',' || c == ':' || c == '\n' ||
                        c == '\r' || c == '\t' || c == '>' || c == '<' || c == '/' || c == '\\' || c == '~' ||
                        c == '[' || c == ']' || c == '{' || c == '}' || c == ';' || c == '(')
                        break;
                    --start;
                }
                std::string name = trim(std::string_view(lower).substr(start, end - start));
                if (name.size() > 4)
                    out.push_back(name);
            }
            pos = end;
        }
    }
    return out;
}

void AssetScanner::scan(Session& session, const AssetScanOptions& options, ProgressSink& progress)
{
    progress.stage("Scanning assets");
    auto& idx = session.assets;
    idx.clear();
    const auto& vfs = session.vfs;
    const std::string language = session.install.config.language.empty() ? "en" : session.install.config.language;

    auto modOf = [&](const VirtualFileSystem::File& f) {
        auto it = session.install.vortexOwners.find(normalizeAssetPath(f.relativePath));
        if (it != session.install.vortexOwners.end())
            return it->second;
        return vfs.winnerName(f);
    };

    // 1. Archive load order: INI lists, then each active plugin's archives.
    std::unordered_set<std::string> queued;
    std::vector<std::pair<const VirtualFileSystem::File*, PluginIndex>> archiveFiles;
    auto queue = [&](const std::string& name, PluginIndex owner) {
        auto key = toLower(name);
        if (!queued.insert(key).second)
            return;
        if (auto f = vfs.find(key))
            archiveFiles.emplace_back(f, owner);
        else if (owner == kInvalidPlugin)
            spdlog::debug("INI archive {} not found", name);
    };
    for (const auto& a : session.install.iniArchives())
        queue(a, kInvalidPlugin);
    for (auto pi : session.loadOrder)
        for (const auto& a : pluginArchiveNames(session.plugins[pi], language))
            queue(a, pi);

    // Record every "<plugin> - *.ba2" next to each plugin (loaded or not).
    for (auto& p : session.plugins)
        p.archives.clear();
    for (const auto* f : vfs.rootFiles()) {
        if (!iendsWith(f->relativePath, ".ba2"))
            continue;
        auto sep = f->relativePath.find(" - ");
        if (sep == std::string::npos)
            continue;
        std::string base = f->relativePath.substr(0, sep);
        for (const char* ext : {".esp", ".esm", ".esl"})
            if (auto p = session.findPlugin(base + ext))
                p->archives.push_back(f->relativePath);
    }

    idx.archives.resize(archiveFiles.size());
    std::atomic<size_t> opened{0};
    parallelFor(
        archiveFiles.size(),
        [&](size_t i) {
            auto& info = idx.archives[i];
            const auto* f = archiveFiles[i].first;
            info.name = f->relativePath;
            info.modName = modOf(*f);
            info.owner = archiveFiles[i].second;
            info.loadSequence = static_cast<uint32_t>(i);
            try {
                info.archive = Ba2Archive::open(vfs.winnerPath(*f));
            } catch (const std::exception& e) {
                spdlog::error("Cannot read archive {}: {}", info.name, e.what());
            }
            progress.progress(++opened, archiveFiles.size(), info.name);
        },
        progress, options.threads);

    // 2. Archive contents in load order (later archives win).
    for (uint32_t a = 0; a < idx.archives.size(); ++a) {
        const auto& info = idx.archives[a];
        if (!info.archive)
            continue;
        uint32_t mod = idx.strings.intern(info.modName);
        const auto& entries = info.archive->entries();
        for (uint32_t e = 0; e < entries.size(); ++e) {
            AssetSource s;
            s.archive = a;
            s.entry = e;
            s.mod = mod;
            s.priority = a;
            s.size = entries[e].size();
            idx.getOrAdd(entries[e].path).sources.push_back(s);
        }
    }

    // 3. Loose files override every archive.
    const uint32_t looseBase = 1'000'000;
    for (uint32_t fi = 0; fi < vfs.files().size(); ++fi) {
        const auto& f = vfs.files()[fi];
        if (f.relativePath.find('\\') == std::string::npos)
            continue; // Data root: plugins, archives, ini files
        std::string key = normalizeAssetPath(f.relativePath);
        if (istartsWith(key, "fomod\\"))
            continue;
        auto& entry = idx.getOrAdd(key);
        for (uint32_t slot = 0; slot < f.sources.size(); ++slot) {
            AssetSource s;
            s.archive = kLooseFile;
            s.entry = fi;
            s.slot = slot;
            uint32_t root = f.sources[slot];
            auto vit = session.install.vortexOwners.find(key);
            s.mod = idx.strings.intern(vit != session.install.vortexOwners.end() ? vit->second : vfs.roots()[root].name);
            s.priority = looseBase + root;
            s.size = f.sizes[slot];
            entry.sources.push_back(s);
        }
    }
    for (auto& e : idx.entries)
        std::stable_sort(e.sources.begin(), e.sources.end(),
                         [](const AssetSource& a, const AssetSource& b) { return a.priority < b.priority; });

    spdlog::info("Assets: {} unique files, {} archives, {} loose files, {} conflicting", idx.entries.size(),
                 idx.archives.size(), idx.looseFileCount(), idx.conflictCount());

    // 4. Files that refer to plugins by name, classified by what they are and how they use the name.
    ReferenceScanOptions refs;
    refs.scripts = options.scanScriptsForPluginNames;
    refs.configs = options.scanConfigsForPluginNames;
    refs.threads = options.threads;
    ReferenceScanner::scan(session, refs, progress);
    session.assetsScanned = true;
}

} // namespace fo4mx
