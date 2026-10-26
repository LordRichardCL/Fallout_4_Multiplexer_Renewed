#include "Fixtures.hpp"
#include "Test.hpp"

#include "ba2/Ba2Archive.hpp"
#include "db/Database.hpp"
#include "engine/Pipeline.hpp"
#include "esp/FieldDecoder.hpp"
#include "esp/PluginFile.hpp"

#include <map>

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
                            u32("KSIZ", 1), formIds("KWDA", {0x01000800}), weaponData(0x00000900, 0x22)}),
                    record("COBJ", 0x01000802,
                           {zs("EDID", "co_ModA_Gun"), components({{0x00000900, 5}}), hasPerk(0x00001300),
                            u32("CNAM", 0x01000801), u32("BNAM", 0x00000800), raw("INTV", {1, 0, 0, 0})}),
                });
    // ModA's archive: loads with the plugin and must be registered after compilation.
    writeGeneralBa2(data / "ModA - Main.ba2", {{"meshes\\moda\\gun.nif", std::vector<uint8_t>(64, 7)}}, true);

    writePlugin(data / "ModB.esp", {"Fallout4.esm", "ModA.esp"},
                {
                    record("WEAP", 0x00001000,
                           {zs("EDID", "Pistol10mm"), zs("FULL", "10mm Pistol"), u32("KSIZ", 1), formIds("KWDA", {0x800}),
                            weaponData(0x900, 0x33)}),
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
            CHECK(f.result.find("ModA_Keyword") != std::string::npos && f.result.find("BaseKeyword2") == std::string::npos);

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
    CHECK_EQ(pk->data.size(), size_t(8)); // base keyword + ModA keyword; ModB removed BaseKeyword2
    CHECK(out.resolve(*pistolOut, loadLE<uint32_t>(pk->data.data())) == std::make_pair(std::string("Fallout4.esm"), 0x800u));
    CHECK(out.resolve(*pistolOut, loadLE<uint32_t>(pk->data.data() + 4)) == kwId);
    CHECK_EQ(int(pistolOut->record.find(Signature("KSIZ"))->data[0]), 2);
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
