#include "esp/RecordDefinitions.hpp"

#include "util/Text.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <initializer_list>

// Field layouts for Fallout 4 1.10.163 (form version 131).
//
// These describe only what the compiler must know: where FormIDs and
// localised strings are. They were written from the community-documented
// record structures (xEdit / UESP). Every structured decoder validates the
// exact byte count it consumes; anything that does not fit is reported and
// scanned heuristically instead of being trusted. The Validation Engine also
// measures how many decoded FormIDs resolve against the real load order, so a
// wrong offset shows up as a "suspect definition" rather than silent damage.

namespace fo4mx {

bool RegionDef::startsWith(Signature s) const
{
    return std::find(start.begin(), start.end(), s) != start.end();
}

bool RegionDef::contains(Signature s) const
{
    return std::find(members.begin(), members.end(), s) != members.end() || s == end;
}

namespace {

Signature S(const char* s)
{
    return Signature::fromString(s);
}

FieldDef noRefs()
{
    return FieldDef{FieldKind::NoRefs};
}
// Explicitly undescribed: overrides a common definition that does not apply to this record.
FieldDef unknown()
{
    return FieldDef{FieldKind::Unknown};
}
FieldDef zstring()
{
    return FieldDef{FieldKind::ZString};
}
FieldDef lstring()
{
    return FieldDef{FieldKind::LString};
}
FieldDef dlstring()
{
    return FieldDef{FieldKind::DLString};
}
FieldDef formId()
{
    FieldDef f{FieldKind::FormId};
    f.requiredSize = 4;
    return f;
}
FieldDef formIdList(MergeMode mode = MergeMode::Set)
{
    FieldDef f = formId();
    f.merge = mode;
    return f;
}
FieldDef formIdArray(MergeMode mode = MergeMode::Set)
{
    FieldDef f{FieldKind::FormIdArray};
    f.merge = mode;
    return f;
}
FieldDef structAt(std::initializer_list<uint16_t> offsets, uint16_t requiredSize = 0, bool partial = false)
{
    FieldDef f{FieldKind::Struct};
    f.offsets = offsets;
    f.requiredSize = requiredSize;
    f.partial = partial;
    return f;
}
FieldDef structArray(uint16_t stride, std::initializer_list<uint16_t> offsets, MergeMode mode = MergeMode::Keyed)
{
    FieldDef f{FieldKind::StructArray};
    f.stride = stride;
    f.offsets = offsets;
    f.merge = mode;
    return f;
}
FieldDef special(FieldKind kind)
{
    return FieldDef{kind};
}

RegionDef region(const char* name, std::initializer_list<const char*> start, std::initializer_list<const char*> members,
                 const char* end = nullptr)
{
    RegionDef r;
    r.name = name;
    for (auto s : start)
        r.start.push_back(S(s));
    for (auto s : members)
        r.members.push_back(S(s));
    if (end)
        r.end = S(end);
    return r;
}

class Builder {
public:
    explicit Builder(RecordDefinitions& defs) : defs_(defs) {}

    Builder& record(const char* sig, const char* name, bool compilable)
    {
        current_ = S(sig);
        auto& rd = defs_.records_[current_];
        rd.signature = current_;
        rd.name = name;
        rd.compilable = compilable;
        defs_.names_[current_] = name;
        if (compilable)
            defs_.compilable_.push_back(current_);
        return *this;
    }
    Builder& field(const char* sub, FieldDef def)
    {
        defs_.records_[current_].fields[S(sub)] = std::move(def);
        return *this;
    }
    Builder& add(RegionDef r)
    {
        defs_.records_[current_].regions.push_back(std::move(r));
        return *this;
    }
    // Common regions shared by most item records.
    Builder& modelRegions()
    {
        add(region("Object Template", {"OBTE"}, {"OBTE", "OBTF", "FULL", "OBTS"}, "STOP"));
        add(region("Model", {"MODL"}, {"MODL", "MODT", "MODC", "MODS", "MODF"}));
        add(region("Destructible", {"DEST"}, {"DEST", "DAMC", "DSTD", "DSTA", "DMDL", "DMDT", "DMDC", "DMDS", "DSTF"}));
        add(region("Icon", {"ICON"}, {"ICON", "MICO"}));
        return *this;
    }
    Builder& effectRegions()
    {
        return add(region("Effects", {"EFID"}, {"EFID", "EFIT", "CTDA", "CIS1", "CIS2"}));
    }
    Builder& conditionRegion()
    {
        return add(region("Conditions", {"CITC", "CTDA"}, {"CITC", "CTDA", "CIS1", "CIS2"}));
    }

private:
    RecordDefinitions& defs_;
    Signature current_;
};

void buildCommon(RecordDefinitions& d)
{
    auto& c = d.common_;
    c[S("EDID")] = zstring();
    c[S("FULL")] = lstring();
    c[S("DESC")] = dlstring();
    c[S("VMAD")] = special(FieldKind::Vmad);
    c[S("OBND")] = noRefs();
    c[S("PTRN")] = formId();
    c[S("XALG")] = noRefs();

    FieldDef kwda = formIdArray(MergeMode::Set);
    kwda.countSig = S("KSIZ");
    kwda.countWidth = 4;
    c[S("KWDA")] = kwda;
    c[S("KSIZ")] = noRefs();

    for (const char* m : {"MODL", "MOD2", "MOD3", "MOD4", "MOD5", "DMDL", "ICON", "MICO", "ICO2", "MIC2"})
        c[S(m)] = zstring();
    for (const char* m : {"MODT", "MO2T", "MO3T", "MO4T", "MO5T", "DMDT", "MODC", "MO2C", "MO3C", "MO4C", "MO5C",
                          "DMDC", "MODF", "MO2F", "MO3F", "MO4F", "MO5F"})
        c[S(m)] = noRefs();
    for (const char* m : {"MODS", "MO2S", "MO3S", "MO4S", "MO5S", "DMDS"})
        c[S(m)] = formId();

    // CUSD crafting use sound and STCP animation sound were confirmed against Fallout4.esm by the definition check.
    for (const char* m : {"YNAM", "ZNAM", "ETYP", "BIDS", "BAMT", "EITM", "INRD", "FIMD", "EFID", "CUSD", "STCP"})
        c[S(m)] = formId();
    c[S("APPR")] = formIdArray(MergeMode::Set);
    c[S("EFIT")] = noRefs();
    c[S("EAMT")] = noRefs();

    c[S("CTDA")] = special(FieldKind::Condition);
    c[S("CIS1")] = zstring();
    c[S("CIS2")] = zstring();
    c[S("CITC")] = noRefs();

    c[S("DEST")] = noRefs();
    c[S("DSTD")] = structAt({8, 12}, 20);
    c[S("DSTA")] = noRefs();
    c[S("DSTF")] = noRefs();
    c[S("DAMC")] = structArray(8, {0});

    c[S("OBTE")] = noRefs();
    c[S("OBTF")] = noRefs();
    c[S("OBTS")] = special(FieldKind::ObjectTemplate);
    c[S("STOP")] = noRefs();

    c[S("DAMA")] = structArray(8, {0});
    c[S("PRPS")] = structArray(8, {0});
    c[S("FLTR")] = zstring();
    c[S("ATTX")] = lstring();
}

void buildConditionFunctions(RecordDefinitions& d)
{
    // function index -> FormID parameters (bit0 = param 1, bit1 = param 2)
    const std::pair<uint16_t, uint8_t> table[] = {
        {14, 1},  // GetValue (actor value form)
        {47, 1},  // GetItemCount
        {58, 1},  // GetStage
        {59, 1},  // GetStageDone
        {69, 1},  // GetIsRace
        {71, 1},  // GetInFaction
        {72, 1},  // GetIsID
        {73, 1},  // GetFactionRank
        {74, 1},  // GetGlobalValue
        {136, 1}, // GetIsReference
        {182, 1}, // GetEquipped
        {214, 1}, // HasMagicEffect
        {277, 1}, // GetBaseValue
        {448, 1}, // HasPerk
        {495, 1}, // GetPermanentValue
        {543, 1}, // GetQuestCompleted
        {560, 1}, // HasKeyword
        {682, 1}, // WornHasKeyword
    };
    // Parameters of functions not listed here are scanned heuristically.
    for (auto [fn, mask] : table)
        d.conditionFormParams_[fn] = mask;
}

void buildOldGen(RecordDefinitions& d)
{
    d.profileName_ = "Fallout 4 1.10.163 (Old Gen)";
    buildCommon(d);
    buildConditionFunctions(d);
    Builder b(d);

    // DNAM: ammo (+0); speed, reload, reach, ranges, attack delay, out-of-range damage, on-hit
    // (+4..+39); skill and resist actor values (+40, +44); flags, capacity, animation type,
    // secondary damage, weight, value, base damage, sound level (+48..+72); attack, attack 2D,
    // attack loop, attack fail, idle, equip, unequip and fast-equip sounds (+73..+104, not 4-byte
    // aligned); accuracy, attack seconds, AP cost, power and stagger numbers (+105..+131).
    // Confirmed on a full load order: base damage + sound level at +67 looked like a FormID in
    // 11,257 of 13,243 weapons, which fixes every offset after it.
    FieldDef weaponData = structAt({0, 40, 44, 73, 77, 81, 85, 89, 93, 97, 101}, 0, true);
    weaponData.plain = {{4, 40}, {48, 73}, {105, 132}};
    // FNAM: animation and rumble timings (+0..+27), projectile count (+28), projectile override
    // (+29, found by the unaligned scan on grenades and custom guns), then pattern and rumble period.
    FieldDef weaponFiring = structAt({29}, 0, true);
    weaponFiring.plain = {{0, 29}};
    b.record("WEAP", "Weapon", true)
        .field("DNAM", weaponData)
        .field("CRDT", structAt({8}, 0, true)) // critical damage and charge multipliers, critical effect spell
        .field("FNAM", weaponFiring)            // firing data: projectile override at +29
        .field("INAM", formId())                // impact data set
        .field("WAMD", formId())                // aim model
        .field("WZMD", formId())                // zoom
        .field("CNAM", formId())                // template weapon
        .field("LNAM", formId())                // NPC add ammo list
        .field("NNAM", zstring())               // embedded weapon node
        .modelRegions();

    b.record("ARMO", "Armor", true)
        .field("MODL", formId()) // armature (ARMA) inside the INDX/MODL list
        .field("INDX", noRefs())
        .field("BOD2", noRefs())
        .field("RNAM", formId())
        .field("DATA", noRefs())
        .field("FNAM", noRefs())
        .field("TNAM", formId())
        .field("BMCT", zstring())
        .add(region("Armatures", {"INDX"}, {"INDX", "MODL"}))
        .add(region("Male World Model", {"MOD2"}, {"MOD2", "MO2T", "MO2C", "MO2S", "MO2F"}))
        .add(region("Female World Model", {"MOD4"}, {"MOD4", "MO4T", "MO4C", "MO4S", "MO4F"}))
        .add(region("Object Template", {"OBTE"}, {"OBTE", "OBTF", "FULL", "OBTS"}, "STOP"))
        .add(region("Destructible", {"DEST"}, {"DEST", "DAMC", "DSTD", "DSTA", "DMDL", "DMDT", "DMDC", "DMDS", "DSTF"}))
        .add(region("Male Icon", {"ICON"}, {"ICON", "MICO"}))
        .add(region("Female Icon", {"ICO2"}, {"ICO2", "MIC2"}));

    b.record("AMMO", "Ammunition", true)
        .field("DATA", noRefs())
        .field("DNAM", structAt({0}, 16)) // projectile, flags, damage, health
        .field("ONAM", lstring())
        .field("NAM1", zstring())
        .field("NAM2", noRefs())
        .modelRegions();

    b.record("OMOD", "Object Modification", true)
        .field("DATA", special(FieldKind::OmodData))
        .field("MNAM", formIdArray(MergeMode::Set)) // target OMOD keywords
        .field("FNAM", formIdArray(MergeMode::Set)) // filter keywords
        .field("LNAM", formId())                    // loose mod
        .field("NAM1", noRefs())
        .modelRegions();

    b.record("COBJ", "Constructible Object", true)
        .field("FVPA", structArray(8, {0})) // components: form + count
        .field("CNAM", formId())            // created object
        .field("BNAM", formId())            // workbench keyword
        .field("FNAM", formIdArray(MergeMode::Set))
        .field("INTV", noRefs())
        .field("ANAM", formId())
        .conditionRegion();

    b.record("KYWD", "Keyword", true)
        .field("CNAM", noRefs()) // colour
        .field("DNAM", zstring())
        .field("TNAM", noRefs())
        .field("DATA", formId()) // attraction rule
        .field("NNAM", lstring());

    // Leveled list entry: level, unused, reference, count, chance none, unused.
    FieldDef lvlo = structAt({4}, 12);
    lvlo.merge = MergeMode::Multiset;
    lvlo.countSig = S("LLCT");
    lvlo.countWidth = 1;
    lvlo.companion = S("COED");
    b.record("LVLI", "Leveled Item", true)
        .field("LVLD", noRefs())
        .field("LVLM", noRefs())
        .field("LVLF", noRefs())
        .field("LVLG", formId())
        .field("LLCT", noRefs())
        .field("LVLO", lvlo)
        .field("COED", structAt({0}, 12, true)) // owner; second field depends on owner type
        .field("LLKC", structArray(8, {0}))
        .field("LVSG", formId())
        .field("ONAM", lstring())
        .modelRegions();

    b.record("FLST", "Form List", true).field("LNAM", formIdList(MergeMode::Set));

    b.record("INGR", "Ingredient", true)
        .field("DATA", noRefs())
        .field("ENIT", noRefs())
        .modelRegions()
        .effectRegions();

    b.record("ALCH", "Aid / Ingestible", true)
        .field("DATA", noRefs())
        .field("ENIT", structAt({8, 16}, 20)) // addiction, consume sound
        .field("DNAM", lstring())             // addiction name
        .modelRegions()
        .effectRegions();

    b.record("MISC", "Misc Item", true)
        .field("DATA", noRefs())
        .field("CVPA", structArray(8, {0})) // components
        .field("CDIX", noRefs())
        .modelRegions();

    // DNAM: flags (+0), then what the book teaches (perk, spell or actor value) at +1; text offsets
    // follow. Found by the unaligned scan on a real load order: magazines' perks at +1.
    FieldDef bookData = structAt({1}, 0, true);
    bookData.plain = {{0, 1}};
    b.record("BOOK", "Book", true)
        .field("DATA", noRefs())
        .field("DNAM", bookData)
        .field("INAM", formId())
        .field("CNAM", dlstring())
        .modelRegions();

    b.record("NOTE", "Note / Holotape", true)
        .field("DNAM", noRefs())
        .field("DATA", noRefs())
        .field("SNAM", formId())
        .field("PNAM", zstring())
        .field("TNAM", dlstring())
        .modelRegions();

    b.record("KEYM", "Key", true).field("DATA", noRefs()).modelRegions();

    FieldDef xnam = structAt({0}, 12);
    xnam.merge = MergeMode::Keyed;
    b.record("FACT", "Faction", true)
        .field("XNAM", xnam)
        .field("DATA", noRefs())
        .field("JAIL", formId())
        .field("WAIT", formId())
        .field("STOL", formId())
        .field("PLCN", formId())
        .field("CRGR", formId())
        .field("JOUT", formId())
        .field("CRVA", noRefs())
        .field("RNAM", noRefs())
        .field("MNAM", lstring())
        .field("FNAM", lstring())
        .field("VEND", formId())
        .field("VENC", formId())
        .field("VENV", noRefs())
        .field("PLVD", special(FieldKind::VendorLocation))
        .add(region("Ranks", {"RNAM"}, {"RNAM", "MNAM", "FNAM", "INAM"}))
        .conditionRegion();

    b.record("PERK", "Perk", true)
        .field("DATA", special(FieldKind::PerkData))
        .field("NNAM", formId())
        .field("SNAM", formId())
        .field("FNAM", zstring())
        .field("PRKE", noRefs())
        .field("PRKC", noRefs())
        .field("EPFT", noRefs())
        .field("EPF2", lstring())
        .field("EPF3", noRefs())
        .field("EPFB", noRefs())
        .field("EPFD", special(FieldKind::PerkEntryData))
        .field("PRKF", noRefs())
        .add(region("Effects", {"PRKE"},
                    {"PRKE", "DATA", "PRKC", "CTDA", "CIS1", "CIS2", "EPFT", "EPF2", "EPF3", "EPFB", "EPFD", "PRKF"}))
        .add(region("Icon", {"ICON"}, {"ICON", "MICO"}))
        .conditionRegion();

    b.record("SPEL", "Spell", true)
        .field("SPIT", structAt({32}, 36)) // casting perk
        .effectRegions();

    b.record("MESG", "Message", true)
        .field("QNAM", formId())
        .field("DNAM", noRefs())
        .field("TNAM", noRefs())
        .field("SNAM", zstring())
        .field("NNAM", lstring())
        .field("ITXT", lstring())
        .field("INAM", formId())
        .add(region("Buttons", {"ITXT"}, {"ITXT", "CTDA", "CIS1", "CIS2"}));

    // ---- Tier 1: base objects that item, armour and gameplay mods add alongside the above.
    // Subrecords that are not listed stay undescribed: they are scanned for FormIDs and the
    // Validation Engine blocks export if one would point at the wrong plugin.

    b.record("ARMA", "Armor Addon", true)
        .field("BOD2", noRefs())                    // biped body template
        .field("RNAM", formId())                    // race
        .field("DNAM", noRefs())                    // priorities, weight slider, detection sound value, weapon adjust
        .field("MODL", formIdList(MergeMode::Set))  // additional races
        .field("NAM0", formId())                    // male skin texture
        .field("NAM1", formId())                    // female skin texture
        .field("NAM2", formId())                    // male skin texture swap list
        .field("NAM3", formId())                    // female skin texture swap list
        .field("SNDD", formId())                    // footstep sound
        .field("ONAM", formId())                    // art object
        .add(region("Male Biped Model", {"MOD2"}, {"MOD2", "MO2T", "MO2C", "MO2S", "MO2F"}))
        .add(region("Female Biped Model", {"MOD3"}, {"MOD3", "MO3T", "MO3C", "MO3S", "MO3F"}))
        .add(region("Male First Person", {"MOD4"}, {"MOD4", "MO4T", "MO4C", "MO4S", "MO4F"}))
        .add(region("Female First Person", {"MOD5"}, {"MOD5", "MO5T", "MO5C", "MO5S", "MO5F"}));

    b.record("OTFT", "Outfit", true).field("INAM", formIdArray(MergeMode::Set));

    b.record("LVLN", "Leveled NPC", true)
        .field("LVLD", noRefs())
        .field("LVLM", noRefs())
        .field("LVLF", noRefs())
        .field("LVLG", formId())
        .field("LLCT", noRefs())
        .field("LVLO", lvlo)
        .field("COED", structAt({0}, 12, true))
        .field("LLKC", structArray(8, {0}))
        .modelRegions();

    // Container item: item, count.
    FieldDef cnto = structAt({0}, 8);
    cnto.merge = MergeMode::Multiset;
    cnto.countSig = S("COCT");
    cnto.countWidth = 4;
    cnto.companion = S("COED");
    b.record("CONT", "Container", true)
        .field("COCT", noRefs())
        .field("CNTO", cnto)
        .field("COED", structAt({0}, 12, true))
        .field("DATA", noRefs()) // flags, weight
        .field("SNAM", formId()) // open sound
        .field("QNAM", formId()) // close sound
        .field("FTYP", formId()) // forced location ref type
        .field("NTRM", formId()) // native terminal
        .field("ONAM", formId()) // filter list (workshop racks, sorters)
        .field("TNAM", formId()) // set on every vanilla container; always names a record
        .modelRegions();

    b.record("CMPO", "Component", true)
        .field("CUSD", formId()) // crafting use sound
        .field("DATA", noRefs()) // auto calc value
        .field("MNAM", formId()) // scrap item
        .field("GNAM", formId()); // mod scrap scalar

    b.record("GLOB", "Global Variable", true).field("FNAM", noRefs()).field("FLTV", noRefs());

    b.record("TXST", "Texture Set", true)
        .field("TX00", zstring())
        .field("TX01", zstring())
        .field("TX02", zstring())
        .field("TX03", zstring())
        .field("TX04", zstring())
        .field("TX05", zstring())
        .field("TX06", zstring())
        .field("TX07", zstring())
        .field("DODT", noRefs()) // decal data
        .field("DNAM", noRefs()) // flags
        .field("MNAM", zstring()); // material

    b.record("MSWP", "Material Swap", true)
        .field("FNAM", zstring()) // tree folder
        .field("BNAM", zstring()) // original material
        .field("SNAM", zstring()) // replacement material
        .field("CNAM", noRefs())  // colour remapping index
        .add(region("Substitution", {"BNAM"}, {"BNAM", "SNAM", "FNAM", "CNAM"}));

    b.record("INNR", "Instance Naming Rules", true)
        .field("UNAM", noRefs()) // target
        .field("VNAM", noRefs()) // names in a ruleset
        .field("WNAM", lstring())
        .field("YNAM", noRefs()) // name index (not the common pick-up sound FormID)
        .add(region("Naming Rules", {"VNAM"}, {"VNAM", "WNAM", "KSIZ", "KWDA", "XNAM", "YNAM"}));

    // ENIT: cost, flags, cast type, amount, target type, enchant type, charge time,
    // base enchantment, worn restrictions.
    b.record("ENCH", "Object Effect", true).field("ENIT", structAt({28, 32}, 36)).effectRegions();

    // MGEF DATA: every FormID slot of the 152-byte effect data (associated item, skill and
    // resist values, light, shaders, actor values, projectile, explosion, art, impact data,
    // dual casting, visuals, equip ability, image space, perk).
    b.record("MGEF", "Magic Effect", true)
        .field("DATA", structAt({8, 12, 16, 24, 32, 36, 68, 72, 76, 88, 92, 96, 100, 108, 116, 120, 124, 128, 132, 136},
                                152, true))
        .field("ESCE", formIdArray(MergeMode::Scalar)) // counter effects (count is stored in DATA)
        .field("SNDD", structArray(8, {4}, MergeMode::Scalar)) // sounds: type, sound
        .field("DNAM", lstring())                              // magic item description
        .field("MDOB", formId())                               // menu display object
        .conditionRegion();

    b.record("IPDS", "Impact Data Set", true).field("PNAM", structArray(8, {0, 4})); // material type, impact

    b.record("AORU", "Attraction Rule", true).field("AOR2", noRefs());

    // ---- Tier 1, second batch: placeable base objects, projectiles and effects, sounds, UI data.

    // Navmeshes embedded in base objects (workshop pieces) hold vertices and triangles only. A
    // definition check over a full load order found 162,000 values that looked like FormIDs at
    // scattered offsets in 7,500 of them: number patterns, not references.
    const FieldDef embeddedNavmesh = noRefs();

    b.record("STAT", "Static", true)
        .field("NVNM", embeddedNavmesh)
        .field("FTYP", formId())               // forced location ref type
        .field("DNAM", structAt({4}, 0, true)) // max angle, directional material, leaf amplitude/frequency
        .field("MNAM", noRefs())               // distant LOD model names
        .modelRegions();

    b.record("SCOL", "Static Collection", true)
        .field("ONAM", formId())  // part: base object
        .field("DATA", noRefs()) // part: placements (position, rotation, scale)
        .add(region("Part", {"ONAM"}, {"ONAM", "DATA"}))
        .modelRegions();

    b.record("MSTT", "Movable Static", true)
        .field("NVNM", embeddedNavmesh)
        .field("DATA", noRefs()) // flags
        .field("SNAM", formId()) // looping sound
        .modelRegions();

    b.record("ACTI", "Activator", true)
        .field("NVNM", embeddedNavmesh)
        .field("FTYP", formId())                // forced location ref type
        .field("NTRM", formId())                // native terminal
        .field("PNAM", noRefs())                // marker colour
        .field("SNAM", formId())                // looping sound
        .field("VNAM", formId())                // activation sound
        .field("WNAM", formId())                // water type
        .field("FNAM", noRefs())                // flags
        .field("KNAM", formId())                // interaction keyword
        .field("RADR", structAt({0}, 0, true))  // radio: sound model, frequency, volume, flags
        .modelRegions()
        .conditionRegion();

    b.record("FURN", "Furniture", true)
        .field("NVNM", embeddedNavmesh)
        .field("FTYP", formId())
        .field("NTRM", formId())
        .field("PNAM", noRefs())
        .field("FNAM", noRefs())
        .field("KNAM", formId())
        .field("MNAM", noRefs()) // active markers
        .field("WBDT", noRefs()) // workbench type, skill
        .field("FNMK", formId()) // marker keyword
        .field("FNPR", noRefs()) // entry points
        .field("XMRK", zstring()) // marker model
        .field("COCT", noRefs())
        .field("CNTO", cnto)     // items (power armour stations)
        .field("COED", structAt({0}, 12, true))
        .field("NAM0", formId())
        .field("NAM1", formId())
        .field("WNAM", formId())
        .modelRegions()
        .conditionRegion();

    b.record("DOOR", "Door", true)
        .field("NTRM", formId())
        .field("SNAM", formId()) // open sound
        .field("ANAM", formId()) // close sound
        .field("BNAM", formId()) // loop sound
        .field("FNAM", noRefs()) // flags
        .field("ONAM", lstring()) // alternate text: open
        .field("CNAM", lstring()) // alternate text: close
        .modelRegions();

    b.record("LIGH", "Light", true)
        .field("DATA", noRefs()) // time, radius, colour, flags, falloff, FOV, clip and flicker numbers
        .field("FNAM", noRefs())  // fade value
        .field("NAM0", zstring()) // gobo texture
        .field("LNAM", formId())  // lens flare
        .field("WGDR", formId())  // god rays
        .field("SNAM", formId())  // sound
        .modelRegions();

    b.record("TERM", "Terminal", true)
        .field("PNAM", noRefs())
        .field("FNAM", noRefs())
        .field("BSIZ", noRefs())
        .field("BTXT", lstring()) // body text
        .field("ISIZ", noRefs())
        .field("ITXT", lstring()) // menu item text
        .field("RNAM", lstring()) // menu item response
        .field("ANAM", noRefs())  // menu item type
        .field("ITID", noRefs())  // menu item id
        .field("TNAM", formId())  // submenu terminal
        .field("WNAM", lstring()) // welcome text
        .field("COCT", noRefs())
        .field("CNTO", cnto) // items (holotapes)
        .field("COED", structAt({0}, 12, true))
        .add(region("Body Text", {"BTXT"}, {"BTXT", "CTDA", "CIS1", "CIS2"}))
        .add(region("Menu Items", {"ITXT"}, {"ITXT", "RNAM", "ANAM", "ITID", "TNAM", "CTDA", "CIS1", "CIS2"}))
        .modelRegions();

    // DNAM: flags, type, gravity, speed, range, light, muzzle flash light, alt-trigger proximity and
    // timer, explosion, sound, muzzle flash duration, fade duration, impact force, countdown sound,
    // disable sound, default weapon source, cone spread, collision radius, lifetime, relaunch
    // interval, decal data, collision layer; later bytes are scanned.
    FieldDef projectileData = structAt({16, 20, 32, 36, 52, 56, 60, 80, 84}, 0, true);
    projectileData.plain = {{0, 16}}; // flags, type, gravity, speed, range
    b.record("PROJ", "Projectile", true)
        .field("DNAM", projectileData)
        .field("NAM1", zstring()) // muzzle flash model
        .modelRegions();

    // DATA: light, sound 1, sound 2, impact data set, placed object, spawn projectile; then numbers.
    b.record("EXPL", "Explosion", true)
        .field("DATA", structAt({0, 4, 8, 12, 16, 20}, 0, true))
        .field("MNAM", formId()) // image space modifier
        .modelRegions();

    b.record("IPCT", "Impact", true)
        .field("DODT", noRefs()) // decal data
        .field("DNAM", formId()) // texture set
        .field("ENAM", formId()) // secondary texture set
        .field("SNAM", formId()) // sound 1
        .field("NAM1", formId()) // sound 2
        .field("NAM2", formId()) // hazard
        .modelRegions();

    // DNAM: limit, radius, lifetime, image space radius, target interval, flags, spell, light,
    // impact data set, sound; later bytes are scanned.
    b.record("HAZD", "Hazard", true)
        .field("DNAM", structAt({24, 28, 32, 36}, 0, true))
        .field("MNAM", formId()) // image space modifier
        .modelRegions();

    b.record("SNDR", "Sound Descriptor", true)
        .field("DNAM", structAt({0}, 0, true)) // a FormID at +0 in 2,961 of 3,079 vanilla descriptors
        .field("GNAM", formId()) // category
        .field("SNAM", formId()) // alternate sound for
        .field("ANAM", zstring()) // sound file
        .field("ONAM", formId()) // output model
        .conditionRegion();

    b.record("SOUN", "Sound Marker", true).field("SDSC", formId()).field("REPT", noRefs());

    b.record("AVIF", "Actor Value", true)
        .field("ANAM", lstring()) // abbreviation
        .field("NAM0", noRefs())  // default value
        .field("AVFL", noRefs())  // flags
        .field("NAM1", noRefs()); // type

    b.record("EQUP", "Equip Type", true)
        .field("PNAM", formIdArray(MergeMode::Set)) // slot parents
        .field("DATA", noRefs()); // flags

    b.record("LSCR", "Load Screen", true)
        .field("NNAM", formId()) // loading screen model (static)
        .field("TNAM", formId()) // transform
        .field("ZNAM", unknown()) // 8 bytes here, not the common put-down sound
        .conditionRegion();

    b.record("DMGT", "Damage Type", true).field("DNAM", structArray(8, {0, 4}, MergeMode::Scalar)); // actor value, spell

    b.record("ZOOM", "Zoom", true).field("GNAM", structAt({8}, 24)); // FOV, overlay, image space, offset

    b.record("AMDL", "Aim Model", true).field("DNAM", noRefs()); // cone of fire and recoil numbers

    b.record("ARTO", "Art Object", true).field("DNAM", noRefs()).modelRegions(); // art type

    b.record("EFSH", "Effect Shader", true)
        .field("NAM7", zstring()) // holes texture
        .field("NAM8", zstring()) // membrane palette
        .field("NAM9", zstring()) // particle palette
        .modelRegions();

    b.record("MATO", "Material Object", true).modelRegions();

    b.record("FLOR", "Flora", true)
        .field("PNAM", noRefs())
        .field("FNAM", noRefs())
        .field("PFIG", formId()) // ingredient
        .field("SNAM", formId()) // harvest sound
        .field("PFPC", noRefs()) // seasonal ingredient chances
        .modelRegions();

    b.record("KSSM", "Sound Keyword Mapping", true)
        .field("DNAM", formId())                  // primary descriptor
        .field("ENAM", formId())                  // exterior tail
        .field("VNAM", formId())                  // VATS descriptor
        .field("TNAM", noRefs())                  // VATS threshold
        .field("RNAM", structAt({4}, 8))          // reverb class, sound descriptor
        .field("KNAM", formIdList(MergeMode::Set)); // keywords

    b.record("LCRT", "Location Reference Type", true).field("CNAM", noRefs());

    b.record("RFCT", "Visual Effect", true).field("DATA", structAt({0, 4}, 12)); // art object, effect shader, flags

    b.record("TRNS", "Transform", true).field("DATA", noRefs()); // position, rotation, scale, zoom

    b.record("DFOB", "Default Object", true).field("DATA", formId());

    b.record("CLFM", "Color", true).field("CNAM", noRefs()).field("FNAM", noRefs()).conditionRegion();

    // Record types analysed for conflicts but not yet compiled (future phases).
    const std::pair<const char*, const char*> future[] = {
        // Not described yet
        {"OVIS", "Object Visibility"}, {"PKIN", "Pack-In"}, {"LGTM", "Lighting Template"}, {"IMGS", "Image Space"},
        {"IMAD", "Image Space Modifier"}, {"DEBR", "Debris"}, {"COLL", "Collision Layer"}, {"SNCT", "Sound Category"},
        {"SOPM", "Sound Output Model"}, {"ASPC", "Acoustic Space"}, {"ANIO", "Animated Object"}, {"ADDN", "Addon Node"},
        {"BPTD", "Body Part Data"}, {"VTYP", "Voice Type"}, {"ECZN", "Encounter Zone"}, {"WATR", "Water"},
        {"WTHR", "Weather"}, {"REGN", "Region"}, {"MUSC", "Music Type"}, {"SCCO", "Scene Collection"},
        // Tier 2-4
        {"NPC_", "Non-Player Character"}, {"RACE", "Race"}, {"CLAS", "Class"}, {"PACK", "Package"},
        {"QUST", "Quest"}, {"DIAL", "Dialogue Topic"}, {"INFO", "Dialogue Response"}, {"SCEN", "Scene"},
        {"IDLE", "Idle Animation"}, {"GMST", "Game Setting"}, {"LCTN", "Location"}, {"WRLD", "Worldspace"},
        {"CELL", "Cell"}, {"REFR", "Reference"}, {"ACHR", "Placed NPC"}, {"PGRE", "Placed Grenade"},
        {"PHZD", "Placed Hazard"}, {"NAVM", "Navmesh"}, {"LAND", "Landscape"},
    };
    for (auto [s, n] : future)
        b.record(s, n, false);
}

} // namespace

const RecordDefinitions& RecordDefinitions::get(RuntimeProfile profile)
{
    static const RecordDefinitions oldGen = [] {
        RecordDefinitions d;
        buildOldGen(d);
        d.computeFingerprint();
        return d;
    }();
    static const RecordDefinitions nextGen = [] {
        // Next Gen changes several layouts (e.g. WEAP DAMA gains a curve
        // table). Until those are described, Next Gen shares the Old Gen
        // definitions and relies on size validation to catch differences.
        RecordDefinitions d;
        buildOldGen(d);
        d.profileName_ = "Fallout 4 Next Gen (experimental, Old Gen layouts)";
        d.computeFingerprint();
        return d;
    }();
    return profile == RuntimeProfile::NextGen ? nextGen : oldGen;
}

void RecordDefinitions::computeFingerprint()
{
    std::string text = profileName_;
    auto addField = [&](Signature sub, const FieldDef& f) {
        text += "|" + sub.str() + ":" + std::to_string(int(f.kind)) + "/" + std::to_string(int(f.merge)) + "/" +
                std::to_string(f.stride) + "/" + std::to_string(f.requiredSize) + "/" + std::to_string(f.partial) +
                "/" + f.countSig.str() + std::to_string(f.countWidth) + "/" + f.companion.str();
        for (auto o : f.offsets)
            text += "," + std::to_string(o);
        for (auto [b, e] : f.plain)
            text += "~" + std::to_string(b) + "-" + std::to_string(e);
    };
    auto sortedFields = [](const std::unordered_map<Signature, FieldDef>& m) {
        std::vector<std::pair<std::string, const FieldDef*>> v;
        for (const auto& [k, f] : m)
            v.emplace_back(k.str(), &f);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        return v;
    };
    for (const auto& [k, f] : sortedFields(common_))
        addField(Signature::fromString(k.c_str()), *f);
    std::vector<std::string> recs;
    for (const auto& [k, _] : records_)
        recs.push_back(k.str());
    std::sort(recs.begin(), recs.end());
    for (const auto& r : recs) {
        const auto& rd = records_.at(Signature::fromString(r.c_str()));
        text += "#" + r + (rd.compilable ? "+" : "-");
        for (const auto& [k, f] : sortedFields(rd.fields))
            addField(Signature::fromString(k.c_str()), *f);
        for (const auto& reg : rd.regions) {
            text += "[" + reg.name + "]" + reg.end.str();
            for (auto s : reg.start)
                text += "<" + s.str();
            for (auto s : reg.members)
                text += ">" + s.str();
        }
    }
    std::vector<std::pair<uint16_t, uint8_t>> params(conditionFormParams_.begin(), conditionFormParams_.end());
    std::sort(params.begin(), params.end());
    for (auto [fn, mask] : params)
        text += "!" + std::to_string(fn) + "=" + std::to_string(mask);
    fingerprint_ = fnv1a64(text.data(), text.size());
}

const RecordDef* RecordDefinitions::record(Signature s) const
{
    auto it = records_.find(s);
    return it == records_.end() ? nullptr : &it->second;
}

const FieldDef& RecordDefinitions::field(Signature rec, Signature sub) const
{
    static const FieldDef unknown{};
    if (auto r = record(rec)) {
        auto it = r->fields.find(sub);
        if (it != r->fields.end())
            return it->second;
    }
    auto it = common_.find(sub);
    return it == common_.end() ? unknown : it->second;
}

bool RecordDefinitions::isCompilable(Signature s) const
{
    auto r = record(s);
    return r && r->compilable;
}

std::string RecordDefinitions::recordName(Signature s) const
{
    auto it = names_.find(s);
    return it == names_.end() ? s.str() : it->second;
}

bool RecordDefinitions::conditionParamIsForm(uint16_t function, int param) const
{
    auto it = conditionFormParams_.find(function);
    if (it == conditionFormParams_.end())
        return false;
    return (it->second >> (param - 1)) & 1;
}

bool RecordDefinitions::conditionFunctionKnown(uint16_t function) const
{
    return conditionFormParams_.count(function) != 0;
}

} // namespace fo4mx
