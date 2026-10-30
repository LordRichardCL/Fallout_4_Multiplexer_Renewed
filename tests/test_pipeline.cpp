#include "Fixtures.hpp"
#include "Test.hpp"

#include "ba2/Ba2Archive.hpp"
#include "db/Database.hpp"
#include "engine/Pipeline.hpp"
#include "engine/ReferenceScanner.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"
#include "papyrus/Pex.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

using namespace fo4mx;
using namespace fo4mx::fixtures;

namespace {

constexpr uint32_t ESM = RecordFlags::Master;

// Builds a small but complete install:
//   Fallout4.esm (localized) <- ModA.esp <- ModB.esp, plus ModC.esp (contains a CELL: ineligible).
std::filesystem::path buildInstall(const std::string& name)
{
    auto root = tempDir(name);
    auto data = root / "Game" / "Data";
    std::filesystem::create_directories(data);

    writePlugin(data / "Fallout4.esm", {},
                {
                    record("KYWD", 0x00000800, {zs("EDID", "BaseKeyword"), u32("FULL", 1)}),
                    record("KYWD", 0x00000801, {zs("EDID", "BaseKeyword2")}),
                    record("AMMO", 0x00000900,
                           {zs("EDID", "Ammo10mm"), u32("FULL", 2), raw("DNAM", std::vector<uint8_t>(16, 0))}),
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), u32("FULL", 3), u32("KSIZ", 2), formIds("KWDA", {0x800, 0x801}),
                            weaponData(0x900, 0x11)}),
                    record("FLST", 0x00001100, {zs("EDID", "WeaponList"), formIds("LNAM", {0x1000})}),
                    record("LVLI", 0x00001200,
                           {zs("EDID", "LootPistols"), raw("LLCT", {1}), lvlo(1, 0x1000, 1)}),
                    record("PERK", 0x00001300, {zs("EDID", "Gunsmith"), u32("FULL", 4), raw("DATA", {0, 1, 1, 1, 0})}),
                },
                ESM | RecordFlags::Localized);
    writeBytes(data / "Strings" / "Fallout4_en.STRINGS",
               stringTable({{1, "Base Keyword"}, {2, "10mm Round"}, {3, "10mm Pistol"}, {4, "Gun Nut"}}, false));

    writePlugin(data / "ModA.esp", {"Fallout4.esm"},
                {
                    record("KYWD", 0x01000800, {zs("EDID", "ModA_Keyword")}),
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), zs("FULL", "Pistol (Mod A)"), u32("KSIZ", 3),
                            formIds("KWDA", {0x800, 0x801, 0x01000800}), weaponData(0x900, 0x11)}),
                    record("WEAP", 0x01000801,
                           {zs("EDID", "ModA_Gun"), vmad({0x01000800}, {0x00000800, 0x01000800}), zs("FULL", "Mod A Gun"),
                            zs("MODL", "ModA\\Gun.nif"), u32("KSIZ", 1), formIds("KWDA", {0x01000800}),
                            weaponData(0x00000900, 0x22)}),
                    record("COBJ", 0x01000802,
                           {zs("EDID", "co_ModA_Gun"), components({{0x00000900, 5}}), hasPerk(0x00001300),
                            u32("CNAM", 0x01000801), u32("BNAM", 0x00000800), raw("INTV", {1, 0, 0, 0})}),
                });
    // ModA's archive: loads with the plugin and must be registered after compilation.
    writeGeneralBa2(data / "ModA - Main.ba2", {{"meshes\\moda\\gun.nif", std::vector<uint8_t>(64, 7)}}, true);

    writePlugin(data / "ModB.esp", {"Fallout4.esm", "ModA.esp"},
                {
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), zs("FULL", "10mm Pistol"), zs("MODL", "Weapons\\Missing.nif"),
                            u32("KSIZ", 1), formIds("KWDA", {0x800}), weaponData(0x900, 0x33)}),
                    record("FLST", 0x00001100, {zs("EDID", "WeaponList"), formIds("LNAM", {0x1000}),
                                                formIds("LNAM", {0x01000801})}),
                    record("LVLI", 0x00001200,
                           {zs("EDID", "LootPistols"), raw("LLCT", {2}), lvlo(1, 0x1000, 1), lvlo(5, 0x01000801, 1)}),
                });

    writePlugin(data / "ModC.esp", {"Fallout4.esm"},
                {
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), zs("FULL", "Pistol (Mod C)"), u32("KSIZ", 2),
                            formIds("KWDA", {0x800, 0x801}), weaponData(0x900, 0x11)}),
                    record("CELL", 0x01000800, {zs("EDID", "ModCCell")}),
                });

    writeFile(root / "plugins.txt", "# test\n*ModA.esp\n*ModB.esp\n*ModC.esp\n");
    std::filesystem::create_directories(root / "ini");
    return root;
}

PipelineConfig configFor(const std::filesystem::path& root)
{
    PipelineConfig cfg;
    cfg.install.kind = InstallKind::Standalone;
    cfg.install.gamePath = root / "Game";
    cfg.install.pluginsFile = root / "plugins.txt";
    cfg.install.iniDirectory = root / "ini";
    cfg.compile.outputCount = 10;
    cfg.exportOptions.outputDirectory = root / "Output";
    cfg.reportDirectory = root / "Reports";
    cfg.databasePath = root / "test.sqlite";
    cfg.scan.threads = 2;
    return cfg;
}

// Reads compiled output plugins back and resolves FormIDs to (plugin, object id).
struct OutputSet {
    struct Rec {
        std::string plugin;
        Record record;
        std::vector<std::string> masters;
    };
    std::vector<Rec> records;

    explicit OutputSet(const std::vector<std::filesystem::path>& files)
    {
        for (const auto& f : files) {
            auto file = PluginFile::load(f);
            std::vector<std::string> masters;
            for (const auto& m : file.header().masters)
                masters.push_back(m.name);
            std::string name = pathToUtf8(f.filename());
            file.forEachRecord([&](const RecordLocation& loc) {
                records.push_back({name, Record::parse(loc.header, loc.rawData), masters});
                return true;
            });
        }
    }

    const Rec* byEditorId(const std::string& edid) const
    {
        for (const auto& r : records)
            if (r.record.editorId() == edid)
                return &r;
        return nullptr;
    }

    std::pair<std::string, uint32_t> resolve(const Rec& r, uint32_t raw) const
    {
        uint32_t mi = raw >> 24;
        return {mi < r.masters.size() ? r.masters[mi] : r.plugin, raw & 0xFFFFFF};
    }

    std::pair<std::string, uint32_t> identity(const Rec& r) const { return resolve(r, r.record.header.formId); }
};

std::string text(const Subrecord* s)
{
    return s ? s->asZString() : std::string("<none>");
}

} // namespace

TEST(pipeline_end_to_end)
{
    auto root = buildInstall("pipeline");
    Pipeline p(configFor(root));
    p.scan();
    auto& s = p.session();

    CHECK_EQ(s.loadOrder.size(), size_t(4));
    CHECK_EQ(s.plugins[s.loadOrder[0]].name, std::string("Fallout4.esm"));
    CHECK_EQ(s.findPlugin("ModA.esp")->eligibility, Eligibility::Eligible);
    CHECK_EQ(s.findPlugin("ModB.esp")->eligibility, Eligibility::Eligible);
    CHECK_EQ(s.findPlugin("ModC.esp")->eligibility, Eligibility::Ineligible);
    CHECK_EQ(s.findPlugin("Fallout4.esm")->eligibility, Eligibility::Retained);
    CHECK(s.assets.find("meshes\\moda\\gun.nif") != nullptr);

    p.analyse();
    const auto* conflicts = p.conflicts();
    FormKey pistol{s.findPlugin("Fallout4.esm")->index, 0x1000};
    const auto* entry = conflicts->find(pistol);
    CHECK(entry != nullptr);
    CHECK_EQ(int(entry->type), int(ConflictType::HardConflict));
    CHECK_EQ(entry->chain.size(), size_t(4));
    bool fullContested = false, localizedResolved = false;
    for (const auto& f : entry->fields) {
        if (f.label == "FULL") {
            fullContested = f.hard;
            // The localized base resolves to text, and ModB's inline copy compares equal to it.
            localizedResolved = f.cells[0].value == "\"10mm Pistol\"" && f.cells[2].value == f.cells[0].value &&
                                f.result == "\"Pistol (Mod C)\"";
        }
    }
    CHECK(fullContested);
    CHECK(localizedResolved);
    for (const auto& f : entry->fields)
        if (f.label == "KWDA")
            // ModB has ModA as a master and drops both ModA's keyword and BaseKeyword2: compared with the
            // version it was made against (ModA's), both are deliberate removals.
            CHECK(f.result.find("ModA_Keyword") == std::string::npos && f.result.find("BaseKeyword2") == std::string::npos);

    CHECK(p.compile());
    const auto* compiled = p.compiledOutput();
    CHECK_EQ(compiled->compiledPlugins.size(), size_t(2));
    CHECK_EQ(compiled->mappings.size(), size_t(3)); // ModA keyword, gun, recipe
    CHECK(std::find(compiled->archivesToRegister.begin(), compiled->archivesToRegister.end(), "ModA - Main.ba2") !=
          compiled->archivesToRegister.end());
    CHECK_EQ(p.validation()->errors, uint64_t(0));

    auto result = p.exportOutputs();
    CHECK(!result.pluginFiles.empty());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    CHECK(std::filesystem::exists(result.formMap));
    CHECK(std::filesystem::exists(root / "Reports" / "ConflictReport.html"));
    CHECK(std::filesystem::exists(root / "Reports" / "ValidationReport.json"));

    OutputSet out(result.pluginFiles);
    const auto* kw = out.byEditorId("ModA_Keyword");
    const auto* gun = out.byEditorId("ModA_Gun");
    const auto* recipe = out.byEditorId("co_ModA_Gun");
    const auto* list = out.byEditorId("WeaponList");
    const auto* loot = out.byEditorId("LootPistols");
    const auto* pistolOut = out.byEditorId("Pistol10mm");
    CHECK(kw && gun && recipe && list && loot && pistolOut);
    CHECK(out.byEditorId("BaseKeyword") == nullptr); // untouched by compiled plugins

    auto kwId = out.identity(*kw);
    auto gunId = out.identity(*gun);
    CHECK(kwId.first.rfind("Dummy_Plugin_", 0) == 0);

    // References inside the new gun point at the new keyword.
    auto kwda = gun->record.find(Signature("KWDA"));
    CHECK(kwda && kwda->data.size() == 4);
    CHECK(out.resolve(*gun, loadLE<uint32_t>(kwda->data.data())) == kwId);
    auto dnam = gun->record.find(Signature("DNAM"));
    CHECK(out.resolve(*gun, loadLE<uint32_t>(dnam->data.data())) == std::make_pair(std::string("Fallout4.esm"), 0x900u));
    CHECK_EQ(int(dnam->data[10]), 0x22);

    // Script properties were rewritten too.
    auto layout = decodeLayout(RecordDefinitions::get(), Signature("WEAP"), gun->record.subrecords);
    size_t vmadToKeyword = 0, vmadToBase = 0;
    for (const auto& slot : layout.formIdSlots) {
        const auto& sr = gun->record.subrecords[slot.sub];
        if (sr.signature != Signature("VMAD"))
            continue;
        auto target = out.resolve(*gun, loadLE<uint32_t>(sr.data.data() + slot.offset));
        if (target == kwId)
            ++vmadToKeyword;
        if (target == std::make_pair(std::string("Fallout4.esm"), 0x800u))
            ++vmadToBase;
    }
    CHECK_EQ(vmadToKeyword, size_t(2));
    CHECK_EQ(vmadToBase, size_t(1));

    // Recipe: created object, workbench keyword, condition perk.
    CHECK(out.resolve(*recipe, loadLE<uint32_t>(recipe->record.find(Signature("CNAM"))->data.data())) == gunId);
    auto ctda = recipe->record.find(Signature("CTDA"));
    CHECK(out.resolve(*recipe, loadLE<uint32_t>(ctda->data.data() + 12)) ==
          std::make_pair(std::string("Fallout4.esm"), 0x1300u));

    // Form list override keeps the base entry and adds the compiled gun.
    CHECK(out.identity(*list) == std::make_pair(std::string("Fallout4.esm"), 0x1100u));
    std::vector<std::pair<std::string, uint32_t>> entries;
    for (const auto* lnam : list->record.findAll(Signature("LNAM")))
        entries.push_back(out.resolve(*list, loadLE<uint32_t>(lnam->data.data())));
    CHECK_EQ(entries.size(), size_t(2));
    CHECK(entries[1] == gunId);

    // Leveled list: count regenerated, compiled gun entry rewritten.
    CHECK_EQ(int(loot->record.find(Signature("LLCT"))->data[0]), 2);
    auto lvlos = loot->record.findAll(Signature("LVLO"));
    CHECK(out.resolve(*loot, loadLE<uint32_t>(lvlos[1]->data.data() + 4)) == gunId);

    // The contested pistol: merged over the retained ModC winner.
    CHECK(out.identity(*pistolOut) == std::make_pair(std::string("Fallout4.esm"), 0x1000u));
    CHECK_EQ(text(pistolOut->record.find(sig::FULL)), std::string("Pistol (Mod C)")); // last changer of a hard conflict
    auto pk = pistolOut->record.find(Signature("KWDA"));
    CHECK_EQ(pk->data.size(), size_t(4)); // base keyword only; ModB (made against ModA) removed the other two
    CHECK(out.resolve(*pistolOut, loadLE<uint32_t>(pk->data.data())) == std::make_pair(std::string("Fallout4.esm"), 0x800u));
    CHECK_EQ(int(pistolOut->record.find(Signature("KSIZ"))->data[0]), 1);
    CHECK_EQ(int(pistolOut->record.find(Signature("DNAM"))->data[10]), 0x33); // ModB's data edit kept

    // Database contains the analysis.
    auto* db = p.database();
    CHECK(db != nullptr);
    CHECK_EQ(*db->scalar("SELECT COUNT(*) FROM Plugins WHERE eligibility = 'Eligible'"), int64_t(2));
    CHECK(*db->scalar("SELECT COUNT(*) FROM Conflicts WHERE conflict_type = 3") >= 1);
    CHECK_EQ(*db->scalar("SELECT COUNT(*) FROM FormMappings"), int64_t(3));
    CHECK(*db->scalar("SELECT COUNT(*) FROM ConflictFields") > 0);

    // Recompiling reuses the previous FormIDs (save-game stability).
    std::map<std::string, std::pair<std::string, uint32_t>> first;
    for (const auto& m : compiled->mappings)
        first[m.editorId] = {m.outputPlugin, m.objectId};
    Pipeline again(configFor(root));
    again.scan();
    again.analyse();
    CHECK(again.compile());
    for (const auto& m : again.compiledOutput()->mappings)
        CHECK(first[m.editorId] == std::make_pair(m.outputPlugin, m.objectId));
}

TEST(pipeline_allocation_strategies_respect_dependencies)
{
    auto root = buildInstall("strategies");
    for (auto strategy : {AllocationStrategy::ByRecordType, AllocationStrategy::Balanced, AllocationStrategy::UserDefined}) {
        auto cfg = configFor(root);
        cfg.databasePath.clear();
        cfg.compile.strategy = strategy;
        cfg.compile.lightOutputsWhenPossible = true;
        cfg.compile.rules = {{"KYWD", 10}}; // forces keywords last: dependants must follow
        cfg.exportOptions.outputDirectory = root / ("Out" + std::to_string(int(strategy)));
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        CHECK(p.compile());
        const auto* c = p.compiledOutput();
        for (const auto& e : c->records)
            for (auto d : e.deps)
                CHECK(c->records[d].output <= e.output);
        auto result = p.exportOutputs();
        CHECK_EQ(p.validation()->errors, uint64_t(0));
        for (const auto& op : c->outputs)
            if (!op.records.empty())
                CHECK(op.light);
        if (strategy == AllocationStrategy::UserDefined) {
            OutputSet out(result.pluginFiles);
            CHECK_EQ(out.byEditorId("ModA_Keyword")->plugin, std::string("Dummy_Plugin_10.esp"));
        }
    }
}

TEST(pipeline_skips_overrides_identical_to_retained_state)
{
    auto root = buildInstall("itm");
    // ModD only carries an identical copy of a Fallout4.esm record (an ITM) plus one new record.
    writePlugin(root / "Game" / "Data" / "ModD.esp", {"Fallout4.esm"},
                {record("KYWD", 0x00000801, {zs("EDID", "BaseKeyword2")}),
                 record("KYWD", 0x01000800, {zs("EDID", "ModD_Keyword")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModD.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    const auto* c = p.compiledOutput();
    FormKey itm{p.session().findPlugin("Fallout4.esm")->index, 0x801};
    CHECK(c->indexByKey.count(itm) == 0); // not carried into the output
    CHECK(c->identicalToRetained >= 1);
    FormKey added{p.session().findPlugin("ModD.esp")->index, 0x800};
    CHECK(c->indexByKey.count(added) == 1);
    CHECK_EQ(p.validation()->errors, uint64_t(0));
}

TEST(pipeline_no_merge_and_exclusions)
{
    auto root = buildInstall("nomerge");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    cfg.compile.mergeCompatible = false;
    cfg.analysis.excludedPlugins = {"ModB.esp"};
    Pipeline p(cfg);
    p.scan();
    CHECK_EQ(p.session().findPlugin("ModB.esp")->eligibility, Eligibility::Retained);
    // ModB keeps ModA as a master, so ModA must stay too.
    CHECK_EQ(p.session().findPlugin("ModA.esp")->eligibility, Eligibility::Ineligible);
    p.analyse();
    CHECK(p.compile());
    CHECK(p.compiledOutput()->empty());
}

TEST(mo2_instance_and_virtual_file_system)
{
    auto root = tempDir("mo2");
    auto game = root / "Game";
    writePlugin(game / "Data" / "Fallout4.esm", {}, {record("KYWD", 0x800, {zs("EDID", "K")})}, ESM);
    writeFile(game / "Data" / "textures" / "a.dds", "base");
    auto inst = root / "MO2";
    writeFile(inst / "ModOrganizer.ini", "[General]\ngamePath=@ByteArray(" + pathToUtf8(game) +
                                             ")\nselected_profile=@ByteArray(Default)\n[Settings]\n");
    writeFile(inst / "profiles" / "Default" / "modlist.txt",
              "# This file was automatically generated by Mod Organizer.\n+ModHigh\n-Disabled Mod\n+ModLow\n"
              "*DLC: Far Harbor\n+Tools_separator\n");
    writeFile(inst / "profiles" / "Default" / "plugins.txt", "*ModLow.esp\n*ModHigh.esp\nUnused.esp\n");
    writePlugin(inst / "mods" / "ModLow" / "ModLow.esp", {"Fallout4.esm"}, {record("KYWD", 0x01000800, {zs("EDID", "L")})});
    writePlugin(inst / "mods" / "ModHigh" / "ModHigh.esp", {"Fallout4.esm"},
                {record("KYWD", 0x00000800, {zs("EDID", "K2")})});
    writeFile(inst / "mods" / "ModLow" / "textures" / "a.dds", "low");
    writeFile(inst / "mods" / "ModHigh" / "textures" / "a.dds", "high");
    writeFile(inst / "mods" / "Disabled Mod" / "textures" / "a.dds", "disabled");
    writeFile(inst / "overwrite" / "textures" / "b.dds", "ow");
    // A script in ModLow's archive refers to ModHigh.esp by name.
    BinaryWriter pex;
    pex.write<uint32_t>(0xFA57C0DE);
    pex.write<uint16_t>(0x0903);
    pex.write<uint16_t>(2);
    pex.write<uint64_t>(0);
    pex.writeWString("x.psc");
    pex.writeWString("u");
    pex.writeWString("m");
    pex.write<uint16_t>(1);
    pex.writeWString("ModHigh.esp");
    writeGeneralBa2(inst / "mods" / "ModLow" / "ModLow - Main.ba2", {{"scripts\\x.pex", pex.take()}}, false);

    PipelineConfig cfg;
    cfg.install.kind = InstallKind::ModOrganizer2;
    cfg.install.mo2Path = inst;
    cfg.install.iniDirectory = root / "ini";
    Pipeline p(cfg);
    p.scan();
    auto& s = p.session();
    CHECK_EQ(s.install.mods.size(), size_t(3));
    CHECK_EQ(s.install.enabledModCount(), size_t(2));
    const auto* a = s.vfs.find("textures\\a.dds");
    CHECK(a != nullptr);
    CHECK_EQ(a->sources.size(), size_t(3)); // Data, ModLow, ModHigh (disabled mod excluded)
    CHECK_EQ(s.vfs.winnerName(*a), std::string("ModHigh"));
    CHECK(s.vfs.find("textures\\b.dds") != nullptr);
    CHECK_EQ(s.findPlugin("ModLow.esp")->sourceMod, std::string("ModLow"));
    CHECK(s.findPlugin("ModLow.esp")->active);
    CHECK(s.assets.find("textures\\a.dds")->conflicting());
    CHECK(s.assets.find("scripts\\x.pex") != nullptr);
    CHECK_EQ(s.findPlugin("ModHigh.esp")->eligibility, Eligibility::Ineligible);
    CHECK_EQ(s.findPlugin("ModLow.esp")->eligibility, Eligibility::Eligible);
    // ModHigh overrides a Fallout4.esm record; ModLow injects nothing.
    CHECK_EQ(s.findPlugin("ModHigh.esp")->overrides, 1u);
}

namespace {

void writeModB(const std::filesystem::path& data, bool extraLoot)
{
    std::vector<Subrecord> loot = {zs("EDID", "LootPistols"), raw("LLCT", {static_cast<uint8_t>(extraLoot ? 3 : 2)}),
                                   lvlo(1, 0x1000, 1), lvlo(5, 0x01000801, 1)};
    if (extraLoot)
        loot.push_back(lvlo(10, 0x01000801, 2));
    writePlugin(data / "ModB.esp", {"Fallout4.esm", "ModA.esp"},
                {
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), zs("FULL", "10mm Pistol"), zs("MODL", "Weapons\\Missing.nif"),
                            u32("KSIZ", 1), formIds("KWDA", {0x800}), weaponData(0x900, 0x33)}),
                    record("FLST", 0x00001100, {zs("EDID", "WeaponList"), formIds("LNAM", {0x1000}),
                                                formIds("LNAM", {0x01000801})}),
                    record("LVLI", 0x00001200, std::move(loot)),
                });
}

} // namespace

TEST(incremental_rebuild_reuses_unchanged_work)
{
    auto root = buildInstall("incremental");
    {
        Pipeline p(configFor(root));
        p.scan();
        p.analyse();
        CHECK(p.compile());
        p.exportOutputs();
        CHECK(p.changes().status == BuildStatus::UpToDate);
    }
    size_t candidates = 0;
    {
        // Nothing changed: every stage restores its previous work.
        Pipeline p(configFor(root));
        p.scan();
        CHECK(p.changes().status == BuildStatus::UpToDate);
        for (auto pi : p.session().loadOrder)
            CHECK(p.session().plugins[pi].fromCache);
        p.analyse();
        CHECK_EQ(p.conflicts()->reusedGroups, p.conflicts()->analysedGroups);
        CHECK(p.compile());
        CHECK_EQ(p.compiledOutput()->mergesComputed, size_t(0));
        candidates = p.compiledOutput()->mergesReused;
        CHECK(candidates > 0);
        auto r = p.exportOutputs();
        CHECK(r.writtenOutputs.empty());
        CHECK_EQ(r.unchangedOutputs.size(), r.pluginFiles.size());
    }
    // One plugin changes: only the chains it takes part in are rebuilt.
    writeModB(root / "Game" / "Data", true);
    {
        Pipeline p(configFor(root));
        p.scan();
        CHECK(!p.session().findPlugin("ModB.esp")->fromCache);
        CHECK(p.session().findPlugin("ModA.esp")->fromCache);
        const auto& ch = p.changes();
        CHECK(ch.status == BuildStatus::RequiresRebuild);
        CHECK_EQ(ch.updatedPlugins, size_t(1));
        CHECK(std::find(ch.affectedRecordTypes.begin(), ch.affectedRecordTypes.end(), "LVLI") != ch.affectedRecordTypes.end());
        CHECK(std::find(ch.unaffectedRecordTypes.begin(), ch.unaffectedRecordTypes.end(), "COBJ") !=
              ch.unaffectedRecordTypes.end());
        p.analyse();
        CHECK(p.compile());
        const auto* c = p.compiledOutput();
        CHECK(c->mergesComputed > 0);
        CHECK(c->mergesReused > 0); // ModA-only chains were not rebuilt
        CHECK_EQ(c->mergesComputed + c->mergesReused, candidates);
        auto r = p.exportOutputs();
        CHECK_EQ(r.writtenOutputs.size(), size_t(1));
        OutputSet out(r.pluginFiles);
        CHECK_EQ(out.byEditorId("LootPistols")->record.findAll(Signature("LVLO")).size(), size_t(3));
    }
}

TEST(change_detection_reports_build_status)
{
    auto root = buildInstall("changes");
    auto cfg = configFor(root);
    {
        Pipeline p(cfg);
        p.scan();
        CHECK(p.changes().status == BuildStatus::NotBuilt);
        p.analyse();
        CHECK(p.compile());
        p.exportOutputs();
    }
    auto check = [&](PipelineConfig c) {
        Pipeline p(c);
        return p.checkForChanges();
    };
    CHECK(check(cfg).status == BuildStatus::UpToDate);

    // Asset-only change: the compiled records are still valid.
    writeFile(root / "Game" / "Data" / "textures" / "new" / "decal.dds", "texture");
    auto assetOnly = check(cfg);
    CHECK(assetOnly.status == BuildStatus::ChangesDetected);
    CHECK_EQ(assetOnly.updatedMods, size_t(1)); // the Data folder gained a file

    // Settings change requires a rebuild.
    auto changedSettings = cfg;
    changedSettings.compile.outputCount = 12;
    CHECK(check(changedSettings).status == BuildStatus::RequiresRebuild);

    // A new enabled plugin requires a rebuild.
    writePlugin(root / "Game" / "Data" / "NewWeapons.esp", {"Fallout4.esm"},
                {record("WEAP", 0x01000800, {zs("EDID", "NewGun")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*NewWeapons.esp\n");
    auto added = check(cfg);
    CHECK(added.status == BuildStatus::RequiresRebuild);
    CHECK_EQ(added.addedPlugins, size_t(1));

    // Deleting a generated plugin is detected too.
    std::filesystem::remove(root / "Output" / "Dummy_Plugin_01.esp");
    auto missing = check(cfg);
    CHECK(std::any_of(missing.items.begin(), missing.items.end(),
                      [](const ChangeItem& i) { return i.category == "Output" && i.kind == "Missing"; }));
}

TEST(asset_trace_keeps_original_sources)
{
    auto root = buildInstall("assettrace");
    Pipeline p(configFor(root));
    p.scan();
    p.analyse();
    CHECK(p.compile());
    const auto& trace = p.assetTrace();
    const TracedAsset* gun = nullptr;
    const TracedAsset* missing = nullptr;
    for (const auto& a : trace.assets) {
        if (a.path == "meshes\\moda\\gun.nif")
            gun = &a;
        if (a.path == "meshes\\weapons\\missing.nif")
            missing = &a;
    }
    CHECK(gun && gun->found && !gun->loose);
    CHECK_EQ(gun->archive, std::string("ModA - Main.ba2"));
    CHECK_EQ(gun->archiveOwner, std::string("ModA.esp"));
    CHECK(gun->needsRegistration); // ModA is compiled: its archive loads through the generated INI
    CHECK(missing && !missing->found);
    CHECK(std::any_of(p.validation()->issues.begin(), p.validation()->issues.end(),
                      [](const ValidationIssue& i) { return i.category == "Missing Assets"; }));
    CHECK(*p.database()->scalar("SELECT COUNT(*) FROM RecordAssets WHERE archive = 'ModA - Main.ba2'") >= 1);
    // Nothing was copied: the output folder holds plugins and text files only.
    auto r = p.exportOutputs();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(r.directory))
        CHECK(!iendsWith(pathToUtf8(entry.path()), ".nif") && !iendsWith(pathToUtf8(entry.path()), ".ba2"));
}

TEST(conflict_resolution_patch_merges_retained_chains)
{
    auto root = buildInstall("patch");
    auto data = root / "Game" / "Data";
    // Two plugins that stay in the load order (they contain cells) and edit the same records.
    writePlugin(data / "ModE.esp", {"Fallout4.esm"},
                {record("AMMO", 0x00000900, {zs("EDID", "Ammo10mm"), zs("FULL", "10mm Round (E)"),
                                             raw("DNAM", std::vector<uint8_t>(16, 0))}),
                 record("LVLI", 0x00001200,
                        {zs("EDID", "LootPistols"), raw("LLCT", {2}), lvlo(1, 0x1000, 1), lvlo(2, 0x900, 10)}),
                 record("CELL", 0x01000800, {zs("EDID", "ModECell")})});
    writePlugin(data / "ModF.esp", {"Fallout4.esm"},
                {record("AMMO", 0x00000900, {zs("EDID", "Ammo10mm"), zs("FULL", "10mm Round"),
                                             raw("DNAM", std::vector<uint8_t>(16, 7))}),
                 record("CELL", 0x01000800, {zs("EDID", "ModFCell")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModE.esp\n*ModF.esp\n");

    // Alongside compilation: chains with a compiled plugin go to the Dummy outputs,
    // chains of retained plugins only go to the patch.
    {
        auto cfg = configFor(root);
        cfg.databasePath.clear();
        cfg.compile.buildPatch = true;
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        CHECK(p.compile());
        const auto* c = p.compiledOutput();
        CHECK(!c->compiledPlugins.empty());
        const auto* patch = c->patchOutput();
        CHECK(patch != nullptr);
        CHECK(patch->light);
        CHECK_EQ(patch->newRecords, 0u);
        FormKey ammo{p.session().findPlugin("Fallout4.esm")->index, 0x900};
        FormKey loot{p.session().findPlugin("Fallout4.esm")->index, 0x1200};
        CHECK(c->indexByKey.count(ammo) == 1);
        CHECK(c->records[c->indexByKey.at(ammo)].patch);
        CHECK_EQ(c->records[c->indexByKey.at(ammo)].output, patch->index);
        CHECK(c->indexByKey.count(loot) == 1);
        CHECK(!c->records[c->indexByKey.at(loot)].patch); // ModB is compiled
        CHECK_EQ(p.validation()->errors, uint64_t(0));
    }

    // Patch only: nothing is compiled and every merged chain goes to the patch.
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    cfg.compile.patchOnly = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    const auto* c = p.compiledOutput();
    CHECK(c->compiledPlugins.empty());
    CHECK(c->mappings.empty());
    for (const auto& op : c->outputs)
        CHECK(op.patch || op.records.empty());
    CHECK(c->patchedRecords >= 2);
    CHECK_EQ(p.validation()->errors, uint64_t(0));

    auto result = p.exportOutputs();
    CHECK_EQ(result.pluginFiles.size(), size_t(1));
    CHECK_EQ(pathToUtf8(result.pluginFiles.at(0).filename()), std::string("Multiplexer_Patch.esp"));
    auto header = readPluginHeader(result.pluginFiles.at(0));
    CHECK(header.record.flags & RecordFlags::Light);

    OutputSet out(result.pluginFiles);
    // ModE's ammo name and ModF's ammo data are both kept.
    const auto* a = out.byEditorId("Ammo10mm");
    CHECK(a != nullptr);
    CHECK_EQ(text(a->record.find(Signature("FULL"))), std::string("10mm Round (E)"));
    CHECK_EQ(a->record.find(Signature("DNAM"))->data.at(0), uint8_t(7));
    CHECK(out.identity(*a) == std::make_pair(std::string("Fallout4.esm"), uint32_t(0x900)));
    // ModB's and ModE's leveled list additions are both kept.
    const auto* l = out.byEditorId("LootPistols");
    CHECK(l != nullptr);
    CHECK_EQ(l->record.findAll(Signature("LVLO")).size(), size_t(3));
    // Above the master limit the patch is split; every record still lands in exactly one patch.
    {
        auto small = configFor(root);
        small.databasePath.clear();
        small.compile.patchOnly = true;
        small.compile.patchMasterLimit = 2; // Fallout4.esm + one more
        Pipeline q(small);
        q.scan();
        q.analyse();
        CHECK(q.compile());
        const auto* split = q.compiledOutput();
        std::vector<std::string> names;
        size_t inPatches = 0;
        for (const auto& op : split->outputs)
            if (op.patch) {
                names.push_back(op.name);
                inPatches += op.records.size();
                CHECK(op.masterNames.size() <= 2);
                CHECK(op.light);
            }
        CHECK(names.size() >= 2);
        CHECK_EQ(names.at(0), std::string("Multiplexer_Patch.esp"));
        CHECK_EQ(names.at(1), std::string("Multiplexer_Patch_2.esp"));
        CHECK_EQ(inPatches, split->patchedRecords);
        CHECK_EQ(q.validation()->errors, uint64_t(0));
    }
    // Nothing was compiled, so nothing should be disabled.
    std::ifstream disable(result.disableList);
    std::string listed((std::istreambuf_iterator<char>(disable)), std::istreambuf_iterator<char>());
    CHECK(listed.find(".esp") == std::string::npos);
}

namespace {

// A subrecord of `size` zero bytes with FormIDs written at the given offsets.
Subrecord structWith(const char* sig, size_t size, const std::vector<std::pair<size_t, uint32_t>>& ids)
{
    std::vector<uint8_t> d(size, 0);
    for (auto [off, id] : ids)
        storeLE<uint32_t>(d.data() + off, id);
    return raw(sig, std::move(d));
}

} // namespace

TEST(tier1_records_compile_with_references_rewritten)
{
    auto root = buildInstall("tier1");
    auto data = root / "Game" / "Data";
    // A typical armour + effect mod: everything it needs is a tier 1 type.
    writePlugin(data / "ModT.esp", {"Fallout4.esm"},
                {
                    record("MSWP", 0x01000800,
                           {zs("EDID", "SuitSwap"), zs("BNAM", "Armor\\Suit.bgsm"), zs("SNAM", "Armor\\SuitRed.bgsm")}),
                    record("ARMA", 0x01000801,
                           {zs("EDID", "SuitAA"), raw("BOD2", {0, 0, 0, 0}), zs("MOD2", "Armor\\Suit_M.nif"),
                            u32("MO2S", 0x01000800), u32("MODL", 0x00000800)}),
                    record("ARMO", 0x01000802,
                           {zs("EDID", "Suit"), zs("FULL", "Suit"), raw("INDX", {0, 0}), u32("MODL", 0x01000801),
                            u32("KSIZ", 1), formIds("KWDA", {0x00000800})}),
                    record("OTFT", 0x01000803, {zs("EDID", "SuitOutfit"), formIds("INAM", {0x01000802, 0x00001000})}),
                    record("MGEF", 0x01000804,
                           {zs("EDID", "SuitEffect"), structWith("DATA", 152, {{8, 0x01000802}, {136, 0x00001300}})}),
                    record("ENCH", 0x01000805,
                           {zs("EDID", "SuitEnchant"), structWith("ENIT", 36, {{32, 0x01000808}}), u32("EFID", 0x01000804),
                            raw("EFIT", std::vector<uint8_t>(12, 0))}),
                    record("CONT", 0x01000806,
                           {zs("EDID", "SuitCrate"), u32("COCT", 1), components({{0x01000802, 2}}, "CNTO")}),
                    record("GLOB", 0x01000807, {zs("EDID", "SuitCount"), raw("FNAM", {'s'}), u32("FLTV", 0)}),
                    record("FLST", 0x01000808, {zs("EDID", "SuitRestrictions"), u32("LNAM", 0x00001000)}),
                });
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModT.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    CHECK_EQ(p.session().findPlugin("ModT.esp")->eligibility, Eligibility::Eligible);
    p.analyse();
    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    auto result = p.exportOutputs();
    OutputSet out(result.pluginFiles);

    auto rec = [&](const char* edid) {
        const auto* r = out.byEditorId(edid);
        CHECK(r != nullptr);
        return r;
    };
    auto refAt = [&](const OutputSet::Rec* r, const char* sig, size_t offset) {
        const auto* s = r->record.find(Signature::fromString(sig));
        CHECK(s != nullptr);
        return out.resolve(*r, loadLE<uint32_t>(s->data.data() + offset));
    };
    const auto* swap = rec("SuitSwap");
    const auto* addon = rec("SuitAA");
    const auto* armor = rec("Suit");
    const auto* effect = rec("SuitEffect");
    const auto* list = rec("SuitRestrictions");
    // Armour -> armour addon -> material swap, all moved to the outputs.
    CHECK(refAt(armor, "MODL", 0) == out.identity(*addon));
    CHECK(refAt(addon, "MO2S", 0) == out.identity(*swap));
    CHECK(refAt(addon, "MODL", 0) == std::make_pair(std::string("Fallout4.esm"), uint32_t(0x800)));
    CHECK(refAt(rec("SuitOutfit"), "INAM", 0) == out.identity(*armor));
    CHECK(refAt(rec("SuitOutfit"), "INAM", 4) == std::make_pair(std::string("Fallout4.esm"), uint32_t(0x1000)));
    // Magic effect DATA slots: associated item (new) and perk (base game).
    CHECK(refAt(effect, "DATA", 8) == out.identity(*armor));
    CHECK(refAt(effect, "DATA", 136) == std::make_pair(std::string("Fallout4.esm"), uint32_t(0x1300)));
    // Enchantment: worn restrictions in ENIT and its effect.
    CHECK(refAt(rec("SuitEnchant"), "ENIT", 32) == out.identity(*list));
    CHECK(refAt(rec("SuitEnchant"), "EFID", 0) == out.identity(*effect));
    // Container item and regenerated count.
    CHECK(refAt(rec("SuitCrate"), "CNTO", 0) == out.identity(*armor));
    CHECK_EQ(loadLE<uint32_t>(rec("SuitCrate")->record.find(Signature("COCT"))->data.data()), uint32_t(1));
    // Paths stay as they were: the assets are not copied.
    CHECK_EQ(text(swap->record.find(Signature("SNAM"))), std::string("Armor\\SuitRed.bgsm"));
}

TEST(undescribed_formid_with_shifted_master_is_isolated)
{
    auto root = buildInstall("softref");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModE.esp", {"Fallout4.esm"}, {record("CELL", 0x01000800, {zs("EDID", "ModECell")})});
    // ModG keeps ModE and ModC (both retained) as masters 1 and 2, and refers to ModC's cell
    // from a subrecord the definitions do not describe.
    writePlugin(data / "ModG.esp", {"Fallout4.esm", "ModE.esp", "ModC.esp"},
                {record("GLOB", 0x03000800,
                        {zs("EDID", "ModG_Global"), raw("FNAM", {'f'}), u32("FLTV", 0), u32("UNKN", 0x02000800)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModE.esp\n*ModG.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    CHECK_EQ(p.session().findPlugin("ModG.esp")->eligibility, Eligibility::Eligible);
    p.analyse();
    // Blocker report: CELL is the only thing keeping ModC and ModE out.
    const auto& blockers = p.dependencies()->blockers;
    CHECK(!blockers.empty());
    CHECK_EQ(blockers.front().sig.str(), std::string("CELL"));
    CHECK_EQ(blockers.front().plugins, size_t(2));
    CHECK_EQ(blockers.front().soleBlocker, size_t(2));
    CHECK_EQ(blockers.front().unlocks, size_t(2));
    CHECK_EQ(p.dependencies()->unlockedByAll, size_t(2));
    // In the output ModC is no longer master 2, so ModG's copied bytes would name another plugin.
    // ModG is kept out of compilation automatically and everything else still compiles.
    CHECK(p.compile());
    CHECK(!p.validation()->critical());
    CHECK_EQ(p.session().findPlugin("ModG.esp")->eligibility, Eligibility::Retained);
    CHECK(p.session().autoRetained.count("modg.esp") == 1);
    CHECK(p.session().autoRetained.at("modg.esp").find("master index would point elsewhere") != std::string::npos);
    CHECK(!p.compiledOutput()->isCompiled(p.session().findPlugin("ModG.esp")->index));
    CHECK(p.compiledOutput()->isCompiled(p.session().findPlugin("ModA.esp")->index));
    bool reported = false;
    for (const auto& is : p.validation()->issues)
        reported |= is.category == "Kept Out Automatically" && is.plugin == "ModG.esp";
    CHECK(reported);
    p.exportOutputs(); // not blocked

    // With automatic isolation off, the same finding blocks export.
    auto strict = configFor(root);
    strict.databasePath.clear();
    strict.autoIsolate = false;
    Pipeline q(strict);
    q.scan();
    q.analyse();
    CHECK(!q.compile());
    CHECK(q.validation()->critical());
}

TEST(definition_check_flags_suspect_layouts)
{
    auto root = buildInstall("defcheck");
    std::vector<Record> records;
    for (uint32_t i = 0; i < 12; ++i)
        // Values at the described ENIT offsets that name no record, and an undescribed
        // subrecord that always holds a real FormID.
        records.push_back(record("ENCH", 0x01000800 + i,
                                 {zs("EDID", "Ench" + std::to_string(i)), structWith("ENIT", 36, {{28, 0x00ABC000 + i}}),
                                  u32("UNKN", 0x00001000)}));
    writePlugin(root / "Game" / "Data" / "ModD.esp", {"Fallout4.esm"}, std::move(records));
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModD.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    auto check = p.checkDefinitions({Signature("ENCH")});
    CHECK_EQ(check.recordsByType[Signature("ENCH")], uint64_t(12));
    bool wrongLayout = false, undescribed = false;
    for (const auto& is : check.report.issues) {
        wrongLayout |= is.message.find("ENCH ENIT: only 0.0%") != std::string::npos;
        undescribed |= is.message.find("ENCH UNKN (not described)") != std::string::npos;
    }
    CHECK(wrongLayout);
    CHECK(undescribed);
    CHECK(!check.report.critical()); // findings never block export
}

namespace {

// CTDA with any function: param 1, run on "reference" (2) with the given reference.
Subrecord condition(uint16_t function, uint32_t param1, uint32_t reference)
{
    BinaryWriter w;
    w.write<uint8_t>(0);
    w.writeZeros(3);
    w.write<float>(1.0f);
    w.write<uint16_t>(function);
    w.write<uint16_t>(0);
    w.write<uint32_t>(param1);
    w.write<uint32_t>(0);
    w.write<uint32_t>(reference ? 2u : 0u);
    w.write<uint32_t>(reference);
    w.write<int32_t>(-1);
    return Subrecord(Signature("CTDA"), w.take());
}

} // namespace

TEST(conditions_with_unlisted_functions_and_hardcoded_forms)
{
    auto root = buildInstall("conditions");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModE.esp", {"Fallout4.esm"}, {record("CELL", 0x01000800, {zs("EDID", "ModECell")})});
    // ModC is master 2 here but will be master 1 in the output; the condition function is not in
    // the definitions' table, and the condition runs on PlayerRef (hard-coded, not in Fallout4.esm).
    writePlugin(data / "ModH.esp", {"Fallout4.esm", "ModE.esp", "ModC.esp"},
                {record("COBJ", 0x03000800,
                        {zs("EDID", "ModH_Recipe"), condition(9999, 0x02000800, 0x00000014), u32("CNAM", 0x00001000),
                         u32("BNAM", 0x00000800)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModE.esp\n*ModH.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    for (const auto& is : p.validation()->issues) {
        CHECK(is.message.find("probably refers") == std::string::npos);
        CHECK(is.message.find("00000014") == std::string::npos);
    }
    auto result = p.exportOutputs();
    OutputSet out(result.pluginFiles);
    const auto* recipe = out.byEditorId("ModH_Recipe");
    CHECK(recipe != nullptr);
    const auto* ctda = recipe->record.find(Signature("CTDA"));
    // The parameter was recognised as a FormID and renumbered for the output's masters.
    CHECK(out.resolve(*recipe, loadLE<uint32_t>(ctda->data.data() + 12)) == std::make_pair(std::string("ModC.esp"), 0x800u));
    CHECK(out.resolve(*recipe, loadLE<uint32_t>(ctda->data.data() + 24)) == std::make_pair(std::string("Fallout4.esm"), 0x14u));
}

TEST(tier1_second_batch_references_rewritten)
{
    auto root = buildInstall("tier1b");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModS.esp", {"Fallout4.esm"},
                {
                    record("MATO", 0x01000800, {zs("EDID", "SnowMat"), zs("MODL", "Materials\\Snow.nif")}),
                    record("STAT", 0x01000801,
                           {zs("EDID", "Crate"), zs("MODL", "Props\\Crate.nif"), structWith("DNAM", 16, {{4, 0x01000800}})}),
                    record("SCOL", 0x01000802,
                           {zs("EDID", "CrateStack"), u32("ONAM", 0x01000801), raw("DATA", std::vector<uint8_t>(28, 0))}),
                    record("SNDR", 0x01000803, {zs("EDID", "CrateSound"), zs("ANAM", "FX\\Crate.wav")}),
                    record("SOUN", 0x01000804, {zs("EDID", "CrateSoundMarker"), u32("SDSC", 0x01000803)}),
                    record("ACTI", 0x01000805,
                           {zs("EDID", "CrateButton"), zs("MODL", "Props\\Button.nif"), u32("SNAM", 0x01000803),
                            u32("KNAM", 0x00000800)}),
                    // Projectile and explosion refer to each other.
                    record("PROJ", 0x01000806,
                           {zs("EDID", "CrateShot"), structWith("DNAM", 92, {{32, 0x01000807}, {36, 0x01000803}})}),
                    record("EXPL", 0x01000807,
                           {zs("EDID", "CrateBoom"), structWith("DATA", 72, {{4, 0x01000803}, {20, 0x01000806}})}),
                    record("LSCR", 0x01000808, {zs("EDID", "CrateLoading"), u32("NNAM", 0x01000801)}),
                    record("EQUP", 0x01000809, {zs("EDID", "CrateHand"), formIds("PNAM", {0x01000809}), u32("DATA", 0)}),
                    record("ARTO", 0x0100080A, {zs("EDID", "CrateArt"), zs("MODL", "Art\\Crate.nif"), u32("DNAM", 0)}),
                    record("EFSH", 0x0100080B, {zs("EDID", "CrateShader")}),
                    record("RFCT", 0x0100080C, {zs("EDID", "CrateVisual"), structWith("DATA", 12, {{0, 0x0100080A}, {4, 0x0100080B}})}),
                });
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModS.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    CHECK_EQ(p.session().findPlugin("ModS.esp")->eligibility, Eligibility::Eligible);
    p.analyse();
    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    OutputSet out(p.exportOutputs().pluginFiles);
    auto rec = [&](const char* edid) {
        const auto* r = out.byEditorId(edid);
        CHECK(r != nullptr);
        return r;
    };
    auto refAt = [&](const OutputSet::Rec* r, const char* sig, size_t offset) {
        const auto* s = r->record.find(Signature::fromString(sig));
        CHECK(s != nullptr);
        return out.resolve(*r, loadLE<uint32_t>(s->data.data() + offset));
    };
    CHECK(refAt(rec("Crate"), "DNAM", 4) == out.identity(*rec("SnowMat")));
    CHECK(refAt(rec("CrateStack"), "ONAM", 0) == out.identity(*rec("Crate")));
    CHECK(refAt(rec("CrateSoundMarker"), "SDSC", 0) == out.identity(*rec("CrateSound")));
    CHECK(refAt(rec("CrateButton"), "SNAM", 0) == out.identity(*rec("CrateSound")));
    CHECK(refAt(rec("CrateButton"), "KNAM", 0) == std::make_pair(std::string("Fallout4.esm"), 0x800u));
    CHECK(refAt(rec("CrateShot"), "DNAM", 32) == out.identity(*rec("CrateBoom")));
    CHECK(refAt(rec("CrateShot"), "DNAM", 36) == out.identity(*rec("CrateSound")));
    CHECK(refAt(rec("CrateBoom"), "DATA", 20) == out.identity(*rec("CrateShot")));
    CHECK(refAt(rec("CrateLoading"), "NNAM", 0) == out.identity(*rec("Crate")));
    CHECK(refAt(rec("CrateHand"), "PNAM", 0) == out.identity(*rec("CrateHand")));
    CHECK(refAt(rec("CrateVisual"), "DATA", 0) == out.identity(*rec("CrateArt")));
    CHECK(refAt(rec("CrateVisual"), "DATA", 4) == out.identity(*rec("CrateShader")));
    CHECK_EQ(text(rec("CrateSound")->record.find(Signature("ANAM"))), std::string("FX\\Crate.wav"));
}

TEST(ini_editing_keeps_everything_else)
{
    // Replace in place, case-insensitive, keeping CRLF, comments and other keys.
    std::string a = "[Display]\r\niSize W=1920\r\n\r\n[archive]\r\n; mods\r\nsresourcearchivelist2 = Old.ba2\r\nbInvalidateOlderFiles=1\r\n";
    std::string ra = Exporter::setIniValue(a, "Archive", "SResourceArchiveList2", "A.ba2, B.ba2");
    CHECK_EQ(ra, std::string("[Display]\r\niSize W=1920\r\n\r\n[archive]\r\n; mods\r\nSResourceArchiveList2=A.ba2, B.ba2\r\n"
                             "bInvalidateOlderFiles=1\r\n"));
    // Section present, key missing: added after the section's last entry.
    std::string b = "[Archive]\nbInvalidateOlderFiles=1\n\n[General]\nsLanguage=en\n";
    CHECK_EQ(Exporter::setIniValue(b, "Archive", "SResourceArchiveList2", "A.ba2"),
             std::string("[Archive]\nbInvalidateOlderFiles=1\nSResourceArchiveList2=A.ba2\n\n[General]\nsLanguage=en\n"));
    // No section: appended. Empty file: created.
    CHECK_EQ(Exporter::setIniValue("[General]\nsLanguage=en\n", "Archive", "SResourceArchiveList2", "A.ba2"),
             std::string("[General]\nsLanguage=en\n\n[Archive]\nSResourceArchiveList2=A.ba2\n"));
    CHECK_EQ(Exporter::setIniValue("", "Archive", "SResourceArchiveList2", "A.ba2"),
             std::string("[Archive]\nSResourceArchiveList2=A.ba2\n"));
}

TEST(apply_registers_and_unregisters_archives_in_the_ini)
{
    auto root = buildInstall("applyini");
    writeFile(root / "ini" / "Fallout4Custom.ini", "[Display]\niSize W=1920\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    {
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        CHECK(p.compile());
        auto result = p.exportOutputs();
        auto written = Exporter::applyArchiveIni(p.session(), *p.compiledOutput(), result.directory);
        CHECK_EQ(written.size(), size_t(1));
        std::ifstream in(root / "ini" / "Fallout4Custom.ini");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("iSize W=1920") != std::string::npos);
        CHECK(text.find("SResourceArchiveList2=Fallout4 - Animations.ba2, ModA - Main.ba2") != std::string::npos);
        bool backup = false;
        for (const auto& e : std::filesystem::directory_iterator(root / "ini"))
            backup |= pathToUtf8(e.path().filename()).find("multiplexer-backup") != std::string::npos;
        CHECK(backup);
    }
    // A later build that no longer compiles ModA removes its archive again.
    cfg.analysis.excludedPlugins = {"ModA.esp"};
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    auto result = p.exportOutputs();
    Exporter::applyArchiveIni(p.session(), *p.compiledOutput(), result.directory);
    std::ifstream in(root / "ini" / "Fallout4Custom.ini");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("ModA - Main.ba2") == std::string::npos);
    CHECK(text.find("SResourceArchiveList2=Fallout4 - Animations.ba2") != std::string::npos);
}

TEST(archive_priority_change_is_reported)
{
    auto root = buildInstall("archprio");
    auto data = root / "Game" / "Data";
    // ModE stays in the load order (it has a cell), loads before ModA and ships the same mesh.
    writePlugin(data / "ModE.esp", {"Fallout4.esm"}, {record("CELL", 0x01000800, {zs("EDID", "ModECell")})});
    writeGeneralBa2(data / "ModE - Main.ba2", {{"meshes\\moda\\gun.nif", std::vector<uint8_t>(64, 9)}}, true);
    writeFile(root / "plugins.txt", "*ModE.esp\n*ModA.esp\n*ModB.esp\n*ModC.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    const auto& changes = p.archivePriorityChanges();
    CHECK_EQ(changes.size(), size_t(1));
    CHECK_EQ(changes.at(0).path, std::string("meshes\\moda\\gun.nif"));
    CHECK_EQ(changes.at(0).before, std::string("ModA - Main.ba2"));
    CHECK_EQ(changes.at(0).after, std::string("ModE - Main.ba2"));
    bool warned = false;
    for (const auto& is : p.validation()->issues)
        warned |= is.category == "Archive Priority";
    CHECK(warned);
    auto result = p.exportOutputs();
    CHECK(std::filesystem::exists(result.directory / "Multiplexer_ArchivePriority.txt"));
}

TEST(weapon_sounds_at_unaligned_offsets_are_rewritten)
{
    auto root = buildInstall("weaponsounds");
    std::vector<uint8_t> dnam(136, 0);
    storeLE<uint32_t>(dnam.data(), 0x00000900);       // ammo (base game)
    storeLE<uint32_t>(dnam.data() + 73, 0x01000800);  // attack sound: this mod's own descriptor
    storeLE<uint32_t>(dnam.data() + 93, 0x01000800);  // equip sound
    writePlugin(root / "Game" / "Data" / "ModW.esp", {"Fallout4.esm"},
                {record("SNDR", 0x01000800, {zs("EDID", "ModW_Fire"), zs("ANAM", "Weapons\\ModW\\Fire.wav")}),
                 record("WEAP", 0x01000801, {zs("EDID", "ModW_Rifle"), raw("DNAM", dnam)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModW.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    OutputSet out(p.exportOutputs().pluginFiles);
    const auto* rifle = out.byEditorId("ModW_Rifle");
    const auto* sound = out.byEditorId("ModW_Fire");
    CHECK(rifle != nullptr && sound != nullptr);
    const auto& d = rifle->record.find(Signature("DNAM"))->data;
    CHECK(out.resolve(*rifle, loadLE<uint32_t>(d.data() + 73)) == out.identity(*sound));
    CHECK(out.resolve(*rifle, loadLE<uint32_t>(d.data() + 93)) == out.identity(*sound));
    CHECK(out.resolve(*rifle, loadLE<uint32_t>(d.data())) == std::make_pair(std::string("Fallout4.esm"), 0x900u));
}

TEST(unaligned_undescribed_formid_is_caught)
{
    auto root = buildInstall("unaligned");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModE.esp", {"Fallout4.esm"}, {record("CELL", 0x01000800, {zs("EDID", "ModECell")})});
    // ModC's cell at an odd offset of a subrecord the definitions do not describe.
    std::vector<uint8_t> bytes(10, 0);
    storeLE<uint32_t>(bytes.data() + 3, 0x02000800);
    writePlugin(data / "ModG.esp", {"Fallout4.esm", "ModE.esp", "ModC.esp"},
                {record("GLOB", 0x03000800, {zs("EDID", "ModG_Global"), raw("FNAM", {'f'}), u32("FLTV", 0),
                                              raw("UNKN", bytes)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModE.esp\n*ModG.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    // Copying the bytes unchanged would point at the wrong plugin, so ModG stays out.
    CHECK(p.session().autoRetained.count("modg.esp") == 1);
    auto check = p.checkDefinitions({Signature("GLOB")});
    bool reported = false;
    for (const auto& is : check.report.issues)
        reported |= is.message.find("GLOB UNKN") != std::string::npos && is.message.find("+3") != std::string::npos;
    CHECK(reported);
}

TEST(archive_loaders_keep_archive_priority_in_mo2)
{
    auto root = tempDir("loaders");
    auto game = root / "Game";
    writePlugin(game / "Data" / "Fallout4.esm", {}, {record("KYWD", 0x800, {zs("EDID", "K")})}, ESM);
    auto inst = root / "MO2";
    writeFile(inst / "ModOrganizer.ini", "[General]\ngamePath=@ByteArray(" + pathToUtf8(game) +
                                             ")\nselected_profile=@ByteArray(Default)\n[Settings]\n");
    // ModStay (a cell: stays) loads first and ships the same texture as ModWeap (compiled).
    writeFile(inst / "profiles" / "Default" / "modlist.txt", "+ModWeap\n+ModStay\n");
    writeFile(inst / "profiles" / "Default" / "plugins.txt", "*ModStay.esp\n*ModWeap.esp\n");
    writePlugin(inst / "mods" / "ModStay" / "ModStay.esp", {"Fallout4.esm"},
                {record("CELL", 0x01000800, {zs("EDID", "StayCell")})});
    writeGeneralBa2(inst / "mods" / "ModStay" / "ModStay - Main.ba2", {{"textures\\gun.dds", std::vector<uint8_t>(32, 1)}},
                    false);
    writePlugin(inst / "mods" / "ModWeap" / "ModWeap.esp", {"Fallout4.esm"},
                {record("KYWD", 0x01000800, {zs("EDID", "WeapKeyword")})});
    writeGeneralBa2(inst / "mods" / "ModWeap" / "ModWeap - Main.ba2", {{"textures\\gun.dds", std::vector<uint8_t>(32, 2)}},
                    false);

    PipelineConfig cfg;
    cfg.install.kind = InstallKind::ModOrganizer2;
    cfg.install.mo2Path = inst;
    cfg.install.iniDirectory = root / "ini";
    cfg.databasePath = root / "db.sqlite";
    cfg.compile.outputCount = 10;
    {
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        CHECK(p.compile());
        const auto* c = p.compiledOutput();
        auto weap = p.session().findPlugin("ModWeap.esp")->index;
        CHECK(c->isCompiled(weap));
        CHECK(c->hasArchiveLoader(weap));
        CHECK(c->archivesToRegister.empty());          // nothing for the INI
        CHECK(p.archivePriorityChanges().empty());     // ModWeap's texture keeps winning
        auto result = p.exportOutputs();
        CHECK_EQ(result.archiveLoaders.size(), size_t(1));
        auto header = readPluginHeader(result.archiveLoaders.at(0));
        CHECK(header.record.flags & RecordFlags::Light);
        CHECK_EQ(header.author, std::string(kGeneratorAuthor));
        CHECK(header.masters.empty());
        std::ifstream proposed(result.pluginsTxt);
        std::string text((std::istreambuf_iterator<char>(proposed)), std::istreambuf_iterator<char>());
        CHECK(text.find("*ModWeap.esp") != std::string::npos); // stays enabled: it is the loader now
    }

    // An output folder typed by hand with different letter case still counts as an MO2 mod.
    {
        auto typed = cfg;
        typed.databasePath = root / "db2.sqlite";
        typed.exportOptions.outputDirectory = inst / "MODS" / "Fallout 4 Multiplexer Output";
        Pipeline q(typed);
        q.scan();
        q.analyse();
        CHECK(q.compile());
        CHECK(q.compiledOutput()->hasArchiveLoader(q.session().findPlugin("ModWeap.esp")->index));
    }
    // Outside the mods folder: no loaders, and validation says why.
    {
        auto outside = cfg;
        outside.databasePath = root / "db3.sqlite";
        outside.exportOptions.outputDirectory = root / "Elsewhere";
        Pipeline q(outside);
        q.scan();
        q.analyse();
        CHECK(q.compile());
        CHECK(q.compiledOutput()->archiveLoaders.empty());
        bool explained = false;
        for (const auto& is : q.validation()->issues)
            explained |= is.category == "Archive Loaders" &&
                         is.message.find("is not inside Mod Organizer's mods folder") != std::string::npos;
        CHECK(explained);
    }

    // After switching over: the output mod wins in MO2 and ModWeap.esp is the loader. A rescan
    // still reads the original underneath and does not treat the outputs as sources.
    writeFile(inst / "profiles" / "Default" / "modlist.txt", "+Fallout 4 Multiplexer Output\n+ModWeap\n+ModStay\n");
    writeFile(inst / "profiles" / "Default" / "plugins.txt", "*ModStay.esp\n*ModWeap.esp\n*Dummy_Plugin_01.esp\n");
    Pipeline p(cfg);
    p.scan();
    const auto* weap = p.session().findPlugin("ModWeap.esp");
    CHECK(weap->behindLoader);
    CHECK_EQ(weap->sourceMod, std::string("ModWeap"));
    CHECK(weap->active);
    CHECK_EQ(weap->recordCount, 1u); // the original's record, not the empty loader
    CHECK(p.session().findPlugin("Dummy_Plugin_01.esp")->generated);
    CHECK(!p.session().findPlugin("Dummy_Plugin_01.esp")->active);
    p.analyse();
    CHECK(p.compile());
    CHECK(p.compiledOutput()->isCompiled(weap->index));
}

TEST(rescan_after_switching_over_reads_the_original_load_order)
{
    auto root = buildInstall("switched");
    auto cfg = configFor(root);
    cfg.exportOptions.outputDirectory = root / "Game" / "Data"; // outputs visible to later scans
    std::vector<std::string> compiled;
    {
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        CHECK(p.compile());
        for (auto pi : p.compiledOutput()->compiledPlugins)
            compiled.push_back(p.session().plugins[pi].name);
        auto result = p.exportOutputs();
        // Switch over: the proposed plugins.txt disables compiled plugins and enables the outputs.
        std::filesystem::copy_file(result.pluginsTxt, root / "plugins.txt", std::filesystem::copy_options::overwrite_existing);
    }
    CHECK(!compiled.empty());
    Pipeline p(cfg);
    p.scan();
    for (const auto& name : compiled) {
        const auto* pl = p.session().findPlugin(name);
        CHECK(pl->active);      // still a source, in its recorded position
        CHECK(pl->reactivated);
    }
    for (const auto& pl : p.session().plugins)
        if (pl.name.rfind("Dummy_Plugin_", 0) == 0) {
            CHECK(pl.generated);
            CHECK(!pl.active);
        }
    // Nothing changed since the build, so nothing needs rebuilding.
    CHECK(p.changes().status != BuildStatus::RequiresRebuild);
}

TEST(references_broken_in_the_source_are_written_as_none)
{
    auto root = buildInstall("brokensource");
    // ModX refers to its own record 0x900, which it does not contain (deleted in an update), and
    // carries a FormID whose master index is past its master list.
    writePlugin(root / "Game" / "Data" / "ModX.esp", {"Fallout4.esm"},
                {record("FLST", 0x01000800, {zs("EDID", "ModX_List"), u32("LNAM", 0x01000900), u32("LNAM", 0xFE001234),
                                              u32("LNAM", 0x00001000)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModX.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    CHECK(p.session().autoRetained.empty()); // compiled, not kept out
    CHECK(p.compiledOutput()->isCompiled(p.session().findPlugin("ModX.esp")->index));
    size_t warned = 0;
    for (const auto& is : p.validation()->issues)
        warned += is.category == "Broken in Source";
    CHECK_EQ(warned, size_t(2));
    OutputSet out(p.exportOutputs().pluginFiles);
    const auto* list = out.byEditorId("ModX_List");
    CHECK(list != nullptr);
    auto items = list->record.findAll(Signature("LNAM"));
    CHECK_EQ(items.size(), size_t(3));
    CHECK_EQ(loadLE<uint32_t>(items[0]->data.data()), 0u);
    CHECK_EQ(loadLE<uint32_t>(items[1]->data.data()), 0u);
    CHECK(out.resolve(*list, loadLE<uint32_t>(items[2]->data.data())) == std::make_pair(std::string("Fallout4.esm"), 0x1000u));
}

TEST(missing_references_get_a_likely_cause)
{
    auto root = buildInstall("causes");
    auto data = root / "Game" / "Data";
    // A patch made for another version of ModA: three records it expects are not in ModA.
    writePlugin(data / "ModA - Patch.esp", {"Fallout4.esm", "ModA.esp"},
                {record("FLST", 0x00001100, {zs("EDID", "WeaponList"), u32("LNAM", 0x01000990), u32("LNAM", 0x01000991),
                                              u32("LNAM", 0x01000992)})});
    // A mod that refers to its own record, which an update removed.
    writePlugin(data / "ModR.esp", {"Fallout4.esm"},
                {record("FLST", 0x01000800, {zs("EDID", "ModR_List"), u32("LNAM", 0x01000F00)})});
    // An ESL-flagged mod referring to its own pre-compaction record, and a FFFFFF placeholder.
    writePlugin(data / "ModEsl.esp", {"Fallout4.esm"},
                {record("FLST", 0x01000800, {zs("EDID", "ModEsl_List"), u32("LNAM", 0x01007A13), u32("LNAM", 0x01FFFFFF)})},
                RecordFlags::Light);
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModA - Patch.esp\n*ModR.esp\n*ModEsl.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    bool summary = false, version = false, own = false;
    for (const auto& is : p.dependencies()->issues) {
        if (is.category == "Missing References (summary)" && is.message.find("ModA - Patch.esp -> ModA.esp: 3") == 0)
            summary = is.cause.find("made for a different version of ModA.esp") != std::string::npos;
        if (is.category == "Missing Reference" && is.plugin == p.session().findPlugin("ModA - Patch.esp")->index)
            version |= is.cause.find("different version of ModA.esp") != std::string::npos;
        if (is.category == "Missing Reference" && is.plugin == p.session().findPlugin("ModR.esp")->index)
            own |= is.cause.find("refers to its own record") != std::string::npos;
    }
    CHECK(summary);
    CHECK(version);
    CHECK(own);
    bool eslOwn = false, placeholder = false;
    for (const auto& is : p.dependencies()->issues)
        if (is.category == "Missing Reference" && is.plugin == p.session().findPlugin("ModEsl.esp")->index) {
            eslOwn |= is.cause.find("refers to its own record 007A13, but it is ESL-flagged") != std::string::npos;
            placeholder |= is.cause.find("placeholder") != std::string::npos;
            CHECK(is.cause.find("made for your version of ModEsl.esp") == std::string::npos);
        }
    CHECK(eslOwn);
    CHECK(placeholder);
    for (const auto& is : p.dependencies()->issues)
        CHECK(!is.cause.empty());
}

// The catalogue names every type, compressed records of any type show their Editor ID, and record
// types that are not Fallout 4 types are reported as such.
TEST(catalogue_types_editor_ids_and_unknown_signatures)
{
    auto root = buildInstall("catalogue");
    auto data = root / "Game" / "Data";
    // A large (compressed) record of a type the compiler does not describe.
    writePlugin(data / "ModL.esp", {"Fallout4.esm"},
                {record("LAND", 0x01000800, {raw("DATA", std::vector<uint8_t>(20000, 1))}),
                 record("GMST", 0x01000801, {zs("EDID", "fModSetting"), raw("DATA", std::vector<uint8_t>(20000, 2))})},
                0, true);
    // A record type that does not exist in Fallout 4 (an Oblivion creature).
    writePlugin(data / "ModU.esp", {"Fallout4.esm"}, {record("CREA", 0x01000800, {zs("EDID", "OldCreature")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModL.esp\n*ModU.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    auto& s = p.session();

    bool compressedEdid = false;
    for (const auto& r : s.records)
        if (r.plugin == s.findPlugin("ModL.esp")->index && r.signature == Signature("GMST"))
            compressedEdid = r.editorId && s.strings.get(r.editorId) == "fModSetting";
    CHECK(compressedEdid);

    const auto* modU = s.findPlugin("ModU.esp");
    CHECK_EQ(modU->eligibility, Eligibility::Ineligible);
    bool unknownReason = false;
    for (const auto& r : modU->reasons)
        unknownReason |= r.find("not Fallout 4 record types: CREA (1)") != std::string::npos;
    CHECK(unknownReason);

    p.analyse();
    bool crea = false, gmst = false;
    for (const auto& b : p.dependencies()->blockers) {
        if (b.sig == Signature("CREA"))
            crea = !b.known && b.category == "Unknown" && b.support.find("Unknown record type") == 0;
        if (b.sig == Signature("GMST"))
            gmst = b.known && b.name == "Game Setting" && b.category == "Game Data";
    }
    CHECK(crea);
    CHECK(gmst);
}

// Record types without a layout are classified record by record and never shown as merged.
TEST(types_that_are_not_merged_are_classified_record_by_record)
{
    auto root = buildInstall("notmerged");
    auto data = root / "Game" / "Data";
    auto gmst = [](uint32_t id, const char* edid, uint8_t v) {
        return record("GMST", id, {zs("EDID", edid), raw("DATA", {v, 0, 0, 0})});
    };
    writePlugin(data / "ModG.esm", {"Fallout4.esm"},
                {gmst(0x01000800, "fA", 1), gmst(0x01000801, "fB", 1), gmst(0x01000802, "fC", 1)}, ESM);
    writePlugin(data / "ModX.esp", {"Fallout4.esm", "ModG.esm"}, {gmst(0x01000800, "fA", 2), gmst(0x01000801, "fB", 2)});
    writePlugin(data / "ModY.esp", {"Fallout4.esm", "ModG.esm"}, {gmst(0x01000800, "fA", 3), gmst(0x01000802, "fC", 1)});
    // ModZ is made against ModX's version and changes it further.
    writePlugin(data / "ModZ.esp", {"Fallout4.esm", "ModG.esm", "ModX.esp"}, {gmst(0x01000801, "fB", 3)});
    writeFile(root / "plugins.txt", "*ModG.esm\n*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModX.esp\n*ModY.esp\n*ModZ.esp\n");

    for (bool allTypes : {true, false}) {
        auto cfg = configFor(root);
        cfg.databasePath.clear();
        cfg.conflicts.analyseAllRecordTypes = allTypes;
        Pipeline p(cfg);
        p.scan();
        p.analyse();
        auto& s = p.session();
        const auto origin = s.findPlugin("ModG.esm")->index;
        const auto* a = p.conflicts()->find({origin, 0x800});
        const auto* b = p.conflicts()->find({origin, 0x801});
        const auto* c = p.conflicts()->find({origin, 0x802});
        CHECK(a && b);
        CHECK(c == nullptr); // identical-to-master chains are counted, not listed

        // Two plugins changed it independently: the winner drops ModX's change. Not merged.
        CHECK(a->handling == MergeHandling::NotCompiled);
        CHECK_EQ(int(a->type), int(ConflictType::RecordConflict));
        CHECK(a->summary.find("drops the changes of ModX.esp") != std::string::npos);
        CHECK(a->summary.find("Not merged") != std::string::npos);
        for (const auto& f : a->fields) {
            CHECK_EQ(f.result, f.cells.back().value); // the game uses the winner's version
            for (const auto& cell : f.cells)
                CHECK(cell.status != CellStatus::Merged);
        }
        CHECK_EQ(a->fields.empty(), !allTypes);

        // ModZ builds on ModX's version: an override, nothing lost.
        CHECK_EQ(int(b->type), int(ConflictType::SimpleOverride));
        CHECK(b->summary.find("builds on") != std::string::npos);

        // ModY repeats the master's version of fC: identical to master.
        CHECK_EQ(s.findPlugin("ModY.esp")->itmCount, size_t(1));
        CHECK(p.conflicts()->byType[4] >= 1);
    }
}

// Cleaning report: ITMs that undo changes, deleted references, navmeshes and records, injected
// records and circular leveled lists. Reporting only.
TEST(cleaning_report_findings)
{
    auto root = buildInstall("cleaning");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModW.esm", {"Fallout4.esm"},
                {record("REFR", 0x01000800, {raw("NAME", {0, 0, 0, 0})}),
                 record("NAVM", 0x01000801, {raw("NVNM", std::vector<uint8_t>(8, 0))}),
                 record("KYWD", 0x01000802, {zs("EDID", "ModW_Keyword")}),
                 record("LVLI", 0x01000803, {zs("EDID", "ListA"), raw("LLCT", {1}), lvlo(1, 0x01000804, 1)}),
                 record("LVLI", 0x01000804, {zs("EDID", "ListB"), raw("LLCT", {1}), lvlo(1, 0x01000803, 1)})},
                ESM);
    writePlugin(data / "ModD.esp", {"Fallout4.esm", "ModW.esm"},
                {record("REFR", 0x01000800, {}, RecordFlags::Deleted),
                 record("NAVM", 0x01000801, {}, RecordFlags::Deleted),
                 record("KYWD", 0x01000802, {}, RecordFlags::Deleted),
                 // A new record in ModW's FormID range that ModW does not have.
                 record("KYWD", 0x01000900, {zs("EDID", "Injected_Keyword")})});
    // Repeats Fallout4.esm's form list unchanged but loads after ModB, which added to it.
    writePlugin(data / "ModI.esp", {"Fallout4.esm"},
                {record("FLST", 0x00001100, {zs("EDID", "WeaponList"), formIds("LNAM", {0x1000})})});
    writeFile(root / "plugins.txt", "*ModW.esm\n*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModD.esp\n*ModI.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    auto& s = p.session();

    std::map<std::string, std::vector<const DependencyIssue*>> found;
    for (const auto& is : p.dependencies()->issues)
        if (is.cleaning) {
            found[is.category].push_back(&is);
            CHECK(!is.cause.empty());
        }
    auto one = [&](const char* category, const char* plugin) -> const DependencyIssue* {
        for (const auto* is : found[category])
            if (is->plugin == s.findPlugin(plugin)->index)
                return is;
        return nullptr;
    };
    const auto* undo = one("Identical to Master (undoes changes)", "ModI.esp");
    CHECK(undo && undo->severity == Severity::Warning);
    CHECK(one("Identical to Master (summary)", "ModI.esp") != nullptr);
    const auto* ref = one("Deleted Reference", "ModD.esp");
    CHECK(ref && ref->severity == Severity::Warning);
    CHECK(ref->cause.find("Undelete and Disable") != std::string::npos);
    CHECK(one("Deleted Navmesh", "ModD.esp") != nullptr);
    CHECK(one("Deleted Record", "ModD.esp") != nullptr);
    const auto* injected = one("Injected Record", "ModD.esp");
    CHECK(injected && injected->message.find("belongs to ModW.esm") != std::string::npos);
    CHECK_EQ(found["Circular Leveled List"].size(), size_t(1));
    const auto* cycle = found["Circular Leveled List"].front();
    CHECK(cycle->severity == Severity::Error);
    CHECK(cycle->message.find("ListA") != std::string::npos && cycle->message.find("ListB") != std::string::npos);
    // Nothing else is flagged: the unmodified fixture plugins have no deleted or injected records.
    CHECK(one("Deleted Reference", "ModA.esp") == nullptr);
    CHECK(one("Injected Record", "ModA.esp") == nullptr);
}

namespace {

// Layers: LayerB's parent is LayerA. ModL2 renames LayerB; ModL3 (not made against ModL2) gives it a
// new parent, LayerC. Both are made against ModL1.
std::filesystem::path buildLayerInstall(const std::string& name)
{
    auto root = buildInstall(name);
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModL1.esp", {"Fallout4.esm"},
                {record("LAYR", 0x01000800, {zs("EDID", "LayerA")}),
                 record("LAYR", 0x01000801, {zs("EDID", "LayerB"), u32("PNAM", 0x01000800)})});
    writePlugin(data / "ModL2.esp", {"Fallout4.esm", "ModL1.esp"},
                {record("LAYR", 0x01000801, {zs("EDID", "LayerB_Renamed"), u32("PNAM", 0x01000800)})});
    writePlugin(data / "ModL3.esp", {"Fallout4.esm", "ModL1.esp"},
                {record("LAYR", 0x02000800, {zs("EDID", "LayerC")}),
                 record("LAYR", 0x01000801, {zs("EDID", "LayerB"), u32("PNAM", 0x02000800)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModL1.esp\n*ModL2.esp\n*ModL3.esp\n");
    return root;
}

} // namespace

// Definitions with some catalogue strategies changed (a trial catalogue), kept alive for the run.
const RecordDefinitions* trialDefinitions(const std::vector<std::pair<std::string, std::string>>& strategies)
{
    static std::vector<std::unique_ptr<RecordCatalogue>> catalogues;
    static std::vector<std::unique_ptr<RecordDefinitions>> definitions;
    std::ifstream in(std::string(FO4MX_SOURCE_DIR) + "/resources/record_catalogue.csv", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const auto& [sig, strategy] : strategies) {
        size_t start = text.find("\n" + sig + ",") + 1;
        size_t field = start;
        for (int i = 0; i < 4; ++i) // the strategy is the fifth column
            field = text.find(',', field) + 1;
        text.replace(field, text.find(',', field) - field, strategy);
    }
    catalogues.push_back(std::make_unique<RecordCatalogue>(RecordCatalogue::parse(text)));
    auto d = std::make_unique<RecordDefinitions>(RecordDefinitions::get());
    d->applyCatalogue(*catalogues.back());
    d->computeFingerprint();
    definitions.push_back(std::move(d));
    return definitions.back().get();
}

// Candidate types have a layout and are checked, but are not compiled until confirmed.
TEST(candidate_types_are_checked_but_not_compiled)
{
    const auto* defs = trialDefinitions({{"LAYR", "Candidate"}, {"SNCT", "Candidate"}});
    CHECK(defs->strategy(Signature("LAYR")) == MergeStrategy::Candidate);
    CHECK(!defs->isCompilable(Signature("LAYR")));
    CHECK(defs->hasLayout(Signature("LAYR")));
    CHECK(defs->supportSummary(Signature("LAYR")).find("Candidate") == 0);

    auto root = buildLayerInstall("candidates");
    // A candidate record carrying an undescribed subrecord that holds a FormID keeps the type unconfirmed.
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModS.esp", {"Fallout4.esm"},
                {record("SNCT", 0x01000800, {zs("EDID", "Cat"), u32("PNAM", 0x01000801)}),
                 record("SNCT", 0x01000801, {zs("EDID", "Parent"), u32("ZZZZ", 0x01000800)})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModL1.esp\n*ModL2.esp\n*ModL3.esp\n*ModS.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    cfg.definitions = defs;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK_EQ(p.session().findPlugin("ModL1.esp")->eligibility, Eligibility::Ineligible);

    auto check = p.checkDefinitions();
    CHECK(std::find(check.readyCandidates.begin(), check.readyCandidates.end(), Signature("LAYR")) !=
          check.readyCandidates.end());
    CHECK(std::find(check.readyCandidates.begin(), check.readyCandidates.end(), Signature("SNCT")) ==
          check.readyCandidates.end());
    bool confirmed = false, rejected = false;
    for (const auto& is : check.report.issues) {
        confirmed |= is.message.find("LAYR (Layer): candidate confirmed") == 0;
        rejected |= is.message.find("SNCT (Sound Category): candidate not confirmed: ZZZZ (not described)") == 0;
    }
    CHECK(confirmed);
    CHECK(rejected);
}

// Winner strategy: compiled, every FormID rewritten, the winning version taken whole.
TEST(winner_types_compile_the_winning_version_whole)
{
    const auto& defs = RecordDefinitions::get(); // LAYR is Winner since a real load order confirmed it
    CHECK(defs.isCompilable(Signature("LAYR")));
    CHECK(!defs.mergesFields(Signature("LAYR")));

    auto root = buildLayerInstall("winneronly");
    auto cfg = configFor(root);
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    auto& s = p.session();
    for (const char* n : {"ModL1.esp", "ModL2.esp", "ModL3.esp"})
        CHECK_EQ(s.findPlugin(n)->eligibility, Eligibility::Eligible);

    // Two independent changes: classified record by record, the winner drops ModL2's rename.
    const auto* e = p.conflicts()->find({s.findPlugin("ModL1.esp")->index, 0x801});
    CHECK(e != nullptr);
    CHECK(e->handling == MergeHandling::WinnerOnly);
    CHECK_EQ(int(e->type), int(ConflictType::RecordConflict));
    CHECK(e->summary.find("Winner only") != std::string::npos);

    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    auto result = p.exportOutputs();
    OutputSet out(result.pluginFiles);
    // Field merging would have produced "LayerB_Renamed" with LayerC as parent; winner-only keeps ModL3's record.
    CHECK(out.byEditorId("LayerB_Renamed") == nullptr);
    const auto* b = out.byEditorId("LayerB");
    const auto* c = out.byEditorId("LayerC");
    CHECK(b && c);
    CHECK(out.resolve(*b, loadLE<uint32_t>(b->record.find(Signature("PNAM"))->data.data())) == out.identity(*c));
}

// BPTD node data holds FormIDs at unaligned offsets; the definition check confirms the layout when
// every described slot names a real record.
TEST(body_part_data_candidate_is_confirmed_by_the_definition_check)
{
    auto root = buildInstall("bptd");
    auto data = root / "Game" / "Data";
    std::vector<uint8_t> bpnd(101, 0);
    for (uint16_t off : {4, 8, 16, 20, 52, 56, 67, 79, 83, 87, 91})
        storeLE<uint32_t>(bpnd.data() + off, 0x01000801); // the keyword below: any existing record resolves
    writePlugin(data / "ModP.esp", {"Fallout4.esm"},
                {record("BPTD", 0x01000800, {zs("EDID", "Bodies"), zs("BPNN", "Head"), zs("BPNT", "Head"),
                                             raw("BPND", bpnd), zs("NAM1", ""), zs("NAM4", "")}),
                 record("KYWD", 0x01000801, {zs("EDID", "Target")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModP.esp\n");
    auto cfg = configFor(root);
    cfg.databasePath.clear();
    cfg.definitions = trialDefinitions({{"BPTD", "Candidate"}});
    Pipeline p(cfg);
    auto check = p.checkDefinitions({Signature("BPTD")});
    CHECK(std::find(check.readyCandidates.begin(), check.readyCandidates.end(), Signature("BPTD")) !=
          check.readyCandidates.end());
    uint64_t formIds = 0, resolved = 0;
    for (const auto& st : check.stats)
        if (st.record == Signature("BPTD") && st.sub == Signature("BPND")) {
            formIds = st.formIds;
            resolved = st.resolved;
        }
    CHECK_EQ(formIds, uint64_t(11));
    CHECK_EQ(resolved, uint64_t(11));
}

// NPC_ compiles only as overrides: a plugin that adds a new NPC stays out (FaceGen data is stored
// under the defining plugin's name), a plugin that only overrides one compiles.
TEST(npc_overrides_only)
{
    const auto* defs = trialDefinitions({{"NPC_", "Winner"}});
    CHECK(defs->isCompilable(Signature("NPC_")));
    CHECK(defs->overridesOnly(Signature("NPC_")));

    auto root = buildInstall("npcoverrides");
    auto data = root / "Game" / "Data";
    auto item = [](uint32_t form, uint32_t count) {
        std::vector<uint8_t> b(8);
        storeLE<uint32_t>(b.data(), form);
        storeLE<uint32_t>(b.data() + 4, count);
        return raw("CNTO", b);
    };
    writePlugin(data / "ModN.esm", {"Fallout4.esm"},
                {record("NPC_", 0x01000800, {zs("EDID", "Raider"), zs("FULL", "Raider"), u32("COCT", 1), item(0x1000, 1)})},
                ESM);
    writePlugin(data / "ModNO.esp", {"Fallout4.esm", "ModN.esm"},
                {record("NPC_", 0x01000800, {zs("EDID", "Raider"), zs("FULL", "Raider Boss"), u32("COCT", 1), item(0x1000, 2)})});
    writePlugin(data / "ModNN.esp", {"Fallout4.esm"}, {record("NPC_", 0x01000800, {zs("EDID", "NewGuy")})});
    writeFile(root / "plugins.txt", "*ModN.esm\n*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModNO.esp\n*ModNN.esp\n");
    auto cfg = configFor(root);
    cfg.definitions = defs;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    auto& s = p.session();
    CHECK_EQ(s.findPlugin("ModNO.esp")->eligibility, Eligibility::Eligible);
    for (const char* name : {"ModNN.esp", "ModN.esm"}) {
        const auto* pl = s.findPlugin(name);
        CHECK_EQ(pl->eligibility, Eligibility::Ineligible);
        bool reason = false;
        for (const auto& r : pl->reasons)
            reason |= r.find("Adds new records of types compiled only as overrides: NPC_ (1)") == 0;
        CHECK(reason);
    }

    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    OutputSet out(p.exportOutputs().pluginFiles);
    const OutputSet::Rec* boss = nullptr;
    for (const auto& r : out.records)
        if (r.record.header.signature == Signature("NPC_"))
            boss = &r;
    CHECK(boss != nullptr);
    CHECK(out.identity(*boss) == std::make_pair(std::string("ModN.esm"), 0x800u)); // still an override
    CHECK_EQ(text(boss->record.find(sig::FULL)), std::string("Raider Boss"));
    auto cnto = boss->record.find(Signature("CNTO"));
    CHECK(out.resolve(*boss, loadLE<uint32_t>(cnto->data.data())) == std::make_pair(std::string("Fallout4.esm"), 0x1000u));
}

// Every report is also written as plain text with aligned columns.
TEST(text_reports)
{
    auto root = buildInstall("textreports");
    auto cfg = configFor(root);
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    auto files = p.writeReports(root / "Text");
    CHECK(std::find(files.begin(), files.end(), root / "Text" / "ConflictReport.txt") != files.end());
    std::ifstream in(root / "Text" / "ConflictReport.txt");
    std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(txt.rfind("Conflict Report\n===============\n", 0) == 0);
    CHECK(txt.find("Pistol10mm [WEAP:00001000]") != std::string::npos);
    CHECK(txt.find('<') == std::string::npos); // no markup
}

// Advanced Users Auto-Merger: a plugin named in a script and an MCM config is compiled when both can
// be relinked; relinked copies point at the output plugin and the record's new FormID. A plugin named
// in a config without a FormID stays out.
TEST(auto_merger_relinks_scripts_and_configs)
{
    auto root = buildInstall("automerger");
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModS.esp", {"Fallout4.esm"}, {record("KYWD", 0x01000812, {zs("EDID", "ModS_Keyword")})});
    writePlugin(data / "ModT.esp", {"Fallout4.esm"}, {record("KYWD", 0x01000800, {zs("EDID", "ModT_Keyword")})});
    writeBytes(data / "Scripts" / "ModSUser.pex", testScript(0x812, "ModS.esp"));
    writeFile(data / "MCM" / "Config" / "ModS" / "config.json",
              "{\"modName\": \"ModS\", \"pluginRequirements\": [\"ModS.esp\"],\n"
              " \"content\": [{\"sourceForm\": \"ModS.esp|812\"}, {\"other\": \"Not ModS.esp|800\"}]}\n");
    writeFile(data / "F4SE" / "Plugins" / "RobCo_Patcher" / "modt.ini", "filterByPlugin=ModT.esp\n");
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModS.esp\n*ModT.esp\n");

    {
        Pipeline off(configFor(root));
        off.scan();
        off.analyse();
        CHECK_EQ(off.session().findPlugin("ModS.esp")->eligibility, Eligibility::Ineligible);
        CHECK(off.autoMerge() == nullptr);
    }

    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    auto& s = p.session();
    const auto* modS = s.findPlugin("ModS.esp");
    const auto* modT = s.findPlugin("ModT.esp");
    CHECK_EQ(modS->eligibility, Eligibility::Eligible);
    CHECK_EQ(modT->eligibility, Eligibility::Ineligible);
    const auto* am = p.autoMerge();
    CHECK(am != nullptr);
    CHECK(am->relinkable.count(modS->index) == 1);
    CHECK(am->relinkable.count(modT->index) == 0);
    bool plain = false;
    for (const auto& f : am->files)
        if (f.path == "f4se\\plugins\\robco_patcher\\modt.ini")
            plain = f.problem.find("without a FormID") != std::string::npos;
    CHECK(plain);

    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    auto result = p.exportOutputs();
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    const FormMapping* kw = nullptr;
    for (const auto& m : p.compiledOutput()->mappings)
        if (m.editorId == "ModS_Keyword")
            kw = &m;
    CHECK(kw != nullptr);

    // The script: GetFormFromFile points at the output and the new FormID; IsPluginInstalled at the output.
    // Read whole files and close them: Windows does not delete files that are still open.
    auto slurp = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string pexText = slurp(result.directory / "scripts" / "modsuser.pex");
    std::vector<uint8_t> bytes(pexText.begin(), pexText.end());
    auto scan = pex::scan(bytes);
    CHECK(scan.ok);
    CHECK_EQ(scan.getFormFromFile.size(), size_t(1));
    CHECK_EQ(scan.getFormFromFile[0].formId, static_cast<int32_t>(kw->objectId));
    CHECK_EQ(scan.strings[scan.getFormFromFile[0].fileString], kw->outputPlugin);
    // Each use points at a string naming the output; the original name is left, unused.
    size_t toOutput = 0;
    for (const auto& u : scan.uses) {
        CHECK(toLower(scan.strings[u.string]) != "mods.esp");
        toOutput += scan.strings[u.string] == kw->outputPlugin;
    }
    CHECK_EQ(toOutput, size_t(2));

    // The MCM config: the pair rewritten, the requirement renamed, the other plugin's name untouched.
    const std::string json = slurp(result.directory / "mcm" / "config" / "mods" / "config.json");
    char id[16];
    std::snprintf(id, sizeof(id), "%03X", kw->objectId);
    CHECK(json.find("\"" + kw->outputPlugin + "|" + id + "\"") != std::string::npos);
    CHECK(json.find("[\"" + kw->outputPlugin + "\"]") != std::string::npos);
    CHECK(json.find("Not ModS.esp|800") != std::string::npos);
    CHECK(std::filesystem::exists(result.directory / "Multiplexer_Relinked.tsv"));

    // Turning it off removes the relinked copies at the next export.
    p.config().compile.autoMerge = false;
    p.exportOutputs();
    CHECK(!std::filesystem::exists(result.directory / "scripts" / "modsuser.pex"));
    CHECK(!std::filesystem::exists(result.directory / "Multiplexer_Relinked.tsv"));
}

// ---- Filename references --------------------------------------------------------------------------
namespace {

// ModS.esp and ModT.esp each add one keyword (compilable), plus whatever files `files` writes.
std::filesystem::path referenceInstall(const std::string& name)
{
    auto root = buildInstall(name);
    auto data = root / "Game" / "Data";
    writePlugin(data / "ModS.esp", {"Fallout4.esm"}, {record("KYWD", 0x01000812, {zs("EDID", "ModS_Keyword")})});
    writePlugin(data / "ModT.esp", {"Fallout4.esm"}, {record("KYWD", 0x01000800, {zs("EDID", "ModT_Keyword")})});
    writeFile(root / "plugins.txt", "*ModA.esp\n*ModB.esp\n*ModC.esp\n*ModS.esp\n*ModT.esp\n");
    return root;
}

std::vector<const PluginReference*> referencesOf(const Session& s, const std::string& plugin)
{
    std::vector<const PluginReference*> out;
    const auto* p = s.findPlugin(plugin);
    for (const auto& r : s.nameReferences)
        if (p && r.plugin == p->index)
            out.push_back(&r);
    return out;
}

bool hasReason(const PluginInfo& p, const std::string& text)
{
    for (const auto& r : p.reasons)
        if (r.find(text) != std::string::npos)
            return true;
    return false;
}

std::vector<uint8_t> bytesOf(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string slurpFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const ReferenceScanner::PluginNames kNames = {{"mods.esp", 1}, {"modt.esp", 2}, {"modm.esm", 3}, {"modl.esl", 4},
                                              {"power mods.esp", 5}};

} // namespace

// 1. A plugin no file names: no references, no decision, eligible.
TEST(references_none)
{
    auto root = referenceInstall("refnone");
    Pipeline p(configFor(root));
    p.scan();
    p.analyse();
    const auto& s = p.session();
    CHECK(referencesOf(s, "ModS.esp").empty());
    CHECK(s.referenceDecisions.count(s.findPlugin("ModS.esp")->index) == 0);
    CHECK_EQ(s.findPlugin("ModS.esp")->eligibility, Eligibility::Eligible);
}

// 2. One reference in a comment line: safe, the plugin stays eligible without the Auto-Merger.
TEST(references_one_safe_text_reference)
{
    auto root = referenceInstall("refonesafe");
    writeFile(root / "Game" / "Data" / "F4SE" / "Plugins" / "RobCo_Patcher" / "keywords.ini",
              "; made for ModS.esp\nfilterByKeyword=Fallout4.esm|800\n");
    Pipeline p(configFor(root));
    p.scan();
    p.analyse();
    const auto& s = p.session();
    auto refs = referencesOf(s, "ModS.esp");
    CHECK_EQ(refs.size(), size_t(1));
    CHECK_EQ(refs[0]->cls, ReferenceClass::Safe);
    CHECK_EQ(refs[0]->context, std::string("comment"));
    CHECK_EQ(refs[0]->location, std::string("line 1"));
    CHECK_EQ(s.findPlugin("ModS.esp")->eligibility, Eligibility::Eligible);
    CHECK(hasReason(*s.findPlugin("ModS.esp"), "1 filename reference: 1 safe"));
}

// 3. Many safe references (comments, a Papyrus source, an unused script string): the number does not
//    matter, the plugin stays eligible.
TEST(references_many_safe)
{
    auto root = referenceInstall("refmanysafe");
    auto data = root / "Game" / "Data";
    std::string ini;
    for (int i = 0; i < 40; ++i)
        ini += "# ModS.esp note " + std::to_string(i) + "\n";
    writeFile(data / "F4SE" / "Plugins" / "RobCo_Patcher" / "notes.ini", ini);
    writeFile(data / "Scripts" / "Source" / "User" / "ModSQuest.psc",
              "Form f = Game.GetFormFromFile(0x812, \"ModS.esp\")\n");
    ScriptOptions unused;
    unused.unused = "ModS.esp";
    writeBytes(data / "Scripts" / "Other.pex", testScript(0x800, "Fallout4.esm", unused));
    Pipeline p(configFor(root));
    p.scan();
    p.analyse();
    const auto& s = p.session();
    auto refs = referencesOf(s, "ModS.esp");
    CHECK_EQ(refs.size(), size_t(42));
    for (const auto* r : refs)
        CHECK_EQ(r->cls, ReferenceClass::Safe);
    CHECK_EQ(s.findPlugin("ModS.esp")->eligibility, Eligibility::Eligible);
    const auto& d = s.referenceDecisions.at(s.findPlugin("ModS.esp")->index);
    CHECK_EQ(d.total, size_t(42));
    CHECK_EQ(d.cls, ReferenceClass::Safe);
}

// 4. Papyrus source: the game runs the compiled script, never the source.
TEST(references_psc_source)
{
    auto refs = ReferenceScanner::inspect(kNames, "scripts\\source\\user\\a.psc",
                                          bytesOf("Scriptname A\nForm f = Game.GetFormFromFile(0x812, \"ModS.esp\")\n"));
    CHECK_EQ(refs.size(), size_t(1));
    CHECK_EQ(refs[0].kind, ReferenceKind::Papyrus);
    CHECK_EQ(refs[0].cls, ReferenceClass::Safe);
    CHECK_EQ(refs[0].format, std::string("PSC"));
    CHECK_EQ(refs[0].location, std::string("line 2"));
}

// 5. Compiled scripts: each use of the name is classified by what the code does with it.
TEST(references_pex_uses)
{
    auto classes = [](const std::vector<PluginReference>& refs) {
        std::multiset<std::string> out;
        for (const auto& r : refs)
            out.insert(std::string(referenceClassName(r.cls)) + ":" + r.context);
        return out;
    };
    // Fixed FormID and a presence check: both rewritable, with the exact places to change.
    auto fixed = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "ModS.esp"));
    CHECK(classes(fixed) == (std::multiset<std::string>{"Rewritable:Game.GetFormFromFile(00000812)",
                                                         "Rewritable:Game.IsPluginInstalled"}));
    for (const auto& r : fixed) {
        CHECK(r.pexStringOffset > 0);
        CHECK_EQ(r.location, std::string("TestScript.OnInit"));
    }
    // FormID from a variable: the new FormID cannot be supplied.
    ScriptOptions runtime;
    runtime.runtimeFormId = true;
    auto rt = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "ModS.esp", runtime));
    CHECK(classes(rt).count("Unsafe:Game.GetFormFromFile") == 1);
    // The name assigned to a variable: its meaning is unknown.
    ScriptOptions assign;
    assign.assignName = true;
    auto as = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "ModS.esp", assign));
    CHECK(classes(as).count("Unknown:assignment") == 1);
    // A script that does not read back exactly: unknown.
    auto bad = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "ModS.esp", 1));
    CHECK_EQ(bad.size(), size_t(1));
    CHECK_EQ(bad[0].cls, ReferenceClass::Unknown);
    CHECK_EQ(bad[0].context, std::string("unreadable script"));
    // In the string table only: safe.
    ScriptOptions unused;
    unused.unused = "ModT.esp";
    auto un = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "Fallout4.esm", unused));
    CHECK_EQ(un.size(), size_t(1));
    CHECK_EQ(un[0].cls, ReferenceClass::Safe);
    CHECK_EQ(un[0].context, std::string("string table"));
}

// 6. Folders the game finds by the plugin's name keep it out, with or without the Auto-Merger; other
//    folders and file names named after it are unknown.
TEST(references_folder_and_path)
{
    auto root = referenceInstall("refpaths");
    auto data = root / "Game" / "Data";
    writeFile(data / "Meshes" / "Actors" / "Character" / "FaceGenData" / "FaceGeom" / "ModS.esp" / "00000812.nif", "x");
    writeFile(data / "Meshes" / "Actors" / "Character" / "FaceGenData" / "FaceGeom" / "ModS.esp" / "00000813.nif", "x");
    writeFile(data / "Textures" / "ModT.esp_readme.txt", "x");
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    const auto& s = p.session();
    auto refs = referencesOf(s, "ModS.esp");
    CHECK_EQ(refs.size(), size_t(1)); // one folder, not one per file
    CHECK_EQ(refs[0]->kind, ReferenceKind::Directory);
    CHECK_EQ(refs[0]->cls, ReferenceClass::CompatibilityRequired);
    CHECK_EQ(refs[0]->location, std::string("2 files"));
    CHECK_EQ(s.findPlugin("ModS.esp")->eligibility, Eligibility::Ineligible);
    auto t = referencesOf(s, "ModT.esp");
    CHECK_EQ(t.size(), size_t(1));
    CHECK_EQ(t[0]->kind, ReferenceKind::FilePath);
    CHECK_EQ(t[0]->cls, ReferenceClass::Unknown);
    CHECK_EQ(s.findPlugin("ModT.esp")->eligibility, Eligibility::Ineligible);
}

// 7. Safe and unsafe references together: the most restrictive decides.
TEST(references_mixed_safe_and_unsafe)
{
    auto root = referenceInstall("refmixed");
    auto data = root / "Game" / "Data";
    writeFile(data / "F4SE" / "Plugins" / "RobCo_Patcher" / "notes.ini", "; ModS.esp\n");
    std::string dll = std::string("MZ\0\0", 4) + std::string("ModS.esp", 8) + std::string("\0\0", 2);
    writeFile(data / "F4SE" / "Plugins" / "ModSHelper.dll", dll);
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    const auto& s = p.session();
    const auto* modS = s.findPlugin("ModS.esp");
    const auto& d = s.referenceDecisions.at(modS->index);
    CHECK_EQ(d.total, size_t(2));
    CHECK_EQ(d.cls, ReferenceClass::Unsafe);
    CHECK_EQ(d.summary, std::string("2 filename references: 1 unsafe, 1 safe"));
    CHECK_EQ(modS->eligibility, Eligibility::Ineligible);
    CHECK(hasReason(*modS, "f4se\\plugins\\modshelper.dll"));
    CHECK(!d.action.empty());

    // The per-plugin block in the dependency report.
    p.writeReports(root / "Text");
    const auto txt = slurpFile(root / "Text" / "DependencyReport.txt");
    const auto block = txt.find("Plugin: ModS.esp\nFilename references: 2\n");
    CHECK(block != std::string::npos);
    CHECK(txt.find("Overall classification: Unsafe\nReason: Unsafe: f4se\\plugins\\modshelper.dll", block) != std::string::npos);
    CHECK(txt.find("Recommended action: Leave it in the load order", block) != std::string::npos);
    const auto json = slurpFile(root / "Text" / "DependencyReport.json");
    CHECK(json.find("\"filenameReferences\"") != std::string::npos);
    CHECK(json.find("\"recommendedAction\"") != std::string::npos);
}

// 8. Binary files: names in compiled code (ASCII or UTF-16) cannot be changed.
TEST(references_unknown_binary)
{
    std::string utf16;
    for (char c : std::string("ModT.esp"))
        utf16 += std::string(1, c) + std::string(1, '\0');
    auto refs = ReferenceScanner::inspect(kNames, "f4se\\plugins\\x.dll", bytesOf(std::string("\0\0", 2) + utf16 + std::string("\0\0", 2)));
    CHECK_EQ(refs.size(), size_t(1));
    CHECK_EQ(refs[0].kind, ReferenceKind::Binary);
    CHECK_EQ(refs[0].cls, ReferenceClass::Unsafe);
    CHECK_EQ(refs[0].text, std::string("modt.esp"));
}

// 9. A rewrite that cannot be done safely (a script whose string table is full) keeps its plugin
//    out, and nothing is written for it (fail closed).
TEST(references_failed_transformation_fails_closed)
{
    auto root = referenceInstall("reffail");
    ScriptOptions full;
    full.filler = 0xFFFF - 17; // 65535 strings: no room for the output's name
    writeBytes(root / "Game" / "Data" / "Scripts" / "Full.pex", testScript(0x812, "ModS.esp", full));
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    const auto* modS = p.session().findPlugin("ModS.esp");
    CHECK_EQ(modS->eligibility, Eligibility::Eligible); // rewritable
    CHECK(p.compile());
    CHECK(!p.compiledOutput()->isCompiled(modS->index));
    CHECK(hasReason(*modS, "string table is full"));
    CHECK(p.autoMerge()->keptOut.count(modS->index) == 1);
    bool warned = false;
    for (const auto& is : p.validation()->issues)
        warned |= is.category == "Kept Out Automatically" && is.message.find("ModS.esp") != std::string::npos;
    CHECK(warned);
    auto result = p.exportOutputs();
    CHECK(!std::filesystem::exists(result.directory / "scripts" / "full.pex"));
    CHECK(!std::filesystem::exists(result.directory / AutoMerger::kManifest));
}

// 10. A successful rewrite: the copy is scanned again and depends on no compiled plugin; the manifest
//     records every change with its old and new value; the original file is unchanged.
TEST(references_successful_transformation_rescans_clean)
{
    auto root = referenceInstall("refok");
    auto data = root / "Game" / "Data";
    writeBytes(data / "Scripts" / "ModSUser.pex", testScript(0x812, "ModS.esp"));
    writeFile(data / "F4SE" / "Plugins" / "RobCo_Patcher" / "mods.ini",
              "; ModS.esp keyword\nfilterByKeyword=ModS.esp|812:Fallout4.esm|800\n");
    const auto originalScript = slurpFile(data / "Scripts" / "ModSUser.pex");
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    auto result = p.exportOutputs();
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    const auto& s = p.session();
    const FormMapping* kw = nullptr;
    for (const auto& m : p.compiledOutput()->mappings)
        if (m.editorId == "ModS_Keyword")
            kw = &m;
    CHECK(kw != nullptr);

    auto names = ReferenceScanner::pluginNames(s);
    for (const char* file : {"scripts/modsuser.pex", "f4se/plugins/robco_patcher/mods.ini"}) {
        std::string relative = file;
        std::replace(relative.begin(), relative.end(), '/', '\\');
        auto copy = bytesOf(slurpFile(result.directory / file));
        CHECK(!copy.empty());
        for (const auto& r : ReferenceScanner::inspect(names, relative, copy))
            if (r.plugin == s.findPlugin("ModS.esp")->index)
                CHECK_EQ(r.cls, ReferenceClass::Safe); // the comment and the unused string only
    }
    const auto ini = slurpFile(result.directory / "f4se" / "plugins" / "robco_patcher" / "mods.ini");
    char id[16];
    std::snprintf(id, sizeof(id), "%03X", kw->objectId);
    CHECK_EQ(ini, "; ModS.esp keyword\nfilterByKeyword=" + kw->outputPlugin + "|" + id + ":Fallout4.esm|800\n");
    CHECK(slurpFile(data / "Scripts" / "ModSUser.pex") == originalScript); // the user's mod is not changed

    const auto manifest = slurpFile(result.directory / AutoMerger::kManifest);
    CHECK(manifest.find("f4se\\plugins\\robco_patcher\\mods.ini\t") != std::string::npos);
    CHECK(manifest.find("\tModS.esp|812\t" + kw->outputPlugin + "|" + id + "\n") != std::string::npos);
    CHECK(manifest.find("scripts\\modsuser.pex\t") != std::string::npos);
    CHECK(manifest.find("Game.IsPluginInstalled\tModS.esp\t" + kw->outputPlugin + "\n") != std::string::npos);
}

// 11. Export is a transaction: when a copy cannot be written, nothing changes and the earlier copies
//     stay as they were; rollback removes every copy. A file in the output folder that this app did
//     not write is never replaced: its plugin is kept out instead.
TEST(references_rollback)
{
    auto root = referenceInstall("refrollback");
    auto data = root / "Game" / "Data";
    writeBytes(data / "Scripts" / "ModSUser.pex", testScript(0x812, "ModS.esp"));
    writeFile(data / "MCM" / "Config" / "ModS" / "config.json", "{\"pluginRequirements\": [\"ModS.esp\"]}\n");
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    auto result = p.exportOutputs();
    const auto dir = result.directory;
    const auto script = slurpFile(dir / "scripts" / "modsuser.pex");
    const auto json = slurpFile(dir / "mcm" / "config" / "mods" / "config.json");
    CHECK(!script.empty());
    CHECK(json.find("ModS.esp") == std::string::npos);

    // Block one temporary file: the export fails as a whole.
    std::filesystem::create_directories(dir / "scripts" / "modsuser.pex.fo4mx-new" / "blocked");
    p.exportOutputs();
    CHECK(p.validation()->errors > 0);
    CHECK(!p.autoMerge()->errors.empty());
    CHECK(slurpFile(dir / "scripts" / "modsuser.pex") == script);
    CHECK(slurpFile(dir / "mcm" / "config" / "mods" / "config.json") == json);
    CHECK(!std::filesystem::exists(dir / "mcm" / "config" / "mods" / "config.json.fo4mx-new"));
    std::filesystem::remove_all(dir / "scripts" / "modsuser.pex.fo4mx-new");

    CHECK_EQ(AutoMerger::rollback(dir), size_t(2));
    CHECK(!std::filesystem::exists(dir / "scripts" / "modsuser.pex"));
    CHECK(!std::filesystem::exists(dir / AutoMerger::kManifest));

    // A user's own file where a copy would go: never replaced, the plugin stays.
    writeFile(dir / "scripts" / "modsuser.pex", "user file");
    Pipeline q(cfg);
    q.scan();
    q.analyse();
    CHECK(q.compile());
    CHECK(!q.compiledOutput()->isCompiled(q.session().findPlugin("ModS.esp")->index));
    CHECK(hasReason(*q.session().findPlugin("ModS.esp"), "never replaced"));
    q.exportOutputs();
    CHECK_EQ(slurpFile(dir / "scripts" / "modsuser.pex"), std::string("user file"));
}

// 12. One file naming two plugins: only the compiled plugin's references change.
TEST(references_two_plugins_in_one_file)
{
    auto root = referenceInstall("reftwo");
    writeFile(root / "Game" / "Data" / "F4SE" / "Plugins" / "RobCo_Patcher" / "both.ini",
              "filterByKeyword=ModS.esp|812\nfilterByKeyword=ModT.esp|800\n");
    auto cfg = configFor(root);
    cfg.compile.autoMerge = true;
    cfg.analysis.excludedPlugins = {"ModT.esp"};
    Pipeline p(cfg);
    p.scan();
    p.analyse();
    CHECK(p.compile());
    const auto& s = p.session();
    CHECK(p.compiledOutput()->isCompiled(s.findPlugin("ModS.esp")->index));
    CHECK(!p.compiledOutput()->isCompiled(s.findPlugin("ModT.esp")->index));
    auto result = p.exportOutputs();
    CHECK_EQ(p.validation()->errors, uint64_t(0));
    const auto ini = slurpFile(result.directory / "f4se" / "plugins" / "robco_patcher" / "both.ini");
    CHECK(ini.find("ModS.esp") == std::string::npos);
    CHECK(ini.find("\nfilterByKeyword=ModT.esp|800\n") != std::string::npos);
}

// 13. Letter case: names are found whatever their case, and only as whole names (the longest active
//     plugin name wins: "Power ModS.esp" is that plugin, not ModS.esp).
TEST(references_case_variations)
{
    auto all = ReferenceScanner::inspect(
        kNames, "f4se\\plugins\\robco_patcher\\a.ini",
        bytesOf("a=MODS.ESP|0x812\nb=mods.esp|812\nc=Power ModS.esp\nd=ModS.esp2\ne=ModS.Esp\nf=MyModS.esp\n"));
    std::vector<PluginReference> refs;
    size_t power = 0;
    for (const auto& r : all)
        if (r.plugin == 1)
            refs.push_back(r);
        else if (r.plugin == 5)
            ++power;
    CHECK_EQ(power, size_t(1));
    CHECK_EQ(refs.size(), size_t(3));
    CHECK(refs[0].hasFormId && refs[0].formId == 0x812u && refs[0].text == "MODS.ESP|0x812");
    CHECK(refs[1].hasFormId && refs[1].formId == 0x812u);
    CHECK(!refs[2].hasFormId);
    CHECK_EQ(refs[2].cls, ReferenceClass::Unknown);
    CHECK_EQ(refs[2].location, std::string("line 5"));
    auto pex = ReferenceScanner::inspect(kNames, "scripts\\a.pex", testScript(0x812, "MODS.ESP"));
    CHECK_EQ(pex.size(), size_t(2));
}

// 14. .esp, .esm and .esl names are all recognised.
TEST(references_esp_esm_esl)
{
    auto refs = ReferenceScanner::inspect(kNames, "mcm\\config\\x\\config.json",
                                          bytesOf("{\"pluginRequirements\": [\"ModM.esm\", \"ModL.esl\"],\n"
                                                  " \"form\": \"ModS.esp|800\"}\n"));
    std::set<std::pair<PluginIndex, std::string>> found;
    for (const auto& r : refs)
        found.insert({r.plugin, r.context});
    CHECK(found == (std::set<std::pair<PluginIndex, std::string>>{
                       {3, "MCM pluginRequirements"}, {4, "MCM pluginRequirements"}, {1, "Plugin|FormID pair"}}));
    for (const auto& r : refs)
        CHECK_EQ(r.cls, ReferenceClass::Rewritable);
    std::string dll = std::string("\0", 1) + "ModL.esl" + std::string("\0", 1) + "ModM.esm" + std::string("\0", 1);
    CHECK_EQ(ReferenceScanner::inspect(kNames, "f4se\\plugins\\x.dll", bytesOf(dll)).size(), size_t(2));
}
