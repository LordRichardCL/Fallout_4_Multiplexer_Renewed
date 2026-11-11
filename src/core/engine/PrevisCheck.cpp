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

std::string previsFilePath(const Session& session, FormKey cell)
{
    return normalizeAssetPath("vis\\" + session.pluginName(cell.origin) + "\\" + hex8(cell.objectId) + ".uvd");
}

bool ownsPrevisFile(const EmittedRecord& e)
{
    if (const auto* s = subOf(e, Signature("RVIS")))
        for (const auto& [off, k] : s->refs)
            if (k.valid() && k != e.key)
                return false;
    return true;
}

PrevisTranslation translatePrevis(Session& session, const CompiledOutput& out, const EmittedRecord& e)
{
    PrevisTranslation t;
    t.path = previsFilePath(session, e.key);
    if (e.output < 0 || !ownsPrevisFile(e) || !assetExists(session, t.path))
        return t;
    t.hasFile = true;
    t.source = assetSource(session, t.path);
    const auto* winner = session.winner(e.key);
    if (!winner) {
        t.ok = false;
        t.problem = "the cell's winning record was not found";
        return t;
    }
    t.numbering = session.pluginName(winner->plugin);
    t.bytes = session.readAsset(t.path);
    const auto file = readPrevisFile(t.bytes);
    if (!file.recognised) {
        t.ok = false;
        t.problem = "its layout is not recognised (" + file.problem + ")";
        return t;
    }
    const auto& numbering = session.plugins[winner->plugin];
    for (uint32_t i = 0; i < file.ids.size(); ++i) {
        const uint32_t raw = file.ids[i];
        if (PrevisFile::isCombinedObject(raw))
            continue;
        ++t.formIds;
        const FormKey k = session.resolve(numbering, raw);
        bool ok = k.valid();
        const uint32_t now = ok ? out.translate(session, k, e.output, &ok) : 0;
        if (!ok) {
            t.ok = false;
            t.problem = "its FormID " + hex8(raw) + " (" +
                        (k.valid() ? session.formLabel(k) + " in " + session.pluginName(k.origin) : std::string("no plugin")) +
                        ", read in " + t.numbering + "'s numbering) cannot be expressed in " +
                        out.outputName(e.output);
            return t;
        }
        if (now != raw) {
            storeLE<uint32_t>(t.bytes.data() + file.idOffset + 4 * size_t(i), now);
            ++t.changed;
        }
    }
    // The copy must read back as the same file with only its ID table changed.
    const auto check = readPrevisFile(t.bytes);
    if (!check.recognised || check.ids.size() != file.ids.size()) {
        t.ok = false;
        t.problem = "the translated copy does not read back";
    }
    return t;
}

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
        // For the winning cell record's plugin, name what does not match (up to 5 each), so a
        // difference can be traced: file IDs that are no XPRI reference, and XPRI references the
        // file does not have.
        if (p != e.winner)
            continue;
        constexpr size_t kShown = 5;
        std::string unmatched;
        size_t unmatchedCount = 0;
        for (uint32_t raw : formIds) {
            const FormKey k = session.resolve(session.plugins[p], raw);
            if (k.valid() && xpri.count(k))
                continue;
            if (++unmatchedCount > kShown)
                continue;
            unmatched += (unmatched.empty() ? "" : ", ") + hex8(raw) + " = " +
                         (!k.valid()            ? std::string("no plugin")
                          : session.exists(k)   ? session.formLabel(k) + " in " + session.pluginName(k.origin)
                                                : "nothing in " + session.pluginName(k.origin));
        }
        if (unmatchedCount)
            out += " File IDs not in XPRI (" + std::to_string(unmatchedCount) + "): " + unmatched +
                   (unmatchedCount > kShown ? ", ..." : "") + ".";
        std::string absent;
        size_t absentCount = 0;
        for (const auto& k : xpri) {
            if (seen.count(k) || ++absentCount > kShown)
                continue;
            absent += (absent.empty() ? "" : ", ") + session.formLabel(k) + " in " + session.pluginName(k.origin);
        }
        if (absentCount)
            out += " XPRI references not in the file (" + std::to_string(absentCount) + "): " + absent +
                   (absentCount > kShown ? ", ..." : "") + ".";
    }
    return out;
}

} // namespace fo4mx
