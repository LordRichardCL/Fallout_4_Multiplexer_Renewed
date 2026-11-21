#include "engine/PrevisCheck.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"
#include "esp/PrevisFile.hpp"
#include "assets/AssetPreview.hpp"
#include "util/Binary.hpp"
#include "util/Text.hpp"

#include <cstdio>
#include <map>
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
    if (e.output < 0)
        return {};
    // Unused IDs in the output's own range, for IDs that point to no record today: above every new record
    // the output holds, one per distinct ID of this file, so the file's ID table keeps its distinct entries.
    const auto& op = out.outputs.at(static_cast<size_t>(e.output));
    uint32_t nextUnused = 0x800;
    for (uint32_t ri : op.records) {
        const auto& r = out.records[ri];
        if (r.isNew && r.objectId >= nextUnused)
            nextUnused = r.objectId + 1;
    }
    return translatePrevisInto(
        session, e, out.outputName(e.output),
        [&](FormKey k, uint32_t& raw) {
            bool ok = true;
            raw = out.translate(session, k, e.output, &ok);
            return ok;
        },
        static_cast<uint32_t>(op.masterNames.size()), nextUnused, op.light ? 0xFFFu : 0xFFFFFFu);
}

PrevisTranslation translatePrevisInto(Session& session, const EmittedRecord& e, const std::string& target,
                                      const std::function<bool(FormKey, uint32_t&)>& express, uint32_t ownIndex,
                                      uint32_t nextUnused, uint32_t maxObject)
{
    PrevisTranslation t;
    t.path = previsFilePath(session, e.key);
    if (!ownsPrevisFile(e) || !assetExists(session, t.path))
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
    std::map<uint32_t, uint32_t> nowhereIds; // raw -> ID that finds nothing in the target
    for (uint32_t i = 0; i < file.ids.size(); ++i) {
        const uint32_t raw = file.ids[i];
        if (PrevisFile::isCombinedObject(raw))
            continue;
        ++t.formIds;
        const FormKey k = session.resolve(numbering, raw);
        const bool pointsNowhere = !k.valid() ? (raw & 0xFFFFFFu) != 0 : session.winner(k) == nullptr;
        if (pointsNowhere) {
            auto [it, fresh] = nowhereIds.emplace(raw, 0);
            if (fresh) {
                if (nextUnused > maxObject) {
                    t.ok = false;
                    t.problem = "its FormID " + hex8(raw) + " points to no record today, and " + target +
                                " has no unused ID left to give it";
                    return t;
                }
                it->second = makeRawFormId(ownIndex, nextUnused++);
                ++t.nowhere;
                if (t.nowhereExamples.size() < 3)
                    t.nowhereExamples.push_back(hex8(raw) + " (" +
                                                (k.valid() ? session.formLabel(k) + " in " + session.pluginName(k.origin)
                                                           : std::string("master index past ") + t.numbering +
                                                                 "'s master list") +
                                                ")");
            }
            if (it->second != raw) {
                storeLE<uint32_t>(t.bytes.data() + file.idOffset + 4 * size_t(i), it->second);
                ++t.changed;
            }
            continue;
        }
        uint32_t now = 0;
        const bool ok = k.valid() && express(k, now);
        if (!ok) {
            t.ok = false;
            t.problem = "its FormID " + hex8(raw) + " (" +
                        (k.valid() ? session.formLabel(k) + " in " + session.pluginName(k.origin) : std::string("no plugin")) +
                        ", read in " + t.numbering + "'s numbering) cannot be expressed in " + target;
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

bool previsRecords(Session& session, const EmittedRecord& e, std::vector<FormKey>& keys, std::string& problem)
{
    keys.clear();
    const std::string path = previsFilePath(session, e.key);
    if (!ownsPrevisFile(e) || !assetExists(session, path))
        return true;
    const auto* winner = session.winner(e.key);
    if (!winner) {
        problem = "the cell's winning record was not found";
        return false;
    }
    const auto file = readPrevisFile(session.readAsset(path));
    if (!file.recognised) {
        problem = path + ": its layout is not recognised (" + file.problem + ")";
        return false;
    }
    std::set<FormKey> seen;
    for (uint32_t raw : file.ids) {
        if (PrevisFile::isCombinedObject(raw))
            continue;
        const FormKey k = session.resolve(session.plugins[winner->plugin], raw);
        if (k.valid() && session.winner(k) && seen.insert(k).second)
            keys.push_back(k);
    }
    return true;
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

bool assessNewCell(Session& session, const EmittedRecord& e, std::string& text)
{
    const auto& plugin = session.plugins[e.key.origin];
    const std::string id = hex8(e.key.objectId);
    std::vector<std::string> blockers, facts;

    const bool exterior = e.placement == Placement::Kind::WorldCell;
    facts.push_back(exterior ? "exterior" : "interior");
    if (exterior)
        blockers.push_back("exterior cell (stage PC3)");

    for (const std::string& file : {plugin.baseName() + " - Geometry.csg", plugin.baseName() + ".cdx"})
        if (assetExists(session, normalizeAssetPath(file)))
            blockers.push_back(file + " is installed (\"clean\" precombines load geometry by the plugin's name)");

    std::vector<uint32_t> meshes;
    if (const auto* s = subOf(e, Signature("XCRI")); s && s->bytes.size() >= 8) {
        const uint32_t n = loadLE<uint32_t>(s->bytes.data());
        for (uint32_t i = 0; i < n && 8 + 4 * size_t(i) + 4 <= s->bytes.size(); ++i)
            meshes.push_back(loadLE<uint32_t>(s->bytes.data() + 8 + 4 * size_t(i)));
    }
    if (!meshes.empty()) {
        const std::string folder =
            "meshes\\precombined\\" + (iequals(plugin.name, "Fallout4.esm") ? std::string() : plugin.name + "\\");
        size_t found = 0, physics = 0, named = 0, read = 0;
        std::string otherName;
        for (uint32_t m : meshes) {
            const std::string base = folder + id + "_" + hex8(m);
            const std::string oc = normalizeAssetPath(base + "_oc.nif");
            if (!assetExists(session, oc))
                continue;
            ++found;
            physics += assetExists(session, normalizeAssetPath(base + "_physics.nif"));
            try {
                if (auto info = preview::readNifInfo(session.readAsset(oc))) {
                    ++read;
                    bool hit = false;
                    for (const auto& str : info->strings)
                        hit |= toLower(str).find(toLower(id)) != std::string::npos;
                    named += hit;
                    if (!hit && otherName.empty() && !info->strings.empty())
                        otherName = info->strings.front();
                }
            } catch (const std::exception&) {
            }
        }
        facts.push_back(std::to_string(found) + " of " + std::to_string(meshes.size()) + " meshes found (" +
                        std::to_string(physics) + " with a physics mesh), " + std::to_string(named) + " of " +
                        std::to_string(read) + " read name the cell's FormID " + id);
        if (found < meshes.size())
            blockers.push_back(std::to_string(meshes.size() - found) + " mesh(es) XCRI names are not installed");
        if (named < read)
            blockers.push_back("a mesh's strings do not name the cell's FormID (first string: \"" + otherName +
                               "\"): renaming it is not understood yet");
        if (read < found)
            blockers.push_back("a mesh could not be read");
    }

    const bool own = ownsPrevisFile(e);
    const bool hasPrevisData = subOf(e, Signature("VISI")) || subOf(e, Signature("XPRI")) || !own;
    if (!own) {
        FormKey cluster = e.key;
        if (const auto* s = subOf(e, Signature("RVIS")))
            for (const auto& [off, k] : s->refs)
                if (k.valid())
                    cluster = k;
        facts.push_back("in the previs file of " + session.formLabel(cluster) + " (" +
                        session.pluginName(cluster.origin) + ")");
        blockers.push_back("its previs file is another cell's (stage PC3)");
    } else if (hasPrevisData) {
        const std::string path = previsFilePath(session, e.key);
        if (assetExists(session, path)) {
            const auto file = readPrevisFile(session.readAsset(path));
            facts.push_back("previs file " + path + (file.recognised ? " readable" : " not recognised"));
            if (!file.recognised)
                blockers.push_back("its previs file's layout is not recognised: " + file.problem);
        } else {
            facts.push_back("no previs file at " + path);
        }
    }

    text = "New cell " + session.formLabel(e.key) + " of " + plugin.name + ": " + join(facts, "; ") + ". ";
    if (blockers.empty())
        text += "Stage PC2 could move its files under the output's name and its new FormID.";
    else
        text += "Stays out: " + join(blockers, "; ") + ".";
    return blockers.empty();
}

} // namespace fo4mx
