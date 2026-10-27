#include "engine/ChangeDetector.hpp"

#include "engine/BuildCache.hpp"
#include "engine/PluginScanner.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* buildStatusName(BuildStatus s)
{
    switch (s) {
    case BuildStatus::NotBuilt: return "Not Built";
    case BuildStatus::UpToDate: return "Up To Date";
    case BuildStatus::ChangesDetected: return "Changes Detected";
    case BuildStatus::RequiresRebuild: return "Requires Rebuild";
    }
    return "Not Built";
}

namespace {

int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

json::Value fileJson(const FileFingerprint& f)
{
    json::Value o;
    o.set("name", f.name);
    o.set("mod", f.mod);
    o.set("size", f.size);
    o.set("modified", static_cast<int64_t>(f.modified));
    o.set("hash", std::to_string(f.hash)); // 64-bit: keep exact
    o.set("active", f.active);
    o.set("loadOrder", f.loadOrder);
    o.set("recordTypes", f.recordTypes);
    return o;
}

FileFingerprint fileFrom(const json::Value& o)
{
    FileFingerprint f;
    f.name = o["name"].asString();
    f.mod = o["mod"].asString();
    f.size = static_cast<uint64_t>(o["size"].asNumber());
    f.modified = static_cast<int64_t>(o["modified"].asNumber());
    f.hash = std::strtoull(o["hash"].asString("0").c_str(), nullptr, 10);
    f.active = o["active"].asBool();
    f.loadOrder = static_cast<int32_t>(o["loadOrder"].asNumber(-1));
    f.recordTypes = o["recordTypes"].asString();
    return f;
}

// Mods from the VFS: every source root, with an order-independent content hash.
std::vector<ModFingerprint> modsFrom(const ResolvedInstall& install, const VirtualFileSystem& vfs, uint64_t& looseFiles)
{
    std::map<std::string, ModFingerprint> mods;
    looseFiles = 0;
    for (const auto& f : vfs.files()) {
        std::string key = normalizeAssetPath(f.relativePath);
        for (size_t s = 0; s < f.sources.size(); ++s) {
            std::string modName = vfs.roots()[f.sources[s]].name;
            auto vit = install.vortexOwners.find(key);
            if (vit != install.vortexOwners.end())
                modName = vit->second;
            auto& m = mods[modName];
            m.name = modName;
            m.files++;
            m.bytes += f.sizes[s];
            // Plugins are tracked by content hash separately; leaving them out here means
            // touching a plugin without changing it does not mark its mod as updated.
            bool plugin = key.find('\\') == std::string::npos &&
                          (iendsWith(key, ".esp") || iendsWith(key, ".esm") || iendsWith(key, ".esl"));
            uint64_t parts[2] = {f.sizes[s], plugin ? 0 : static_cast<uint64_t>(f.mtimes[s])};
            m.hash += fnv1a64(parts, sizeof(parts), fnv1a64(key.data(), key.size()));
            if (key.find('\\') != std::string::npos)
                ++looseFiles;
        }
    }
    for (const auto& mi : install.mods) {
        auto& m = mods[mi.name];
        m.name = mi.name;
        m.enabled = mi.enabled;
        m.priority = mi.priority;
    }
    std::vector<ModFingerprint> out;
    for (auto& [_, m] : mods)
        out.push_back(std::move(m));
    return out;
}

std::vector<FileFingerprint> archivesFrom(const VirtualFileSystem& vfs, const InstallSnapshot* reference)
{
    std::unordered_map<std::string, const FileFingerprint*> previous;
    if (reference)
        for (const auto& a : reference->archives)
            previous[toLower(a.name)] = &a;
    std::vector<FileFingerprint> out;
    for (const auto* f : vfs.rootFiles()) {
        if (!iendsWith(f->relativePath, ".ba2"))
            continue;
        FileFingerprint a;
        a.name = f->relativePath;
        a.mod = vfs.winnerName(*f);
        a.size = f->sizes.back();
        a.modified = f->mtimes.back();
        auto it = previous.find(toLower(a.name));
        if (it != previous.end() && it->second->size == a.size && it->second->modified == a.modified)
            a.hash = it->second->hash;
        else
            a.hash = sampleFileHash(vfs.winnerPath(*f), a.size);
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return toLower(a.name) < toLower(b.name); });
    return out;
}

std::string typesOf(const PluginInfo& p)
{
    std::vector<std::string> t;
    for (const auto& [s, n] : p.typeCounts)
        t.push_back(s.str());
    return join(t, " ");
}

} // namespace

json::Value InstallSnapshot::toJson() const
{
    json::Value v;
    v.set("version", 1);
    v.set("created", static_cast<int64_t>(created));
    v.set("installKind", installKind);
    v.set("profile", profile);
    v.set("looseFiles", looseFiles);
    json::Value m;
    for (const auto& mod : mods) {
        json::Value o;
        o.set("name", mod.name);
        o.set("enabled", mod.enabled);
        o.set("priority", mod.priority);
        o.set("files", mod.files);
        o.set("bytes", mod.bytes);
        o.set("hash", std::to_string(mod.hash));
        m.push(o);
    }
    v.set("mods", m);
    json::Value p, a, o, lo, cp, pa;
    for (const auto& f : plugins)
        p.push(fileJson(f));
    for (const auto& f : archives)
        a.push(fileJson(f));
    for (const auto& f : outputs)
        o.push(fileJson(f));
    for (const auto& s : loadOrder)
        lo.push(s);
    for (const auto& s : compiledPlugins)
        cp.push(s);
    for (const auto& s : participants)
        pa.push(s);
    v.set("plugins", p);
    v.set("archives", a);
    v.set("loadOrder", lo);
    v.set("isBuild", isBuild);
    v.set("compiledPlugins", cp);
    v.set("participants", pa);
    v.set("outputDirectory", outputDirectory);
    v.set("outputs", o);
    v.set("settingsHash", std::to_string(settingsHash));
    return v;
}

InstallSnapshot InstallSnapshot::fromJson(const json::Value& v)
{
    InstallSnapshot s;
    s.created = static_cast<int64_t>(v["created"].asNumber());
    s.installKind = v["installKind"].asString();
    s.profile = v["profile"].asString();
    s.looseFiles = static_cast<uint64_t>(v["looseFiles"].asNumber());
    for (const auto& o : v["mods"].asArray()) {
        ModFingerprint m;
        m.name = o["name"].asString();
        m.enabled = o["enabled"].asBool(true);
        m.priority = static_cast<int32_t>(o["priority"].asNumber());
        m.files = static_cast<uint64_t>(o["files"].asNumber());
        m.bytes = static_cast<uint64_t>(o["bytes"].asNumber());
        m.hash = std::strtoull(o["hash"].asString("0").c_str(), nullptr, 10);
        s.mods.push_back(std::move(m));
    }
    for (const auto& o : v["plugins"].asArray())
        s.plugins.push_back(fileFrom(o));
    for (const auto& o : v["archives"].asArray())
        s.archives.push_back(fileFrom(o));
    for (const auto& o : v["outputs"].asArray())
        s.outputs.push_back(fileFrom(o));
    for (const auto& o : v["loadOrder"].asArray())
        s.loadOrder.push_back(o.asString());
    s.isBuild = v["isBuild"].asBool();
    for (const auto& o : v["compiledPlugins"].asArray())
        s.compiledPlugins.push_back(o.asString());
    for (const auto& o : v["participants"].asArray())
        s.participants.push_back(o.asString());
    s.outputDirectory = v["outputDirectory"].asString();
    s.settingsHash = std::strtoull(v["settingsHash"].asString("0").c_str(), nullptr, 10);
    return s;
}

InstallSnapshot ChangeDetector::capture(const Session& session)
{
    InstallSnapshot s;
    s.created = nowSeconds();
    s.installKind = installKindName(session.install.config.kind);
    s.profile = pathToUtf8(session.install.profilePath.filename());
    s.mods = modsFrom(session.install, session.vfs, s.looseFiles);
    for (const auto& p : session.plugins) {
        FileFingerprint f;
        f.name = p.name;
        f.mod = p.sourceMod;
        f.size = p.fileSize;
        f.modified = p.modified;
        f.hash = p.fileHash ? p.fileHash : fullFileHash(p.path);
        f.active = p.active;
        f.loadOrder = p.loadOrder;
        f.recordTypes = typesOf(p);
        s.plugins.push_back(std::move(f));
    }
    s.archives = archivesFrom(session.vfs, nullptr);
    for (auto pi : session.loadOrder)
        s.loadOrder.push_back(session.plugins[pi].name);
    return s;
}

void ChangeDetector::addBuild(InstallSnapshot& s, const Session& session, const CompiledOutput& compiled,
                              const ExportResult& exported, uint64_t settings)
{
    s.isBuild = true;
    s.settingsHash = settings;
    s.outputDirectory = pathToUtf8(exported.directory);
    for (auto pi : compiled.compiledPlugins)
        s.compiledPlugins.push_back(session.plugins[pi].name);
    std::set<PluginIndex> participants;
    for (const auto& e : compiled.records)
        participants.insert(e.chain.begin(), e.chain.end());
    for (const auto& op : compiled.outputs)
        participants.insert(op.retainedMasters.begin(), op.retainedMasters.end());
    for (auto pi : participants)
        s.participants.push_back(session.plugins[pi].name);
    for (const auto& path : exported.pluginFiles) {
        FileFingerprint f;
        f.name = pathToUtf8(path.filename());
        std::error_code ec;
        f.size = std::filesystem::file_size(path, ec);
        f.hash = fullFileHash(path);
        s.outputs.push_back(std::move(f));
    }
}

InstallSnapshot ChangeDetector::captureQuick(const InstallConfig& config, const InstallSnapshot* reference,
                                             ProgressSink& progress)
{
    progress.stage("Checking for changes");
    Session session;
    session.install = resolveInstall(config);
    populateVfs(session.install, session.vfs, progress);
    PluginScanner::discoverPlugins(session, progress);

    InstallSnapshot s;
    s.created = nowSeconds();
    s.installKind = installKindName(config.kind);
    s.profile = pathToUtf8(session.install.profilePath.filename());
    s.mods = modsFrom(session.install, session.vfs, s.looseFiles);

    std::unordered_map<std::string, const FileFingerprint*> previous;
    if (reference)
        for (const auto& p : reference->plugins)
            previous[toLower(p.name)] = &p;
    for (const auto& p : session.plugins) {
        FileFingerprint f;
        f.name = p.name;
        f.mod = p.sourceMod;
        f.size = p.fileSize;
        f.modified = p.modified;
        f.active = p.active;
        f.loadOrder = p.loadOrder;
        auto it = previous.find(p.key);
        if (it != previous.end() && it->second->size == f.size && it->second->modified == f.modified) {
            f.hash = it->second->hash;
            f.recordTypes = it->second->recordTypes;
        } else {
            f.hash = fullFileHash(p.path); // touched: confirm the content really changed
            if (it != previous.end() && it->second->hash == f.hash)
                f.recordTypes = it->second->recordTypes;
        }
        s.plugins.push_back(std::move(f));
    }
    s.archives = archivesFrom(session.vfs, reference);
    for (auto pi : session.loadOrder)
        s.loadOrder.push_back(session.plugins[pi].name);

    if (reference)
        fingerprintOutputs(*reference, s);
    return s;
}

void ChangeDetector::fingerprintOutputs(const InstallSnapshot& reference, InstallSnapshot& current)
{
    if (!reference.isBuild)
        return;
    current.outputs.clear();
    auto dir = utf8ToPath(reference.outputDirectory);
    for (const auto& o : reference.outputs) {
        FileFingerprint f;
        f.name = o.name;
        auto path = dir / utf8ToPath(o.name);
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            f.size = std::filesystem::file_size(path, ec);
            f.hash = f.size == o.size ? fullFileHash(path) : 1;
        }
        current.outputs.push_back(std::move(f));
    }
}

ChangeReport ChangeDetector::compare(const InstallSnapshot* reference, const InstallSnapshot& current)
{
    ChangeReport r;
    r.checkedTime = current.created;
    if (!reference || !reference->isBuild) {
        r.status = BuildStatus::NotBuilt;
        r.reasons.push_back("No compiled output has been exported yet");
        return r;
    }
    r.referenceTime = reference->created;
    std::set<std::string> compiled, participants;
    for (const auto& p : reference->compiledPlugins)
        compiled.insert(toLower(p));
    for (const auto& p : reference->participants)
        participants.insert(toLower(p));
    std::set<std::string> affectedTypes;
    auto addTypes = [&](const std::string& types) {
        for (const auto& t : split(types, ' '))
            if (!t.empty())
                affectedTypes.insert(t);
    };
    auto add = [&](ChangeItem item) {
        if (item.affectsBuild && r.reasons.size() < 12)
            r.reasons.push_back(item.category + " " + item.kind + ": " + item.name);
        r.items.push_back(std::move(item));
    };

    // Mods
    std::map<std::string, const ModFingerprint*> oldMods, newMods;
    for (const auto& m : reference->mods)
        oldMods[m.name] = &m;
    for (const auto& m : current.mods)
        newMods[m.name] = &m;
    for (const auto& [name, m] : newMods) {
        auto it = oldMods.find(name);
        if (it == oldMods.end()) {
            ++r.newMods;
            add({"Mod", "Added", name, formatCount(m->files) + " files, " + formatBytes(m->bytes), false});
        } else if (it->second->enabled != m->enabled) {
            add({"Mod", m->enabled ? "Enabled" : "Disabled", name, "", false});
            m->enabled ? ++r.newMods : ++r.removedMods;
        } else if (it->second->hash != m->hash || it->second->files != m->files) {
            ++r.updatedMods;
            long long delta = static_cast<long long>(m->files) - static_cast<long long>(it->second->files);
            add({"Mod", "Updated", name,
                 formatCount(m->files) + " files (" + (delta >= 0 ? "+" : "") + std::to_string(delta) + ")", false});
        }
    }
    for (const auto& [name, m] : oldMods)
        if (!newMods.count(name)) {
            ++r.removedMods;
            add({"Mod", "Removed", name, formatCount(m->files) + " files", false});
        }

    // Plugins
    std::map<std::string, const FileFingerprint*> oldPlugins, newPlugins;
    for (const auto& p : reference->plugins)
        oldPlugins[toLower(p.name)] = &p;
    for (const auto& p : current.plugins)
        newPlugins[toLower(p.name)] = &p;
    for (const auto& [key, p] : newPlugins) {
        auto it = oldPlugins.find(key);
        if (it == oldPlugins.end()) {
            ++r.addedPlugins;
            add({"Plugin", "Added", p->name, p->active ? "enabled in the load order" : "installed, not enabled",
                 p->active});
            continue;
        }
        const auto* o = it->second;
        bool role = compiled.count(key) || participants.count(key);
        if (o->active != p->active) {
            p->active ? ++r.addedPlugins : ++r.removedPlugins;
            add({"Plugin", p->active ? "Enabled" : "Disabled", p->name, role ? "part of the current build" : "", true});
            addTypes(o->recordTypes);
        } else if (o->hash != p->hash) {
            ++r.updatedPlugins;
            add({"Plugin", "Updated", p->name,
                 formatBytes(o->size) + " → " + formatBytes(p->size) + (compiled.count(key) ? " · compiled" : role ? " · participates" : ""),
                 p->active});
            addTypes(o->recordTypes);
        }
    }
    for (const auto& [key, o] : oldPlugins)
        if (!newPlugins.count(key)) {
            ++r.removedPlugins;
            add({"Plugin", "Removed", o->name, compiled.count(key) ? "was compiled" : "", o->active});
            addTypes(o->recordTypes);
        }

    // Load order (relative order of plugins active in both)
    std::set<std::string> inNew;
    for (const auto& n : current.loadOrder)
        inNew.insert(toLower(n));
    std::vector<std::string> before, after;
    for (const auto& n : reference->loadOrder)
        if (inNew.count(toLower(n)))
            before.push_back(toLower(n));
    std::set<std::string> inOld(before.begin(), before.end());
    for (const auto& n : current.loadOrder)
        if (inOld.count(toLower(n)))
            after.push_back(toLower(n));
    if (before != after) {
        std::map<std::string, size_t> oldPos;
        for (size_t i = 0; i < before.size(); ++i)
            oldPos[before[i]] = i;
        for (size_t i = 0; i < after.size(); ++i) {
            if (oldPos[after[i]] == i)
                continue;
            ++r.loadOrderChanges;
            const auto* np = newPlugins[after[i]];
            add({"Load Order", "Moved", np ? np->name : after[i],
                 "position " + std::to_string(oldPos[after[i]]) + " → " + std::to_string(i), true});
        }
    }

    // Archives
    std::map<std::string, const FileFingerprint*> oldArchives, newArchives;
    for (const auto& a : reference->archives)
        oldArchives[toLower(a.name)] = &a;
    for (const auto& a : current.archives)
        newArchives[toLower(a.name)] = &a;
    auto ownerCompiled = [&](const std::string& archive) {
        auto sep = archive.find(" - ");
        if (sep == std::string::npos)
            return false;
        std::string base = archive.substr(0, sep);
        return compiled.count(base + ".esp") || compiled.count(base + ".esm") || compiled.count(base + ".esl");
    };
    for (const auto& [key, a] : newArchives) {
        auto it = oldArchives.find(key);
        if (it == oldArchives.end()) {
            ++r.archiveChanges;
            add({"Archive", "Added", a->name, a->mod, ownerCompiled(key)});
        } else if (it->second->hash != a->hash) {
            ++r.archiveChanges;
            add({"Archive", "Updated", a->name, a->mod, false});
        }
    }
    for (const auto& [key, a] : oldArchives)
        if (!newArchives.count(key)) {
            ++r.archiveChanges;
            add({"Archive", "Removed", a->name, a->mod, ownerCompiled(key)});
        }

    // Generated output plugins
    std::map<std::string, const FileFingerprint*> currentOutputs;
    for (const auto& o : current.outputs)
        currentOutputs[toLower(o.name)] = &o;
    for (const auto& o : reference->outputs) {
        auto it = currentOutputs.find(toLower(o.name));
        if (it == currentOutputs.end())
            continue; // not checked (full rescans do not look at outputs)
        if (it->second->size == 0 && it->second->hash == 0)
            add({"Output", "Missing", o.name, "generated plugin was deleted", true});
        else if (it->second->hash != o.hash)
            add({"Output", "Modified", o.name, "generated plugin was changed outside the Multiplexer", true});
    }

    // Record types the changes touch, versus the compiled types they leave alone.
    std::set<std::string> compiledTypes;
    for (const auto& p : reference->plugins)
        if (compiled.count(toLower(p.name)))
            for (const auto& t : split(p.recordTypes, ' '))
                if (!t.empty())
                    compiledTypes.insert(t);
    r.affectedRecordTypes.assign(affectedTypes.begin(), affectedTypes.end());
    if (!affectedTypes.empty())
        for (const auto& t : compiledTypes)
            if (!affectedTypes.count(t))
                r.unaffectedRecordTypes.push_back(t);

    bool rebuild = std::any_of(r.items.begin(), r.items.end(), [](const ChangeItem& i) { return i.affectsBuild; });
    r.status = rebuild ? BuildStatus::RequiresRebuild
                       : (r.items.empty() ? BuildStatus::UpToDate : BuildStatus::ChangesDetected);
    return r;
}

uint64_t ChangeDetector::settingsHash(const CompileOptions& c, const std::vector<std::string>& excluded,
                                      const std::string& runtimeName, std::vector<std::string> resolutions)
{
    std::string s = std::to_string(c.outputCount) + "|" + c.outputPrefix + "|" + std::to_string(int(c.strategy)) + "|" +
                    std::to_string(c.mergeCompatible) + std::to_string(c.lightOutputsWhenPossible) +
                    std::to_string(c.rewriteHeuristicReferences) + "|" + runtimeName;
    for (const auto& r : c.rules)
        s += "|" + toLower(r.pattern) + "=" + std::to_string(r.output);
    std::vector<std::string> ex;
    for (const auto& e : excluded)
        ex.push_back(toLower(e));
    std::sort(ex.begin(), ex.end());
    s += "|" + join(ex, ",");
    for (auto& r : resolutions)
        r = toLower(r);
    std::sort(resolutions.begin(), resolutions.end());
    s += "|" + join(resolutions, ",");
    return fnv1a64(s.data(), s.size());
}

uint64_t ChangeDetector::settingsHash(const Session& session, const CompileOptions& c,
                                      const std::vector<std::string>& excluded)
{
    std::vector<std::string> forced;
    for (const auto& [k, v] : c.forcedWinners) {
        FormKey key = FormKey::unpack(k);
        forced.push_back(session.pluginName(key.origin) + ":" + std::to_string(key.objectId) + "=" + session.pluginName(v));
    }
    return settingsHash(c, excluded, session.defs.profileName(), std::move(forced));
}

} // namespace fo4mx
