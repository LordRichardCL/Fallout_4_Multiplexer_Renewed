#include "engine/PrevisCheck.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "esp/PrevisFile.hpp"
#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <set>
#include <vector>

namespace fo4mx {

namespace {

const NormSub* subOf(const EmittedRecord& e, Signature sig)
{
    for (const auto& f : e.fields.fields)
        for (const auto& sub : f.subs)
            if (sub.sig == sig)
                return &sub;
    return nullptr;
}

std::string hex8(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", v);
    return buf;
}

bool assetExists(Session& session, const std::string& path)
{
    return session.assets.find(path) != nullptr || session.vfs.find(path) != nullptr;
}

std::string assetSource(Session& session, const std::string& path)
{
    if (const auto* a = session.assets.find(path))
        return session.assets.strings.get(a->winner().mod);
    if (const auto* f = session.vfs.find(path))
        return session.vfs.winnerName(*f);
    return {};
}

} // namespace

std::string checkPrevis(Session& session, const EmittedRecord& e)
{
    std::set<FormKey> xpri, xcri;
    if (const auto* s = subOf(e, Signature("XPRI")))
        for (const auto& [off, k] : s->refs)
            xpri.insert(k);
    std::vector<uint32_t> meshes;
    if (const auto* s = subOf(e, Signature("XCRI"))) {
        for (const auto& [off, k] : s->refs)
            xcri.insert(k);
        if (s->bytes.size() >= 8) {
            const uint32_t n = loadLE<uint32_t>(s->bytes.data());
            for (uint32_t i = 0; i < n && 8 + 4 * size_t(i) + 4 <= s->bytes.size(); ++i)
                meshes.push_back(loadLE<uint32_t>(s->bytes.data() + 8 + 4 * size_t(i)));
        }
    }

    std::string out = "Previs check of " + session.formLabel(e.key) + " (cell record from " +
                      session.pluginName(e.winner) + "; XPRI " + std::to_string(xpri.size()) + ", XCRI " +
                      std::to_string(xcri.size()) + " references, " + std::to_string(meshes.size()) + " meshes). ";

    // Precombined meshes: found by the cell's own plugin (no folder for Fallout4.esm) and FormID.
    if (!meshes.empty()) {
        const std::string plugin = session.pluginName(e.key.origin);
        const std::string folder = iequals(plugin, "Fallout4.esm") ? std::string() : plugin + "\\";
        size_t found = 0;
        std::string missing;
        for (uint32_t m : meshes) {
            const std::string path =
                normalizeAssetPath("meshes\\precombined\\" + folder + hex8(e.key.objectId) + "_" + hex8(m) + "_oc.nif");
            if (assetExists(session, path))
                ++found;
            else if (missing.empty())
                missing = path;
        }
        out += "Precombined meshes found: " + std::to_string(found) + " of " + std::to_string(meshes.size()) +
               (missing.empty() ? std::string() : " (first missing: " + missing + ")") + ". ";
    }

    // The previs file: the cell's own, or the one RVIS names (an exterior cell's cluster).
    FormKey visCell = e.key;
    if (const auto* s = subOf(e, Signature("RVIS")))
        for (const auto& [off, k] : s->refs)
            if (k.valid())
                visCell = k;
    const std::string path =
        normalizeAssetPath("vis\\" + session.pluginName(visCell.origin) + "\\" + hex8(visCell.objectId) + ".uvd");
    if (!assetExists(session, path))
        return out + "No previs file at " + path + ".";
    const auto bytes = session.readAsset(path);
    const auto file = readPrevisFile(bytes);
    out += "Previs file " + path + " (from " + assetSource(session, path) + ", " + std::to_string(bytes.size()) +
           " bytes): ";
    if (!file.recognised)
        return out + "layout not recognised: " + file.problem + ".";
    std::vector<uint32_t> formIds;
    for (uint32_t id : file.ids)
        if (!PrevisFile::isCombinedObject(id))
            formIds.push_back(id);
    out += std::to_string(file.ids.size()) + " IDs, " + std::to_string(file.ids.size() - formIds.size()) +
           " combined objects, " + std::to_string(formIds.size()) + " FormIDs.";

    // Read the FormIDs in the numbering of each plugin that could own them.
    std::vector<PluginIndex> owners;
    for (PluginIndex p : {e.winner, e.key.origin, visCell.origin})
        if (p < session.plugins.size() && std::find(owners.begin(), owners.end(), p) == owners.end())
            owners.push_back(p);
    for (PluginIndex p : owners) {
        size_t exist = 0, inXpri = 0, inXcri = 0;
        std::set<FormKey> seen;
        for (uint32_t raw : formIds) {
            const FormKey k = session.resolve(session.plugins[p], raw);
            if (!k.valid())
                continue;
            seen.insert(k);
            exist += session.exists(k);
            inXpri += xpri.count(k);
            inXcri += xcri.count(k);
        }
        size_t xpriMissing = 0;
        for (const auto& k : xpri)
            xpriMissing += !seen.count(k);
        out += " Read in " + session.plugins[p].name + "'s numbering: " + std::to_string(exist) + " exist, " +
               std::to_string(inXpri) + " in XPRI, " + std::to_string(inXcri) + " in XCRI; " +
               std::to_string(xpriMissing) + " XPRI references not in the file.";
    }
    return out;
}

} // namespace fo4mx
