#include "db/DatabaseWriter.hpp"

#include "db/Schema.hpp"
#include "util/Text.hpp"

#include <set>
#include <algorithm>

#include <spdlog/spdlog.h>

namespace fo4mx::db {

namespace {

std::string recordTypesSummary(const PluginInfo& p)
{
    std::vector<std::pair<uint32_t, std::string>> types;
    for (const auto& [s, n] : p.typeCounts)
        types.emplace_back(n, s.str());
    std::sort(types.rbegin(), types.rend());
    std::string out;
    for (size_t i = 0; i < types.size(); ++i) {
        if (i)
            out += ", ";
        out += types[i].second + " " + std::to_string(types[i].first);
    }
    return out;
}

std::string pluginStatus(const PluginInfo& p)
{
    if (!p.error.empty())
        return "Error";
    if (!p.missingMasters.empty())
        return "Missing Masters";
    if (!p.active)
        return "Disabled";
    if (p.hardConflictCount)
        return "Hard Conflicts";
    if (p.conflictCount)
        return "Conflicting";
    return "OK";
}

} // namespace

void DatabaseWriter::initSchema()
{
    db_.exec(kSchema);
    setMeta("schema_version", std::to_string(kSchemaVersion));
}

void DatabaseWriter::setMeta(const std::string& key, const std::string& value)
{
    auto st = db_.prepare("INSERT INTO Meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    st.bind(1, key).bind(2, value).run();
}

void DatabaseWriter::writePlugins(const Session& session)
{
    db_.exec("DELETE FROM Plugins; DELETE FROM Masters;");
    auto st = db_.prepare(
        "INSERT INTO Plugins(id, name, type, load_order, load_order_hex, active, base_game, creation_club, source_mod, "
        "path, file_size, header_version, author, description, flags, localized, record_count, new_records, "
        "override_count, injected_count, itm_count, deleted_count, conflict_count, hard_conflict_count, master_count, "
        "status, eligibility, eligibility_reason, archives, record_types) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto ms = db_.prepare(
        "INSERT INTO Masters(plugin_id, position, master_name, master_plugin_id, missing) VALUES(?,?,?,?,?)");
    for (const auto& p : session.plugins) {
        st.bind(1, p.index)
            .bind(2, p.name)
            .bind(3, pluginKindName(p.kind))
            .bind(4, p.loadOrder)
            .bind(5, p.loadOrderHex)
            .bind(6, p.active ? 1 : 0)
            .bind(7, p.baseGame ? 1 : 0)
            .bind(8, p.creationClub ? 1 : 0)
            .bind(9, p.sourceMod)
            .bind(10, pathToUtf8(p.path))
            .bind(11, p.fileSize)
            .bind(12, static_cast<double>(p.header.version))
            .bind(13, p.header.author)
            .bind(14, p.header.description)
            .bind(15, p.header.record.flags)
            .bind(16, p.header.isLocalized() ? 1 : 0)
            .bind(17, p.recordCount)
            .bind(18, p.newRecords)
            .bind(19, p.overrides)
            .bind(20, p.injected)
            .bind(21, p.itmCount)
            .bind(22, p.deletedCount)
            .bind(23, p.conflictCount)
            .bind(24, p.hardConflictCount)
            .bind(25, static_cast<uint32_t>(p.header.masters.size()))
            .bind(26, pluginStatus(p))
            .bind(27, eligibilityName(p.eligibility))
            .bind(28, join(p.reasons, "; "))
            .bind(29, join(p.archives, ", "))
            .bind(30, recordTypesSummary(p));
        st.run();
        for (size_t i = 0; i < p.header.masters.size(); ++i) {
            PluginIndex m = i < p.masters.size() ? p.masters[i] : kInvalidPlugin;
            ms.bind(1, p.index).bind(2, static_cast<int>(i)).bind(3, p.header.masters[i].name);
            if (m == kInvalidPlugin)
                ms.bindNull(4).bind(5, 1);
            else
                ms.bind(4, m).bind(5, 0);
            ms.run();
        }
    }
}

void DatabaseWriter::updatePlugins(const Session& session)
{
    Transaction tx(db_);
    writePlugins(session);
    tx.commit();
}

void DatabaseWriter::writeScan(const Session& session, ProgressSink& progress)
{
    progress.stage("Writing scan results");
    Transaction tx(db_);
    db_.exec("DELETE FROM Records; DELETE FROM OverrideChains; DELETE FROM Archives; DELETE FROM Assets; "
             "DELETE FROM AssetSources; DELETE FROM NameReferences; DELETE FROM Forms;");
    writePlugins(session);

    // Records: everything compilable plus every instance in an override chain.
    auto rec = db_.prepare("INSERT INTO Records(plugin_id, signature, origin_plugin_id, object_id, form_id, raw_form_id, "
                           "editor_id, flags, data_size, file_offset, kind, is_winner) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    auto chain = db_.prepare(
        "INSERT INTO OverrideChains(origin_plugin_id, object_id, position, plugin_id, record_id) VALUES(?,?,?,?,?)");
    auto form = db_.prepare("INSERT OR REPLACE INTO Forms(origin_plugin_id, object_id, form_id, signature, editor_id, "
                            "winner_plugin_id) VALUES(?,?,?,?,?,?)");
    size_t n = 0;
    for (const auto& g : session.groups) {
        auto inst = session.instances(g);
        bool compilable = session.defs.isCompilable(inst[0].signature);
        bool overridden = inst.size() > 1;
        if ((++n & 0xFFFF) == 0)
            progress.progress(n, session.groups.size(), "records");
        if (!compilable && !overridden)
            continue;
        const auto& first = inst[0];
        std::string fid = session.loadOrderFormId(first.key);
        std::string edid;
        for (size_t i = inst.size(); i-- > 0;)
            if (inst[i].editorId) {
                edid = session.strings.get(inst[i].editorId);
                break;
            }
        form.bind(1, first.key.origin)
            .bind(2, first.key.objectId)
            .bind(3, fid)
            .bind(4, first.signature.str())
            .bind(5, edid)
            .bind(6, inst.back().plugin);
        form.run();
        for (size_t i = 0; i < inst.size(); ++i) {
            const auto& r = inst[i];
            const char* kind = r.plugin == r.key.origin ? "New" : (i == 0 ? "Injected" : "Override");
            rec.bind(1, r.plugin)
                .bind(2, r.signature.str())
                .bind(3, r.key.origin)
                .bind(4, r.key.objectId)
                .bind(5, fid)
                .bind(6, r.rawFormId)
                .bind(7, session.strings.get(r.editorId))
                .bind(8, r.flags)
                .bind(9, r.dataSize)
                .bind(10, r.offset)
                .bind(11, kind)
                .bind(12, i + 1 == inst.size() ? 1 : 0);
            rec.run();
            int64_t id = db_.lastInsertId();
            if (overridden) {
                chain.bind(1, r.key.origin)
                    .bind(2, r.key.objectId)
                    .bind(3, static_cast<int>(i))
                    .bind(4, r.plugin)
                    .bind(5, id);
                chain.run();
            }
        }
    }

    // Archives and assets.
    auto ar = db_.prepare("INSERT INTO Archives(id, name, mod_name, owner_plugin_id, load_sequence, type, version, "
                          "file_count, path) VALUES(?,?,?,?,?,?,?,?,?)");
    const auto& assets = session.assets;
    for (uint32_t i = 0; i < assets.archives.size(); ++i) {
        const auto& a = assets.archives[i];
        ar.bind(1, i).bind(2, a.name).bind(3, a.modName);
        if (a.owner == kInvalidPlugin)
            ar.bindNull(4);
        else
            ar.bind(4, a.owner);
        ar.bind(5, a.loadSequence)
            .bind(6, a.archive ? (a.archive->type() == Ba2Type::General ? "General" : "Textures") : "Unreadable")
            .bind(7, a.archive ? a.archive->version() : 0u)
            .bind(8, a.archive ? static_cast<uint64_t>(a.archive->entries().size()) : uint64_t{0})
            .bind(9, a.archive ? pathToUtf8(a.archive->path()) : std::string());
        ar.run();
    }

    auto as = db_.prepare("INSERT INTO Assets(id, path, asset_type, source_count, winner_mod, winner_kind, "
                          "winner_archive, winner_size, winner_physical, reason) VALUES(?,?,?,?,?,?,?,?,?,?)");
    auto src = db_.prepare("INSERT INTO AssetSources(asset_id, position, mod_name, kind, archive, physical, size, "
                           "priority, is_winner) VALUES(?,?,?,?,?,?,?,?,?)");
    auto physical = [&](const AssetSource& s) -> std::string {
        if (s.archive != kLooseFile) {
            const auto& a = assets.archives[s.archive];
            return a.archive ? pathToUtf8(a.archive->path()) : a.name;
        }
        return pathToUtf8(session.vfs.physicalPath(session.vfs.files()[s.entry], s.slot));
    };
    for (uint32_t i = 0; i < assets.entries.size(); ++i) {
        const auto& e = assets.entries[i];
        const auto& w = e.winner();
        bool loose = w.archive == kLooseFile;
        std::string reason;
        if (e.sources.size() == 1)
            reason = "Only source";
        else if (loose)
            reason = e.sources[e.sources.size() - 2].archive == kLooseFile
                         ? "Highest priority mod providing a loose file"
                         : "Loose files override archives";
        else
            reason = "Archive loaded last";
        as.bind(1, i)
            .bind(2, assets.path(e))
            .bind(3, assetTypeName(e.type))
            .bind(4, static_cast<uint32_t>(e.sources.size()))
            .bind(5, assets.modName(w))
            .bind(6, loose ? "Loose" : "Archive")
            .bind(7, loose ? std::string() : assets.archives[w.archive].name)
            .bind(8, w.size)
            .bind(9, physical(w))
            .bind(10, reason);
        as.run();
        if (e.sources.size() > 1) {
            for (size_t s = 0; s < e.sources.size(); ++s) {
                const auto& v = e.sources[s];
                bool l = v.archive == kLooseFile;
                src.bind(1, i)
                    .bind(2, static_cast<int>(s))
                    .bind(3, assets.modName(v))
                    .bind(4, l ? "Loose" : "Archive")
                    .bind(5, l ? std::string() : assets.archives[v.archive].name)
                    .bind(6, physical(v))
                    .bind(7, v.size)
                    .bind(8, v.priority)
                    .bind(9, s + 1 == e.sources.size() ? 1 : 0);
                src.run();
            }
        }
        if ((i & 0xFFFF) == 0)
            progress.progress(i, assets.entries.size(), "assets");
    }

    auto nr = db_.prepare("INSERT INTO NameReferences(plugin_id, asset_path, source) VALUES(?,?,?)");
    for (const auto& r : session.nameReferences)
        nr.bind(1, r.plugin).bind(2, r.assetPath).bind(3, r.source).run();

    tx.commit();
    setMeta("game_path", pathToUtf8(session.install.gamePath));
    setMeta("install_kind", installKindName(session.install.config.kind));
    setMeta("game_version", session.install.gameVersion);
    setMeta("runtime_profile", session.defs.profileName());
    setMeta("mods_enabled", std::to_string(session.install.enabledModCount()));
    setMeta("total_records", std::to_string(session.records.size()));
    setMeta("unique_forms", std::to_string(session.groups.size()));
    setMeta("archive_count", std::to_string(assets.archives.size()));
    setMeta("asset_count", std::to_string(assets.entries.size()));
    setMeta("loose_file_count", std::to_string(assets.looseFileCount()));
    setMeta("asset_conflicts", std::to_string(assets.conflictCount()));
}

void DatabaseWriter::writeAnalysis(const Session& session, const ConflictReport& conflicts, const DependencyReport& deps,
                                   ProgressSink& progress)
{
    progress.stage("Writing analysis results");
    Transaction tx(db_);
    db_.exec("DELETE FROM Conflicts; DELETE FROM ConflictFields; DELETE FROM ConflictPlugins; "
             "DELETE FROM Dependencies; DELETE FROM DependencyIssues;");
    writePlugins(session);

    auto c = db_.prepare("INSERT INTO Conflicts(origin_plugin_id, object_id, form_id, signature, editor_id, "
                         "conflict_type, type_name, severity, winner_plugin_id, chain_length, chain, "
                         "winner_loses_changes, summary) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto cf = db_.prepare("INSERT INTO ConflictFields(conflict_id, field_index, field, hard, position, plugin_id, "
                          "value, status) VALUES(?,?,?,?,?,?,?,?)");
    auto cp = db_.prepare("INSERT INTO ConflictPlugins(conflict_id, plugin_id) VALUES(?,?)");
    size_t n = 0;
    for (const auto& e : conflicts.entries) {
        std::vector<std::string> names;
        for (auto p : e.chain)
            names.push_back(session.pluginName(p));
        c.bind(1, e.key.origin)
            .bind(2, e.key.objectId)
            .bind(3, session.loadOrderFormId(e.key))
            .bind(4, e.sig.str())
            .bind(5, e.editorId)
            .bind(6, static_cast<int>(e.type))
            .bind(7, conflictTypeName(e.type))
            .bind(8, conflictSeverityName(e.severity))
            .bind(9, e.winner)
            .bind(10, static_cast<uint32_t>(e.chain.size()))
            .bind(11, join(names, " > "))
            .bind(12, e.winnerLosesChanges ? 1 : 0)
            .bind(13, e.summary);
        c.run();
        int64_t id = db_.lastInsertId();
        std::set<PluginIndex> uniq(e.chain.begin(), e.chain.end());
        for (auto p : uniq)
            cp.bind(1, id).bind(2, p).run();
        for (size_t fi = 0; fi < e.fields.size(); ++fi) {
            const auto& f = e.fields[fi];
            cf.bind(1, id).bind(2, static_cast<int>(fi)).bind(3, f.label).bind(4, f.hard ? 1 : 0).bind(5, -1).bind(6, -1);
            cf.bind(7, f.result).bind(8, "Result");
            cf.run();
            for (size_t pos = 0; pos < f.cells.size(); ++pos) {
                cf.bind(1, id)
                    .bind(2, static_cast<int>(fi))
                    .bind(3, f.label)
                    .bind(4, f.hard ? 1 : 0)
                    .bind(5, static_cast<int>(pos))
                    .bind(6, f.cells[pos].plugin)
                    .bind(7, f.cells[pos].value)
                    .bind(8, cellStatusName(f.cells[pos].status));
                cf.run();
            }
        }
        if ((++n & 0xFFF) == 0)
            progress.progress(n, conflicts.entries.size(), "conflicts");
    }

    auto d = db_.prepare(
        "INSERT INTO Dependencies(from_origin, from_object, to_origin, to_object, via) VALUES(?,?,?,?,?)");
    for (const auto& e : session.recordEdges) {
        d.bind(1, e.from.origin).bind(2, e.from.objectId);
        if (e.to.valid())
            d.bind(3, e.to.origin);
        else
            d.bind(3, -1);
        d.bind(4, e.to.objectId).bind(5, e.via.str());
        d.run();
    }
    auto di = db_.prepare("INSERT INTO DependencyIssues(severity, category, plugin_id, form_id, message) VALUES(?,?,?,?,?)");
    for (const auto& is : deps.issues) {
        di.bind(1, severityName(is.severity)).bind(2, is.category);
        if (is.plugin == kInvalidPlugin)
            di.bindNull(3);
        else
            di.bind(3, is.plugin);
        di.bind(4, is.form.valid() ? session.loadOrderFormId(is.form) : std::string()).bind(5, is.message);
        di.run();
    }
    db_.exec("DELETE FROM EligibilityBlockers;");
    auto eb = db_.prepare(
        "INSERT INTO EligibilityBlockers(signature, name, plugins, records, sole_blocker, unlocks) VALUES(?,?,?,?,?,?)");
    for (const auto& b : deps.blockers)
        eb.bind(1, b.sig.str())
            .bind(2, b.name)
            .bind(3, static_cast<uint64_t>(b.plugins))
            .bind(4, static_cast<uint64_t>(b.records))
            .bind(5, static_cast<uint64_t>(b.soleBlocker))
            .bind(6, static_cast<uint64_t>(b.unlocks))
            .run();
    tx.commit();
    setMeta("blocked_by_types", std::to_string(deps.blockedByTypes));
    setMeta("unlocked_by_all_types", std::to_string(deps.unlockedByAll));
    setMeta("conflicts_type1", std::to_string(conflicts.byType[1]));
    setMeta("conflicts_type2", std::to_string(conflicts.byType[2]));
    setMeta("conflicts_type3", std::to_string(conflicts.byType[3]));
    setMeta("conflicts_total", std::to_string(conflicts.conflictCount()));
    setMeta("itm_chains", std::to_string(conflicts.identicalOverrides));
    setMeta("eligible_plugins", std::to_string(deps.eligiblePlugins));
    setMeta("eligible_records", std::to_string(deps.eligibleRecords));
}

void DatabaseWriter::writeCompilation(const Session& session, const CompiledOutput& compiled)
{
    Transaction tx(db_);
    db_.exec("DELETE FROM CompilationGroups; DELETE FROM FormMappings; DELETE FROM GeneratedRecords;");
    auto g = db_.prepare("INSERT INTO CompilationGroups(output_index, output_name, record_count, new_records, overrides, "
                         "size_bytes, light, strategy, masters) VALUES(?,?,?,?,?,?,?,?,?)");
    for (const auto& op : compiled.outputs) {
        g.bind(1, op.index)
            .bind(2, op.name)
            .bind(3, static_cast<uint32_t>(op.records.size()))
            .bind(4, op.newRecords)
            .bind(5, op.overrides)
            .bind(6, op.bytes)
            .bind(7, op.light ? 1 : 0)
            .bind(8, allocationStrategyName(compiled.options.strategy))
            .bind(9, join(op.masterNames, ", "));
        g.run();
    }
    auto m = db_.prepare("INSERT INTO FormMappings(source_plugin, source_plugin_id, source_object_id, output_plugin, "
                         "output_index, output_object_id, signature, editor_id) VALUES(?,?,?,?,?,?,?,?)");
    for (const auto& fm : compiled.mappings) {
        m.bind(1, fm.sourcePlugin)
            .bind(2, fm.source.origin)
            .bind(3, fm.sourceObjectId)
            .bind(4, fm.outputPlugin)
            .bind(5, fm.output)
            .bind(6, fm.objectId)
            .bind(7, fm.sig.str())
            .bind(8, fm.editorId);
        m.run();
    }
    auto r = db_.prepare("INSERT INTO GeneratedRecords(output_index, output_plugin, form_id, signature, editor_id, "
                         "origin_plugin_id, object_id, is_new, merged, winner_plugin_id, contributors, size_bytes) "
                         "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    for (const auto& e : compiled.records) {
        std::vector<std::string> contrib;
        for (auto p : e.contributors)
            contrib.push_back(session.pluginName(p));
        r.bind(1, e.output)
            .bind(2, compiled.outputName(e.output))
            .bind(3, formatFormId(e.rawFormId))
            .bind(4, e.sig.str())
            .bind(5, e.editorId)
            .bind(6, e.key.origin)
            .bind(7, e.key.objectId)
            .bind(8, e.isNew ? 1 : 0)
            .bind(9, e.merged ? 1 : 0)
            .bind(10, e.winner)
            .bind(11, join(contrib, ", "))
            .bind(12, e.size);
        r.run();
    }
    tx.commit();
    size_t used = 0;
    for (const auto& op : compiled.outputs)
        used += op.patch || op.records.empty() ? 0 : 1;
    std::vector<std::string> patches;
    for (const auto& op : compiled.outputs)
        if (op.patch)
            patches.push_back(op.name);
    setMeta("patch_name", join(patches, ","));
    setMeta("patched_records", std::to_string(compiled.patchedRecords));
    setMeta("compiled_plugins", std::to_string(compiled.compiledPlugins.size()));
    setMeta("merges_reused", std::to_string(compiled.mergesReused));
    setMeta("merges_computed", std::to_string(compiled.mergesComputed));
    setMeta("compiled_records", std::to_string(compiled.records.size()));
    setMeta("output_plugins", std::to_string(used));
}

void DatabaseWriter::writeValidation(const std::string& stage, const ValidationReport& report, bool clearPrevious)
{
    Transaction tx(db_);
    // Definition checks run separately and stay until the next definition check.
    if (stage == "definitions")
        db_.exec("DELETE FROM ValidationResults WHERE stage = 'definitions';");
    else if (clearPrevious)
        db_.exec("DELETE FROM ValidationResults WHERE stage <> 'definitions';");
    auto st = db_.prepare(
        "INSERT INTO ValidationResults(stage, severity, category, message, plugin, form_id) VALUES(?,?,?,?,?,?)");
    for (const auto& is : report.issues)
        st.bind(1, stage)
            .bind(2, severityName(is.severity))
            .bind(3, is.category)
            .bind(4, is.message)
            .bind(5, is.plugin)
            .bind(6, is.formId)
            .run();
    tx.commit();
    setMeta("validation_" + stage + "_checks", std::to_string(report.checks));
    setMeta("validation_" + stage + "_passed", std::to_string(report.passed));
    setMeta("validation_" + stage + "_warnings", std::to_string(report.warnings));
    setMeta("validation_" + stage + "_errors", std::to_string(report.errors));
}

void DatabaseWriter::writeAssetTrace(const CompiledOutput& compiled, const AssetTrace& trace)
{
    Transaction tx(db_);
    db_.exec("DELETE FROM RecordAssets;");
    auto st = db_.prepare("INSERT INTO RecordAssets(output_plugin, form_id, editor_id, asset_path, role, via, found, "
                          "source_kind, origin_mod, archive, archive_owner, physical, needs_registration) "
                          "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (const auto& a : trace.assets) {
        const auto& e = compiled.records[a.record];
        st.bind(1, compiled.outputName(e.output))
            .bind(2, formatFormId(e.rawFormId))
            .bind(3, e.editorId)
            .bind(4, a.path)
            .bind(5, a.role)
            .bind(6, a.via)
            .bind(7, a.found ? 1 : 0)
            .bind(8, !a.found ? "Missing" : a.loose ? "Loose" : "Archive")
            .bind(9, a.mod)
            .bind(10, a.archive)
            .bind(11, a.archiveOwner)
            .bind(12, a.physical)
            .bind(13, a.needsRegistration ? 1 : 0);
        st.run();
    }
    tx.commit();
    setMeta("traced_assets", std::to_string(trace.assets.size()));
    setMeta("missing_assets", std::to_string(trace.missing));
}

void DatabaseWriter::saveSnapshot(const std::string& kind, const InstallSnapshot& snapshot)
{
    db_.prepare("INSERT OR REPLACE INTO Snapshots(kind, created, data) VALUES(?,?,?)")
        .bind(1, kind)
        .bind(2, static_cast<int64_t>(snapshot.created))
        .bind(3, snapshot.toJson().dump(0))
        .run();
}

std::optional<InstallSnapshot> DatabaseWriter::loadSnapshot(const std::string& kind)
{
    auto st = db_.prepare("SELECT data FROM Snapshots WHERE kind = ?");
    st.bind(1, kind);
    if (!st.step())
        return std::nullopt;
    try {
        return InstallSnapshot::fromJson(json::Value::parse(st.columnText(0)));
    } catch (const std::exception& e) {
        spdlog::warn("Ignoring unreadable {} snapshot: {}", kind, e.what());
        return std::nullopt;
    }
}

void DatabaseWriter::writeChanges(const ChangeReport& r)
{
    {
        Transaction tx(db_);
        db_.exec("DELETE FROM Changes;");
        auto st = db_.prepare("INSERT INTO Changes(category, kind, name, detail, affects_build) VALUES(?,?,?,?,?)");
        for (const auto& i : r.items)
            st.bind(1, i.category).bind(2, i.kind).bind(3, i.name).bind(4, i.detail).bind(5, i.affectsBuild ? 1 : 0).run();
        tx.commit();
    }
    setMeta("build_status", buildStatusName(r.status));
    setMeta("build_checked", std::to_string(r.checkedTime));
    setMeta("build_reference", std::to_string(r.referenceTime));
    setMeta("changes_new_mods", std::to_string(r.newMods));
    setMeta("changes_removed_mods", std::to_string(r.removedMods));
    setMeta("changes_updated_mods", std::to_string(r.updatedMods));
    setMeta("changes_added_plugins", std::to_string(r.addedPlugins));
    setMeta("changes_removed_plugins", std::to_string(r.removedPlugins));
    setMeta("changes_updated_plugins", std::to_string(r.updatedPlugins));
    setMeta("changes_archives", std::to_string(r.archiveChanges));
    setMeta("changes_load_order", std::to_string(r.loadOrderChanges));
    setMeta("changes_affected_types", join(r.affectedRecordTypes, " "));
    setMeta("changes_unaffected_types", join(r.unaffectedRecordTypes, " "));
    setMeta("changes_reasons", join(r.reasons, "\n"));
}

std::vector<std::string> DatabaseWriter::resolutionStrings()
{
    std::vector<std::string> out;
    auto st = db_.prepare("SELECT origin_plugin, object_id, winner_plugin FROM UserResolutions");
    while (st.step())
        out.push_back(st.columnText(0) + ":" + std::to_string(st.columnInt(1)) + "=" + st.columnText(2));
    return out;
}

void DatabaseWriter::saveResolution(const std::string& originPlugin, uint32_t objectId, const std::string& winnerPlugin)
{
    db_.prepare("INSERT OR REPLACE INTO UserResolutions(origin_plugin, object_id, winner_plugin) VALUES(?,?,?)")
        .bind(1, originPlugin)
        .bind(2, objectId)
        .bind(3, winnerPlugin)
        .run();
}

void DatabaseWriter::clearResolution(const std::string& originPlugin, uint32_t objectId)
{
    db_.prepare("DELETE FROM UserResolutions WHERE origin_plugin = ? AND object_id = ?")
        .bind(1, originPlugin)
        .bind(2, objectId)
        .run();
}

std::unordered_map<uint64_t, PluginIndex> DatabaseWriter::loadResolutions(const Session& session)
{
    std::unordered_map<uint64_t, PluginIndex> out;
    auto st = db_.prepare("SELECT origin_plugin, object_id, winner_plugin FROM UserResolutions");
    while (st.step()) {
        auto origin = session.findPlugin(st.columnText(0));
        auto winner = session.findPlugin(st.columnText(2));
        if (!origin || !winner)
            continue;
        FormKey k{origin->index, static_cast<uint32_t>(st.columnInt(1))};
        out[k.packed()] = winner->index;
    }
    return out;
}

} // namespace fo4mx::db
