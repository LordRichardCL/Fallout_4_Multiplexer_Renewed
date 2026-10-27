#include "engine/AssetTracer.hpp"

#include "assets/AssetPreview.hpp"
#include "util/Text.hpp"

#include <map>
#include <set>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace fo4mx {

namespace {

// Record subrecords that hold asset paths, and the folder the engine resolves them against.
const char* rootFor(Signature record, Signature sub)
{
    static const std::set<std::string> meshes = {"MODL", "MOD2", "MOD3", "MOD4", "MOD5", "DMDL"};
    static const std::set<std::string> icons = {"ICON", "MICO", "ICO2", "MIC2"};
    std::string s = sub.str();
    // In armours and armour addons MODL holds FormIDs (armatures, additional races), not a mesh.
    if (s == "MODL" && (record == sig::ARMO || record == Signature("ARMA")))
        return nullptr;
    if (meshes.count(s))
        return "meshes\\";
    if (record == Signature("TXST")) {
        if (s.size() == 4 && s[0] == 'T' && s[1] == 'X' && s[2] == '0' && s[3] >= '0' && s[3] <= '7')
            return "textures\\";
        if (s == "MNAM")
            return "materials\\";
    }
    if (record == Signature("MSWP") && (s == "BNAM" || s == "SNAM"))
        return "materials\\";
    if (icons.count(s))
        return "textures\\";
    if (record == sig::AMMO && s == "NAM1")
        return "meshes\\"; // shell casing model
    return nullptr;
}

std::string underRoot(const std::string& value, const char* root)
{
    std::string p = normalizeAssetPath(value);
    if (p.empty())
        return p;
    return istartsWith(p, root) ? p : std::string(root) + p;
}

struct Child {
    std::string path;
    std::string role;
};

} // namespace

AssetTrace AssetTracer::trace(Session& session, const CompiledOutput& compiled, bool followMeshes, ProgressSink& progress)
{
    progress.stage("Tracing assets");
    AssetTrace out;
    std::set<std::string> compiledNames;
    for (auto pi : compiled.compiledPlugins)
        compiledNames.insert(session.plugins[pi].key);

    std::unordered_map<std::string, std::vector<Child>> children; // parsed meshes/materials

    auto locate = [&](TracedAsset& a) {
        const AssetEntry* e = session.assets.find(a.path);
        a.found = e != nullptr;
        if (!e)
            return;
        const auto& w = e->winner();
        a.mod = session.assets.modName(w);
        a.loose = w.archive == kLooseFile;
        if (a.loose) {
            a.physical = pathToUtf8(session.vfs.physicalPath(session.vfs.files()[w.entry], w.slot));
        } else {
            const auto& ar = session.assets.archives[w.archive];
            a.archive = ar.name;
            a.physical = ar.archive ? pathToUtf8(ar.archive->path()) : ar.name;
            if (ar.owner != kInvalidPlugin) {
                a.archiveOwner = session.plugins[ar.owner].name;
                a.needsRegistration = compiledNames.count(session.plugins[ar.owner].key) != 0;
            }
        }
    };

    auto childrenOf = [&](const std::string& path) -> const std::vector<Child>& {
        auto it = children.find(path);
        if (it != children.end())
            return it->second;
        std::vector<Child> list;
        try {
            if (iendsWith(path, ".nif")) {
                auto bytes = session.readAsset(path, 4 * 1024 * 1024);
                if (auto m = preview::readNifInfo(bytes))
                    for (const auto& s : m->strings) {
                        if (iendsWith(s, ".bgsm") || iendsWith(s, ".bgem"))
                            list.push_back({underRoot(s, "materials\\"), "Material"});
                        else if (iendsWith(s, ".dds"))
                            list.push_back({underRoot(s, "textures\\"), "Texture"});
                    }
            } else if (iendsWith(path, ".bgsm") || iendsWith(path, ".bgem")) {
                auto bytes = session.readAsset(path);
                if (auto m = preview::readMaterialInfo(bytes))
                    for (const auto& t : m->textures)
                        if (iendsWith(t, ".dds"))
                            list.push_back({underRoot(t, "textures\\"), "Texture"});
            }
        } catch (const std::exception& e) {
            spdlog::debug("Cannot inspect {}: {}", path, e.what());
        }
        return children.emplace(path, std::move(list)).first->second;
    };

    for (uint32_t ri = 0; ri < compiled.records.size(); ++ri) {
        const auto& rec = compiled.records[ri];
        std::set<std::string> seen;
        bool any = false;
        auto add = [&](const std::string& path, const std::string& role, const std::string& via, bool direct) {
            if (path.empty() || !seen.insert(path).second)
                return;
            TracedAsset a;
            a.record = ri;
            a.path = path;
            a.role = role;
            a.via = via;
            locate(a);
            if (!a.found && direct)
                out.missing++;
            if (a.needsRegistration)
                out.viaRegisteredArchives++;
            out.assets.push_back(std::move(a));
            any = true;
        };
        auto visitSub = [&](const NormSub& s) {
            const char* root = rootFor(rec.sig, s.sig);
            if (!root || !s.refs.empty() || s.bytes.empty() || s.bytes.back() != 0)
                return;
            std::string value(reinterpret_cast<const char*>(s.bytes.data()));
            std::string path = underRoot(value, root);
            if (path.size() <= std::string(root).size())
                return;
            add(path, std::string(root) == "meshes\\" ? "Mesh" : "Icon", {}, true);
            if (!followMeshes)
                return;
            for (const auto& c : childrenOf(path)) {
                add(c.path, c.role, path, false);
                if (c.role == "Material")
                    for (const auto& t : childrenOf(c.path))
                        add(t.path, t.role, c.path, false);
            }
        };
        for (const auto& f : rec.fields.fields) {
            for (const auto& s : f.subs)
                visitSub(s);
            for (const auto& item : f.items)
                for (const auto& s : item.subs)
                    visitSub(s);
        }
        if (any)
            out.records++;
        if ((ri & 0x3FF) == 0)
            progress.progress(ri, compiled.records.size(), "assets");
    }
    spdlog::info("Asset trace: {} assets referenced by {} compiled records, {} missing, {} served by archives of "
                 "compiled plugins",
                 out.assets.size(), out.records, out.missing, out.viaRegisteredArchives);
    return out;
}

} // namespace fo4mx
