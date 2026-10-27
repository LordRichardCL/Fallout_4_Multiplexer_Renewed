#include "engine/PluginScanner.hpp"

#include "engine/BuildCache.hpp"
#include "engine/ParallelFor.hpp"
#include "esp/PluginFile.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

bool isPluginFile(std::string_view name)
{
    return iendsWith(name, ".esp") || iendsWith(name, ".esm") || iendsWith(name, ".esl");
}

PluginKind classify(const PluginInfo& p)
{
    bool light = p.header.isLightFlagged();
    if (iendsWith(p.name, ".esl"))
        return PluginKind::ESL;
    if (iendsWith(p.name, ".esm"))
        return light ? PluginKind::LightESM : PluginKind::ESM;
    return light ? PluginKind::LightESP : PluginKind::ESP;
}

// Cheap EDID extraction: uncompressed records almost always start with EDID.
std::string quickEditorId(const RecordLocation& loc, bool allowDecompress)
{
    try {
        if (loc.header.compressed()) {
            if (!allowDecompress)
                return {};
            auto data = Record::decompress(loc.header, loc.rawData);
            if (data.size() >= 6 && loadLE<uint32_t>(data.data()) == sig::EDID.value) {
                uint16_t len = loadLE<uint16_t>(data.data() + 4);
                if (6u + len <= data.size()) {
                    BinaryReader r(data.data() + 6, len);
                    return r.readZString();
                }
            }
            return {};
        }
        const auto& d = loc.rawData;
        if (d.size() >= 6 && loadLE<uint32_t>(d.data()) == sig::EDID.value) {
            uint16_t len = loadLE<uint16_t>(d.data() + 4);
            if (6u + len <= d.size()) {
                BinaryReader r(d.data() + 6, len);
                return r.readZString();
            }
        }
    } catch (const std::exception&) {
    }
    return {};
}

} // namespace

void PluginScanner::discoverPlugins(Session& session, ProgressSink& progress)
{
    progress.stage("Discovering plugins");
    session.plugins.clear();
    session.loadOrder.clear();

    // Plugin files visible in the virtual Data folder.
    std::unordered_map<std::string, const VirtualFileSystem::File*> files;
    for (const auto* f : session.vfs.rootFiles())
        if (isPluginFile(f->relativePath))
            files[toLower(f->relativePath)] = f;

    std::unordered_set<std::string> ccSet;
    for (const auto& c : session.install.creationClub)
        ccSet.insert(toLower(c));

    std::vector<std::string> order;
    std::unordered_map<std::string, PluginListEntry> entries;
    for (const auto& e : session.install.plugins) {
        auto key = toLower(e.name);
        if (!files.count(key)) {
            if (e.active && !e.implicit)
                spdlog::warn("{} is enabled in the load order but not present in Data", e.name);
            continue;
        }
        if (entries.emplace(key, e).second)
            order.push_back(key);
    }
    // Plugins present in Data but not listed anywhere are inactive.
    std::vector<std::string> unlisted;
    for (const auto& [key, f] : files)
        if (!entries.count(key))
            unlisted.push_back(key);
    std::sort(unlisted.begin(), unlisted.end());
    for (const auto& key : unlisted) {
        entries[key] = PluginListEntry{files[key]->relativePath, false, false};
        order.push_back(key);
    }

    std::vector<const VirtualFileSystem::File*> fileOf;
    for (const auto& key : order) {
        const auto* f = files[key];
        fileOf.push_back(f);
        PluginInfo info;
        info.name = f->relativePath;
        info.path = session.vfs.winnerPath(*f);
        info.sourceMod = session.vfs.winnerName(*f);
        info.fileSize = f->sizes.back();
        info.modified = f->mtimes.back();
        info.active = entries[key].active;
        info.implicit = entries[key].implicit;
        info.baseGame = isBaseGamePlugin(info.name);
        info.creationClub = ccSet.count(key) != 0;
        auto vit = session.install.vortexOwners.find(key);
        if (vit != session.install.vortexOwners.end())
            info.sourceMod = vit->second;
        session.addPlugin(std::move(info));
    }

    // Headers (parallel, they are small).
    parallelFor(session.plugins.size(), [&](size_t i) {
        auto& p = session.plugins[i];
        try {
            p.header = readPluginHeader(p.path);
            p.kind = classify(p);
        } catch (const std::exception& e) {
            p.error = e.what();
            p.active = false;
        }
    });

    // This app's own files: Dummy plugins and patches are outputs, never sources. An archive
    // loader stands in for a compiled plugin, so the original underneath it is read instead.
    for (auto& p : session.plugins) {
        if (!p.error.empty() || p.header.author != kGeneratorAuthor)
            continue;
        const auto* f = fileOf[p.index];
        if (p.header.description.rfind(kArchiveLoaderTag, 0) == 0 && f->sources.size() > 1) {
            size_t slot = f->sources.size() - 2;
            p.path = session.vfs.physicalPath(*f, slot);
            p.sourceMod = session.vfs.roots()[f->sources[slot]].name;
            p.fileSize = f->sizes[slot];
            p.modified = f->mtimes[slot];
            p.behindLoader = true;
            try {
                p.header = readPluginHeader(p.path);
                p.kind = classify(p);
            } catch (const std::exception& e) {
                p.error = e.what();
                p.active = false;
            }
            continue;
        }
        p.generated = true;
        p.active = false;
    }
    // Plugins the last build compiled are still sources, in their recorded position.
    for (auto& p : session.plugins)
        if (!p.active && !p.generated && p.error.empty() && session.reactivate.count(p.key)) {
            p.active = true;
            p.reactivated = true;
        }

    // Final load order: implicit plugins first, then masters, then the rest,
    // each group keeping the recorded order (this is what the engine does).
    std::vector<PluginIndex> implicitList, masters, others;
    for (auto& p : session.plugins) {
        if (!p.active)
            continue;
        if (p.implicit)
            implicitList.push_back(p.index);
        else if (p.isMaster())
            masters.push_back(p.index);
        else
            others.push_back(p.index);
    }
    for (auto* list : {&implicitList, &masters, &others})
        session.loadOrder.insert(session.loadOrder.end(), list->begin(), list->end());

    int fullIndex = 0, lightIndex = 0;
    for (size_t i = 0; i < session.loadOrder.size(); ++i) {
        auto& p = session.plugins[session.loadOrder[i]];
        p.loadOrder = static_cast<int32_t>(i);
        char buf[16];
        if (p.isLight())
            std::snprintf(buf, sizeof(buf), "FE %03X", lightIndex++);
        else
            std::snprintf(buf, sizeof(buf), "%02X", fullIndex++);
        p.loadOrderHex = buf;
    }
    if (fullIndex > 254)
        spdlog::error("Load order has {} full plugins; Fallout 4 supports at most 254", fullIndex);
    if (lightIndex > 4096)
        spdlog::error("Load order has {} light plugins; Fallout 4 supports at most 4096", lightIndex);

    // Masters.
    for (auto& p : session.plugins) {
        p.masters.clear();
        p.missingMasters.clear();
        for (const auto& m : p.header.masters) {
            const PluginInfo* mp = session.findPlugin(m.name);
            if (!mp) {
                p.masters.push_back(kInvalidPlugin);
                p.missingMasters.push_back(m.name);
                continue;
            }
            p.masters.push_back(mp->index);
            if (p.active && (!mp->active || mp->loadOrder > p.loadOrder))
                p.missingMasters.push_back(m.name + (mp->active ? " (loads after this plugin)" : " (not active)"));
        }
    }
    spdlog::info("Discovered {} plugins ({} active)", session.plugins.size(), session.loadOrder.size());
}

void PluginScanner::indexRecords(Session& session, const ScanOptions& options, ProgressSink& progress)
{
    progress.stage("Scanning plugins");
    const auto& defs = session.defs;
    std::vector<std::vector<RecordInstance>> perPlugin(session.loadOrder.size());
    struct PendingCache {
        bool store = false;
        uint64_t sample = 0;
        CachedPluginIndex index;
    };
    std::vector<PendingCache> pending(session.loadOrder.size());
    BuildCache* cache = session.cache;
    std::atomic<size_t> done{0};

    parallelFor(
        session.loadOrder.size(),
        [&](size_t pos) {
            auto& p = session.plugins[session.loadOrder[pos]];
            auto& out = perPlugin[pos];
            try {
                // Unchanged plugin: restore its record index instead of parsing it.
                uint64_t sample = 0;
                if (cache && p.modified != 0) {
                    sample = sampleFileHash(p.path, p.fileSize);
                    if (auto cached = cache->loadPluginIndex(pathToUtf8(p.path), p.fileSize, p.modified, sample)) {
                        out.reserve(cached->records.size());
                        for (const auto& c : cached->records) {
                            RecordInstance r;
                            r.plugin = p.index;
                            r.signature = c.sig;
                            r.flags = c.flags;
                            r.dataSize = c.dataSize;
                            r.rawFormId = c.rawFormId;
                            r.offset = c.offset;
                            r.hash = c.hash;
                            r.loadPos = p.loadOrder;
                            r.key = session.resolve(p, c.rawFormId);
                            if (!c.editorId.empty())
                                r.editorId = session.strings.intern(c.editorId);
                            ++p.recordCount;
                            ++p.typeCounts[c.sig];
                            if (c.flags & RecordFlags::Deleted)
                                ++p.deletedCount;
                            out.push_back(r);
                        }
                        p.fileHash = cached->fileHash;
                        p.fromCache = true;
                        p.scanned = true;
                        size_t n = ++done;
                        progress.progress(n, session.loadOrder.size(), p.name);
                        return;
                    }
                }
                auto file = PluginFile::load(p.path);
                p.header = file.header();
                p.fileHash = fnv1a64(file.bytes().data(), file.bytes().size());
                auto& pc = pending[pos];
                pc.store = cache && p.modified != 0;
                pc.sample = sample;
                pc.index.fileHash = p.fileHash;
                file.forEachRecord([&](const RecordLocation& loc) {
                    if ((p.recordCount & 0xFFFF) == 0 && progress.cancelled())
                        return false;
                    RecordInstance r;
                    r.plugin = p.index;
                    r.signature = loc.header.signature;
                    r.flags = loc.header.flags;
                    r.dataSize = loc.header.dataSize;
                    r.rawFormId = loc.header.formId;
                    r.offset = loc.offset;
                    r.loadPos = p.loadOrder;
                    r.key = session.resolve(p, loc.header.formId);
                    uint32_t flagsForHash = loc.header.flags & ~RecordFlags::Compressed;
                    r.hash = fnv1a64(loc.rawData.data(), loc.rawData.size(), 0xcbf29ce484222325ULL ^ flagsForHash);
                    auto edid = quickEditorId(loc, defs.isCompilable(loc.header.signature));
                    if (!edid.empty())
                        r.editorId = session.strings.intern(edid);
                    ++p.recordCount;
                    ++p.typeCounts[loc.header.signature];
                    if (loc.header.deleted())
                        ++p.deletedCount;
                    out.push_back(r);
                    if (pc.store)
                        pc.index.records.push_back({r.signature, r.flags, r.dataSize, r.rawFormId, r.offset, r.hash, edid});
                    return true;
                });
                p.scanned = true;
            } catch (const std::exception& e) {
                p.error = e.what();
                spdlog::error("Failed to scan {}: {}", p.name, e.what());
            }
            size_t n = ++done;
            progress.progress(n, session.loadOrder.size(), p.name);
        },
        progress, options.threads);

    if (cache) {
        cache->begin();
        for (size_t pos = 0; pos < pending.size(); ++pos) {
            const auto& pc = pending[pos];
            const auto& p = session.plugins[session.loadOrder[pos]];
            if (pc.store && p.scanned)
                cache->storePluginIndex(pathToUtf8(p.path), p.fileSize, p.modified, pc.sample, pc.index);
        }
        cache->commit();
        spdlog::info("Plugin index cache: {} plugins reused, {} parsed", cache->plugins.hits.load(),
                     cache->plugins.misses.load());
    }

    size_t total = 0;
    for (const auto& v : perPlugin)
        total += v.size();
    session.records.clear();
    session.records.reserve(total);
    for (auto& v : perPlugin) {
        session.records.insert(session.records.end(), v.begin(), v.end());
        std::vector<RecordInstance>().swap(v);
    }

    progress.stage("Building override chains");
    std::sort(session.records.begin(), session.records.end(), [](const RecordInstance& a, const RecordInstance& b) {
        if (a.key != b.key)
            return a.key < b.key;
        return a.loadPos < b.loadPos;
    });

    session.groups.clear();
    for (uint32_t i = 0; i < session.records.size();) {
        uint32_t j = i + 1;
        while (j < session.records.size() && session.records[j].key == session.records[i].key)
            ++j;
        session.groups.push_back({i, j});
        // Classify each instance as new / override / injected.
        const auto& first = session.records[i];
        bool definedByOrigin = first.plugin == first.key.origin;
        for (uint32_t k = i; k < j; ++k) {
            auto& p = session.plugins[session.records[k].plugin];
            if (session.records[k].plugin == session.records[k].key.origin)
                ++p.newRecords;
            else if (!definedByOrigin && k == i)
                ++p.injected;
            else
                ++p.overrides;
        }
        i = j;
    }
    session.scanned = true;
    spdlog::info("Indexed {} records in {} plugins ({} unique forms)", session.records.size(), session.loadOrder.size(),
                 session.groups.size());
}

void PluginScanner::scan(Session& session, const ScanOptions& options, ProgressSink& progress)
{
    discoverPlugins(session, progress);
    indexRecords(session, options, progress);
}

} // namespace fo4mx
