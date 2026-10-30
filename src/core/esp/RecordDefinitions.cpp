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
        rd.layout = compilable;
        defs_.names_[current_] = name;
        if (compilable)
            defs_.compilable_.push_back(current_);
        return *this;
    }
    // Selects an already defined record to add to.
    Builder& edit(const char* sig)
    {
        current_ = S(sig);
        return *this;
    }
    Builder& asset(const char* sub, AssetRole role)
    {
        defs_.records_[current_].assets[S(sub)] = role;
        return *this;
    }
    Builder& field(Signature sub, FieldDef def)
    {
        defs_.records_[current_].fields[sub] = std::move(def);
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
    for (const char* m : {"MODL", "MOD2", "MOD3", "MOD4", "MOD5", "DMDL"})
        d.commonAssets_[S(m)] = AssetRole::Mesh;
    for (const char* m : {"ICON", "MICO", "ICO2", "MIC2"})
        d.commonAssets_[S(m)] = AssetRole::Icon;
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
        .field("DESC", lstring()) // the one DESC read from STRINGS, not DLSTRINGS (xEdit's localization rules)
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

    // Stage 4 candidates, transcribed from xEdit's wbDefinitionsFO4.pas (every subrecord it defines
    // for the type). Whether they compile, and how, is set by the catalogue: they start as Candidate
    // and become Winner (compiled, winning version taken whole) once the definition check confirms
    // them on a real load order.
    b.record("AACT", "Action", true)
        .field("CNAM", noRefs())   // colour
        .field("DNAM", zstring())  // notes
        .field("TNAM", noRefs())   // keyword type
        .field("DATA", formId());  // attraction rule
    b.record("ASPC", "Acoustic Space", true)
        .field("SNAM", formId())   // looping sound
        .field("RDAT", formId())   // region sound
        .field("BNAM", formId())   // environment type (REVB)
        .field("XTRI", noRefs())
        .field("WNAM", noRefs());
    b.record("COLL", "Collision Layer", true)
        .field("BNAM", noRefs())
        .field("FNAM", noRefs())   // debug colour
        .field("GNAM", noRefs())   // flags
        .field("MNAM", zstring())  // name
        .field("INTV", noRefs())
        .field("CNAM", formIdArray()); // collides with
    b.record("SNCT", "Sound Category", true)
        .field("FNAM", noRefs())
        .field("PNAM", formId())   // parent category
        .field("ONAM", formId())   // menu slider category
        .field("VNAM", noRefs())
        .field("UNAM", noRefs())
        .field("MNAM", noRefs())
        .field("CNAM", noRefs());
    b.record("SOPM", "Sound Output Model", true)
        .field("NAM1", noRefs())
        .field("MNAM", noRefs())
        .field("VNAM", noRefs())
        .field("ONAM", noRefs())
        .field("ATTN", noRefs())
        .field("ENAM", formId());  // effect chain (AECH)
    b.record("REVB", "Reverb Parameters", true).field("DATA", noRefs()).field("ANAM", noRefs());
    b.record("LAYR", "Layer", true).field("PNAM", formId());
    b.record("MOVT", "Movement Type", true)
        .field("MNAM", zstring())
        .field("SPED", noRefs())   // speeds (floats; layout depends on form version)
        .field("INAM", noRefs())
        .field("JNAM", noRefs())
        .field("LNAM", noRefs());
    b.record("LGTM", "Lighting Template", true)
        .field("DATA", noRefs())   // colours, fog and fade distances
        .field("DALC", noRefs())   // directional ambient colours
        .field("WGDR", formId());  // god rays
    b.record("GDRY", "God Rays", true).field("DATA", noRefs());
    b.record("LENS", "Lens Flare", true)
        .field("CNAM", noRefs())
        .field("LFSP", noRefs())   // sprite count
        .field("DNAM", zstring())  // sprite ID
        .field("FNAM", zstring())  // texture
        .field("LFSD", noRefs());
    b.record("STAG", "Animation Sound Tag Set", true).field("TNAM", structAt({0})); // sound, then the action text
    b.record("AECH", "Audio Effect Chain", true).field("KNAM", noRefs()).field("DNAM", noRefs());
    b.record("OVIS", "Object Visibility Manager", true).field("INDX", formId()).field("DATA", noRefs());
    b.record("DOBJ", "Default Object Manager", true).field("DNAM", structArray(8, {4})); // (use, object) pairs
    b.record("ANIO", "Animated Object", true).field("BNAM", zstring()); // model subrecords are common

    // Stage 5 candidates, in the order the Blocking Types ranking of a real load order put them, from
    // xEdit's wbDefinitionsFO4.pas. GMST is left out: string settings store localized string IDs
    // depending on their Editor ID, which the field model cannot express.
    b.record("ADDN", "Addon Node", true)
        .field("DATA", noRefs())   // addon index (NIFs refer to it by index)
        .field("SNAM", formId())   // sound
        .field("LNAM", formId())   // light
        .field("DNAM", noRefs());  // particle cap, flags
    b.record("HDPT", "Head Part", true)
        .field("DATA", noRefs())   // flags
        .field("PNAM", noRefs())   // type
        .field("HNAM", formIdList()) // extra parts
        .field("NAM0", noRefs())   // part type
        .field("NAM1", zstring())  // part file name
        .field("TNAM", formId())   // texture set
        .field("CNAM", formId())   // colour
        .field("RNAM", formId())   // valid races (form list)
        .conditionRegion();
    b.record("IMAD", "Image Space Adapter", true).field("DNAM", noRefs());
    // Interpolator arrays only (no FormIDs). YNAM is listed so the common YNAM (a FormID) does not apply.
    for (const char* sub : {"BNAM", "VNAM", "TNAM", "NAM3", "RNAM", "SNAM", "UNAM", "NAM1", "NAM2", "WNAM", "XNAM",
                            "YNAM", "NAM5", "NAM6", "NAM4"})
        b.field(sub, noRefs());
    for (uint32_t i = 0; i <= 0x14; ++i) { // index byte + "IAD": HDR and cinematic multiply / add interpolators
        const uint32_t iad = (uint32_t('I') << 8) | (uint32_t('A') << 16) | (uint32_t('D') << 24);
        b.field(Signature(iad | i), noRefs());
        b.field(Signature(iad | (i + 0x40)), noRefs());
    }
    b.record("LTEX", "Landscape Texture", true)
        .field("TNAM", formId())   // texture set
        .field("MNAM", formId())   // material type
        .field("HNAM", noRefs())   // havok data
        .field("GNAM", formIdList()); // grasses
    b.record("GRAS", "Grass", true).field("DATA", noRefs());
    b.record("IMGS", "Image Space", true)
        .field("ENAM", noRefs())
        .field("HNAM", noRefs())
        .field("CNAM", noRefs())
        .field("TNAM", noRefs())
        .field("DNAM", noRefs())
        .field("TX00", zstring()); // LUT texture
    b.record("BPTD", "Body Part Data", true)
        .field("BPTN", lstring())  // part name
        .field("BPNN", zstring())
        .field("BPNT", zstring())
        // Node data: debris, explosions, impact data sets, actor value and crippling effects.
        .field("BPND", structAt({4, 8, 16, 20, 52, 56, 67, 79, 83, 87, 91}, 101))
        .field("NAM1", zstring())
        .field("NAM4", zstring())
        .field("NAM5", noRefs())   // model info
        .field("ENAM", zstring())
        .field("FNAM", zstring())
        .field("BNAM", formId())   // dismember blood art
        .field("INAM", formId())   // blood impact material
        .field("JNAM", formId())   // cripple blood impact material
        .field("CNAM", formId())   // meat cap texture set
        .field("NAM2", formId())   // collar texture set
        .field("DNAM", zstring()); // twist variable prefix

    // Stage 7a: NPC_, transcribed from xEdit's wbDefinitionsFO4.pas (and the shared wbFaction,
    // wbActorSounds and wbNPCTemplateActorEntry). The catalogue compiles only overrides of it: new
    // NPCs are tied to their plugin by FaceGen data stored under the plugin's name.
    FieldDef npcFaction = structAt({0}); // faction, rank (no padding in Fallout 4)
    npcFaction.merge = MergeMode::Keyed;
    FieldDef spellList = formIdList(MergeMode::Set);
    spellList.countSig = S("SPCT");
    spellList.countWidth = 4;
    FieldDef perk = structAt({0}); // perk, rank
    perk.merge = MergeMode::Keyed;
    perk.countSig = S("PRKZ");
    perk.countWidth = 4;
    b.record("NPC_", "Non-Player Character", true)
        .field("ACBS", noRefs())   // configuration: flags, levels, template flags
        .field("SNAM", npcFaction)
        .field("INAM", formId())   // death item
        .field("VTCK", formId())   // voice type
        .field("TPLT", formId())   // default template
        .field("LTPT", formId())   // legendary template
        .field("LTPC", formId())   // legendary chance (global)
        .field("TPTA", structAt({0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48}, 52)) // template actors
        .field("RNAM", formId())   // race
        .field("SPCT", noRefs())
        .field("SPLO", spellList)  // actor effects
        .field("WNAM", formId())   // skin
        .field("ANAM", formId())   // far away model
        .field("ATKR", formId())   // attack race
        .field("ATKD", structAt({8}, 44)) // attack data: spell at +8
        .field("ATKE", zstring())
        .field("ATKW", formId())   // weapon slot
        .field("ATKS", formId())   // required slot
        .field("ATKT", zstring())
        .field("SPOR", formId())
        .field("OCOR", formId())
        .field("GWOR", formId())
        .field("ECOR", formId())
        .field("FCPL", formId())
        .field("RCLR", formId())   // override package lists
        .field("PRKZ", noRefs())
        .field("PRKR", perk)
        .field("FTYP", formId())   // forced location ref type
        .field("NTRM", formId())   // native terminal
        .field("COCT", noRefs())
        .field("CNTO", cnto)       // inventory
        .field("COED", structAt({0}, 12, true))
        .field("AIDT", noRefs())   // AI data
        .field("PKID", formIdList(MergeMode::Multiset)) // packages (ordered)
        .field("CNAM", formId())   // class
        .field("SHRT", lstring())  // short name
        .field("DATA", noRefs())   // marker
        .field("DNAM", noRefs())   // calculated health, action points, far away distance
        .field("PNAM", formIdList()) // head parts
        .field("HCLF", formId())   // hair colour
        .field("BCLF", formId())   // facial hair colour
        .field("ZNAM", formId())   // combat style
        .field("GNAM", formId())   // gift filter
        .field("NAM5", unknown())  // undecoded in xEdit: scanned
        .field("NAM6", noRefs())
        .field("NAM7", noRefs())
        .field("NAM4", noRefs())   // height
        .field("MWGT", noRefs())   // weight
        .field("NAM8", noRefs())   // sound level
        .field("CS2H", noRefs())
        .field("CS2K", formId())   // sound keyword
        .field("CS2D", formId())   // sound
        .field("CS2E", noRefs())
        .field("CS2F", noRefs())
        .field("CSCR", formId())   // inherits sounds from
        .field("PFRN", formId())   // power armour stand
        .field("DOFT", formId())   // default outfit
        .field("SOFT", formId())   // sleeping outfit
        .field("DPLT", formId())   // default package list
        .field("CRIF", formId())   // crime faction
        .field("FTST", formId())   // head texture
        .field("QNAM", noRefs())   // texture lighting
        .field("MSDK", noRefs())   // morph keys
        .field("MSDV", noRefs())   // morph values
        .field("TETI", noRefs())   // tint layer index
        .field("TEND", noRefs())   // tint layer data
        .field("MRSV", noRefs())   // body morph regions
        .field("FMRI", noRefs())   // face morph index
        .field("FMRS", noRefs())   // face morph values
        .field("FMIN", noRefs())   // morph intensity
        .add(region("Attack", {"ATKD"}, {"ATKD", "ATKE", "ATKW", "ATKS", "ATKT"}))
        .add(region("Sound", {"CS2K"}, {"CS2K", "CS2D"}))
        .add(region("Tint Layer", {"TETI"}, {"TETI", "TEND"}))
        .add(region("Face Morph", {"FMRI"}, {"FMRI", "FMRS"}))
        .modelRegions();

    // WTHR, from xEdit's wbDefinitionsFO4.pas and the shared weather definitions
    // (wbDefinitionsCommon.pas, Fallout 4 branches).
    FieldDef weatherSound = structAt({0}, 8); // sound, type
    weatherSound.merge = MergeMode::Multiset;
    b.record("WTHR", "Weather", true)
        .field("LNAM", noRefs())   // max cloud layers
        .field("MNAM", formId())   // precipitation (SPGD)
        .field("NNAM", formId())   // visual effect (RFCT)
        .field("ONAM", noRefs())
        .field("RNAM", noRefs())
        .field("QNAM", noRefs())   // cloud speeds
        .field("PNAM", noRefs())   // cloud colours
        .field("JNAM", noRefs())   // cloud alphas
        .field("NAM0", noRefs())   // weather colours
        .field("NAM4", noRefs())
        .field("FNAM", noRefs())   // fog distance
        .field("DATA", noRefs())   // wind, transitions, flags, lightning colour
        .field("NAM1", noRefs())   // disabled cloud layers
        .field("SNAM", weatherSound)
        .field("TNAM", formIdList()) // sky statics
        .field("IMSP", structAt({0, 4, 8, 12, 16, 20, 24, 28}, 32)) // image spaces per time of day
        .field("WGDR", structAt({0, 4, 8, 12, 16, 20, 24, 28}, 32)) // god rays per time of day
        .field("DALC", noRefs())   // directional ambient colours
        .field("GNAM", formId())   // sun glare lens flare
        .field("UNAM", structAt({0, 8})) // magic: lightning strike spell, weather activate spell
        .field("VNAM", noRefs())
        .field("WNAM", noRefs())
        .modelRegions(); // aurora model
    // Cloud texture layers: "00TX" .. "?0TX", "@0TX", "A0TX" .. "O0TX" (32 layers).
    for (int i = 0; i < 32; ++i) {
        const char c = static_cast<char>(i < 17 ? '0' + i : 'A' + (i - 17));
        const char sub[5] = {c, '0', 'T', 'X', 0};
        b.field(sub, zstring());
    }

    // Further types requested for analysis, transcribed from xEdit's wbDefinitionsFO4.pas. All start as
    // Candidates: described for conflict analysis and the definition check, compiled only once confirmed.
    b.record("VTYP", "Voice Type", true).field("DNAM", noRefs());
    b.record("ECZN", "Encounter Zone", true).field("DATA", structAt({0, 4}, 12)); // owner, location, levels
    b.record("RFGP", "Reference Group", true)
        .field("NNAM", zstring())
        .field("RNAM", formId())   // reference
        .field("PNAM", formId());  // pack-in
    b.record("PKIN", "Pack-In", true)
        .field("FLTR", zstring())
        .field("CNAM", formId())   // cell
        .field("VNAM", noRefs());
    b.record("CLAS", "Class", true).field("DATA", noRefs()); // FULL, DESC, ICON, PRPS are common
    b.record("CLMT", "Climate", true)
        .field("WLST", structArray(12, {0, 8})) // weather, chance, global
        .field("FNAM", zstring())  // sun texture
        .field("GNAM", zstring())  // sun glare texture
        .field("TNAM", noRefs());  // timing
    b.record("DLBR", "Dialog Branch", true)
        .field("QNAM", formId())   // quest
        .field("TNAM", noRefs())
        .field("DNAM", noRefs())
        .field("SNAM", formId());  // starting topic
    b.record("IDLE", "Idle Animation", true)
        .field("DNAM", zstring())  // behaviour graph
        .field("ENAM", zstring())  // animation event
        .field("ANAM", structAt({0, 4}, 8)) // parent, previous
        .field("DATA", noRefs())
        .field("GNAM", zstring())  // animation file
        .conditionRegion();
    b.record("MUSC", "Music Type", true)
        .field("FNAM", noRefs())
        .field("PNAM", noRefs())
        .field("WNAM", noRefs())
        .field("TNAM", formIdArray(MergeMode::Multiset)); // tracks
    b.record("MUST", "Music Track", true)
        .field("CNAM", noRefs())
        .field("FLTV", noRefs())
        .field("DNAM", noRefs())
        .field("ANAM", zstring())  // track file
        .field("BNAM", zstring())  // finale file
        .field("LNAM", noRefs())   // loop data
        .field("SNAM", formIdArray(MergeMode::Multiset)) // tracks
        .conditionRegion();
    b.record("RELA", "Relationship", true).field("DATA", structAt({0, 4, 12}, 16)); // parent, child, association
    b.record("IDLM", "Idle Marker", true)
        .field("IDLF", noRefs())
        .field("IDLC", noRefs())
        .field("IDLT", noRefs())
        .field("IDLA", formIdArray(MergeMode::Multiset)) // animations
        .field("IDLB", unknown())  // undecoded in xEdit: scanned
        .field("PNAM", formId())   // anim archetype keyword
        .field("QNAM", formId())   // flavour anim keyword
        .modelRegions();
    b.record("TACT", "Talking Activator", true)
        .field("PNAM", unknown())  // undecoded in xEdit: scanned
        .field("SNAM", formId())   // looping sound
        .field("FNAM", unknown())  // undecoded in xEdit: scanned
        .field("VNAM", formId())   // voice type
        .modelRegions();
    b.record("FSTS", "Footstep Set", true)
        .field("XCNT", noRefs())   // counts
        .field("DATA", formIdArray(MergeMode::Multiset)); // footsteps
    b.record("FSTP", "Footstep", true).field("DATA", formId()).field("ANAM", zstring());
    b.record("MATT", "Material Type", true)
        .field("PNAM", formId())   // parent
        .field("MNAM", zstring())
        .field("CNAM", noRefs())
        .field("BNAM", noRefs())
        .field("FNAM", noRefs())
        .field("HNAM", formId())   // havok impact data set
        .field("ANAM", zstring()); // breakable FX
    FieldDef categoryMultiplier = structAt({0}, 8); // category, multiplier
    categoryMultiplier.merge = MergeMode::Keyed;
    b.record("SCSN", "Audio Category Snapshot", true)
        .field("PNAM", noRefs())
        .field("CNAM", categoryMultiplier);
    b.record("BNDS", "Bendable Spline", true).field("DNAM", noRefs()).field("TNAM", formId());
    b.record("SMEN", "Story Manager Event Node", true)
        .field("PNAM", formId())   // parent node
        .field("SNAM", formId())   // previous node
        .field("DNAM", noRefs())
        .field("XNAM", noRefs())
        .field("ENAM", noRefs())   // event type
        .conditionRegion();
    b.record("CAMS", "Camera Shot", true)
        .field("DATA", noRefs())
        .field("MNAM", formId())   // image space modifier
        .conditionRegion()
        .modelRegions();
    b.record("CPTH", "Camera Path", true)
        .field("ANAM", structAt({0, 4}, 8)) // parent, previous
        .field("DATA", noRefs())
        .field("SNAM", formIdList(MergeMode::Multiset)) // camera shots
        .conditionRegion();
    b.record("SPGD", "Shader Particle Geometry", true).field("DATA", noRefs()).field("MNAM", zstring());
    b.record("SCCO", "Scene Collection", true)
        .field("QNAM", formId())   // quest
        .field("SNAM", formId())   // scene
        .field("XNAM", noRefs())   // coordinates
        .field("VNAM", unknown())  // undecoded in xEdit: scanned
        .add(region("Scene", {"SNAM"}, {"SNAM", "XNAM"}));
    b.record("LCTN", "Location", true)
        .field("ACPR", structArray(12, {0, 4})) // persistent references: ref, world/cell, grid
        .field("LCPR", structArray(12, {0, 4}))
        .field("RCPR", formIdArray())
        .field("ACUN", structArray(12, {0, 4, 8})) // unique NPCs: NPC, actor ref, location
        .field("LCUN", structArray(12, {0, 4, 8}))
        .field("RCUN", formIdArray())
        .field("ACSR", structArray(16, {0, 4, 8})) // special references: type, ref, world/cell, grid
        .field("LCSR", structArray(16, {0, 4, 8}))
        .field("RCSR", formIdArray())
        .field("ACEC", structAt({0}))  // worldspace, then cell coordinates
        .field("LCEC", structAt({0}))
        .field("RCEC", structAt({0}))
        .field("ACID", formIdArray())  // initially disabled references
        .field("LCID", formIdArray())
        .field("ACEP", structArray(12, {0, 4})) // enable parents: ref, parent, flags
        .field("LCEP", structArray(12, {0, 4}))
        .field("PNAM", formId())   // parent location
        .field("NAM1", formId())   // music
        .field("FNAM", formId())   // unreported crime faction
        .field("MNAM", formId())   // world location marker
        .field("RNAM", noRefs())
        .field("CNAM", noRefs());
    b.record("CSTY", "Combat Style", true) // numbers and flags only
        .field("CSGD", noRefs())
        .field("CSMD", unknown())  // undecoded in xEdit: scanned
        .field("CSME", noRefs())
        .field("CSCR", noRefs())
        .field("CSLR", noRefs())
        .field("CSFL", noRefs())
        .field("DATA", noRefs());
    b.record("WATR", "Water", true)
        .field("ANAM", noRefs())
        .field("FNAM", noRefs())
        .field("TNAM", formId())   // material
        .field("SNAM", formId())   // open sound
        .field("XNAM", formId())   // consume spell
        .field("YNAM", formId())   // contact spell
        .field("INAM", formId())   // image space
        .field("DATA", noRefs())
        .field("DNAM", noRefs())   // visual data
        .field("GNAM", noRefs())
        .field("NAM0", noRefs())
        .field("NAM1", noRefs())
        .field("NAM2", zstring())  // noise textures
        .field("NAM3", zstring())
        .field("NAM4", zstring());
    b.record("REGN", "Region", true)
        .field("RCLR", noRefs())
        .field("WNAM", formId())   // worldspace
        .field("RPLI", noRefs())
        .field("RPLD", noRefs())   // area points
        .field("RDAT", noRefs())   // data entry header
        .field("RDMO", formId())   // music
        .field("RDSA", structArray(12, {0})) // sounds: sound, flags, chance
        .field("RDMP", lstring())  // map name
        .field("RDOT", structArray(52, {0})) // objects
        .field("RDGS", structArray(8, {0}))  // grasses
        .field("RDWT", structArray(12, {0, 8})) // weathers: weather, chance, global
        .field("RLDM", noRefs())
        .field("ANAM", noRefs())
        .add(region("Region Data Entry", {"RDAT"}, {"RDAT", "ICON", "RDMO", "RDSA", "RDMP", "RDOT", "RDGS", "RDWT", "RLDM", "ANAM"}));
    b.record("DEBR", "Debris", true).field("DATA", noRefs()); // percentage, model file, collision; MODT is common
    // RACE: several subrecords repeat with different roles (empty gender markers, strings); each is
    // described by what it can hold in any position (NAM2: empty marker or the collar texture set).
    b.record("RACE", "Race", true)
        .field("SPCT", noRefs())
        .field("SPLO", spellList)  // actor effects
        .field("WNAM", formId())   // skin
        .field("BOD2", noRefs())   // biped body template
        // Data: severable / explodable explosion, debris, impact data set; crippling; subsegment explosion.
        .field("DATA", structAt({146, 150, 154, 158, 162, 166, 176, 180, 184, 188}, 200))
        .field("MNAM", noRefs())   // male marker
        .field("FNAM", noRefs())   // female marker
        .field("ANAM", zstring())  // skeletal model
        .field("NAM0", noRefs())
        .field("NAM1", noRefs())
        .field("NAM2", structAt({0})) // empty marker, or the collar texture set
        .field("NAM3", noRefs())
        .field("MTNM", zstring())  // movement type names
        .field("VTCK", formIdArray(MergeMode::Multiset)) // voices: male, female
        .field("HCLF", formIdArray(MergeMode::Multiset)) // default hair colours: male, female
        .field("TINL", noRefs())
        .field("PNAM", noRefs())
        .field("UNAM", noRefs())   // FaceGen clamps
        .field("ATKR", formId())   // attack race
        .field("ATKD", structAt({8}, 44))
        .field("ATKE", zstring())
        .field("ATKW", formId())
        .field("ATKS", formId())
        .field("ATKT", zstring())
        .field("INDX", noRefs())   // body / head part index
        .field("GNAM", formId())   // body part data
        .field("NAM4", formId())   // impact material type
        .field("NAM5", formId())   // impact data set
        .field("NAM7", formId())   // dismember blood art
        .field("CNAM", formId())   // meat cap texture set
        .field("ONAM", formId())   // open corpse sound
        .field("LNAM", formId())   // close corpse sound
        .field("NAME", zstring())  // biped object names
        .field("RBPC", formIdArray(MergeMode::Multiset)) // biped object conditions (AVIF)
        .field("MTYP", formId())   // movement type override
        .field("SPED", noRefs())
        .field("VNAM", noRefs())   // equipment flags
        .field("QNAM", formId())   // equip slot
        .field("ZNAM", zstring())  // equip slot node (not the common combat style FormID)
        .field("UNWP", formId())   // unarmed weapon
        .field("PHTN", zstring())
        .field("PHWT", noRefs())   // phoneme targets
        .field("WKMV", formId())
        .field("SWMV", formId())
        .field("FLMV", formId())
        .field("SNMV", formId())   // base movement defaults
        .field("NNAM", noRefs())   // neck fat adjustments
        .field("HEAD", formId())   // head part
        .field("RPRM", formIdList(MergeMode::Multiset)) // race presets
        .field("RPRF", formIdList(MergeMode::Multiset))
        .field("AHCM", formIdList(MergeMode::Multiset)) // hair colours
        .field("AHCF", formIdList(MergeMode::Multiset))
        .field("FTSM", formIdList())  // face details
        .field("FTSF", formIdList())
        .field("DFTM", formId())   // default face texture
        .field("DFTF", formId())
        .field("TTGP", lstring())  // tint group / option name
        .field("TETI", noRefs())
        .field("TTEF", noRefs())
        .field("TTET", zstring())  // tint textures
        .field("TTEB", noRefs())
        .field("TTEC", structArray(14, {0})) // template colours: colour, alpha, index, blend
        .field("TTED", noRefs())
        .field("TTGE", noRefs())
        .field("MPGN", zstring())  // morph groups
        .field("MPPC", noRefs())
        .field("MPPI", noRefs())
        .field("MPPN", lstring())
        .field("MPPM", zstring())
        .field("MPPT", formId())   // morph preset texture
        .field("MPPF", noRefs())
        .field("MPPK", noRefs())
        .field("MPGS", noRefs())
        .field("FMRI", noRefs())   // face morphs
        .field("FMRN", lstring())
        .field("WMAP", zstring())  // wrinkle map
        .field("NAM8", formId())   // morph race
        .field("RNAM", formId())   // armour race
        .field("SRAC", formId())   // subgraph template race
        .field("SADD", formId())   // subgraph additive race
        .field("SAKD", formIdList(MergeMode::Multiset)) // subgraph actor keywords
        .field("SGNM", zstring())
        .field("SAPT", zstring())
        .field("STKD", formIdList(MergeMode::Multiset)) // subgraph target keywords
        .field("SRAF", noRefs())
        .field("PTOP", noRefs())
        .field("NTOP", noRefs())
        .field("MSID", noRefs())
        .field("MSM0", zstring())
        .field("MSM1", zstring())
        .field("MLSI", noRefs())
        .field("HNAM", zstring())  // hair colour lookup texture
        .field("HLTX", zstring())
        .field("QSTI", formId())   // dialogue quest
        .field("BMMP", noRefs())   // bone scale data
        .field("BSMP", noRefs())
        .field("BSMB", zstring())
        .field("BSMS", noRefs())
        .add(region("Attack", {"ATKD"}, {"ATKD", "ATKE", "ATKW", "ATKS", "ATKT"}));
    // SCEN: phases, actors and actions. HTID is an actor ID array, an empty marker or a sound FormID
    // depending on the action type, so it is left undescribed (scanned).
    b.record("SCEN", "Scene", true)
        .field("FNAM", noRefs())
        .field("HNAM", noRefs())   // phase markers
        .field("NAM0", zstring())  // phase / action name
        .field("NEXT", noRefs())
        .field("WNAM", noRefs())
        .field("SCQS", noRefs())   // set parent quest stage
        .field("ALID", noRefs())   // actor (alias) IDs
        .field("LNAM", noRefs())
        .field("DNAM", noRefs())
        .field("ANAM", noRefs())   // action type
        .field("INAM", noRefs())
        .field("SNAM", noRefs())   // start phase / timer
        .field("ENAM", noRefs())
        .field("DATA", formId())   // topic
        .field("DMAX", noRefs())
        .field("DMIN", noRefs())
        .field("CRIS", noRefs())
        .field("DEMO", noRefs())
        .field("DEVA", noRefs())
        .field("HTID", unknown())
        .field("DTID", noRefs())   // NPC headtracking actor IDs
        .field("VENC", formId())
        .field("PLVD", formId())
        .field("JOUT", formId())
        .field("DALC", formId())   // dialogue subtypes (keywords)
        .field("ONAM", formId())   // audio output override
        .field("PNAM", formId())   // parent quest / anim archetype / package
        .field("TNAM", noRefs())
        .field("PTOP", formId())
        .field("NTOP", formId())
        .field("NETO", formId())
        .field("QTOP", formId())   // player responses (topics)
        .field("NPOT", formId())
        .field("NNGT", formId())
        .field("NNUT", formId())
        .field("NQUT", formId())   // NPC responses (topics)
        .field("NPOS", formId())
        .field("NNGS", formId())
        .field("NNUS", formId())
        .field("NQUS", formId())   // NPC response subtypes (keywords)
        .field("DTGT", noRefs())
        .field("STSC", formId())   // topic
        .field("LCEP", formId())   // start scene
        .field("INTT", noRefs())
        .field("SSPN", zstring())
        .field("VNAM", noRefs())   // actor behaviour settings
        .field("CNAM", noRefs())
        .field("ACTV", noRefs())
        .field("NNAM", zstring())  // notes
        .field("XNAM", noRefs())
        .conditionRegion();
    b.record("DIAL", "Dialog Topic", true)
        .field("PNAM", noRefs())   // priority
        .field("BNAM", formId())   // branch
        .field("QNAM", formId())   // quest
        .field("KNAM", formId())   // keyword
        .field("DATA", noRefs())   // flags, category, subtype
        .field("SNAM", noRefs())   // subtype name (4 characters)
        .field("TIFC", noRefs());  // info count
    b.record("INFO", "Dialog response", true)
        .field("ENAM", noRefs())   // response flags
        .field("TPIC", formId())   // previous topic
        .field("PNAM", formId())   // previous INFO
        .field("DNAM", formId())   // shared INFO
        .field("GNAM", formId())   // INFO group
        .field("IOVR", zstring())  // override file name
        .field("TRDA", structAt({0, 5}, 20)) // response data: emotion keyword, sound
        .field("NAM1", FieldDef{FieldKind::ILString}) // response text (dialogue strings)
        .field("NAM2", zstring())
        .field("NAM3", zstring())
        .field("NAM4", zstring())
        .field("SNAM", formId())   // speaker idle
        .field("LNAM", formId())   // listener idle
        .field("TNAM", noRefs())
        .field("NAM9", noRefs())   // text hash
        .field("SRAF", formId())   // camera path
        .field("WZMD", noRefs())
        .field("RNAM", lstring())  // prompt (STRINGS, unlike the other INFO strings)
        .field("ANAM", formId())   // speaker
        .field("TSCE", formId())   // start scene
        .field("ALFA", noRefs())
        .field("ONAM", formId())   // audio output override
        .field("GREE", noRefs())
        .field("TIQS", noRefs())
        .field("NAM0", zstring())  // start scene phase
        .field("INCC", noRefs())   // challenge
        .field("MODQ", formId())   // reset global
        .field("INAM", noRefs())   // subtitle priority
        .add(region("Response", {"TRDA"}, {"TRDA", "NAM1", "NAM2", "NAM3", "NAM4", "SNAM", "LNAM", "TNAM", "NAM9", "SRAF", "WZMD"}))
        .conditionRegion();
    // Large references: cell Y, X, a 32-bit count, then (reference, Y, X) entries.
    FieldDef largeRefs = structArray(8, {0}, MergeMode::Scalar);
    largeRefs.arrayStart = 8;
    b.record("WRLD", "Worldspace", true)
        .field("RNAM", largeRefs)
        .field("MHDT", noRefs())   // max height data
        .field("WCTR", noRefs())   // fixed centre cell
        .field("LTMP", formId())   // interior lighting
        .field("XEZN", formId())   // encounter zone
        .field("XLCN", formId())   // location
        .field("WNAM", formId())   // parent worldspace
        .field("PNAM", noRefs())   // parent flags
        .field("CNAM", formId())   // climate
        .field("NAM2", formId())   // water
        .field("NAM3", formId())   // LOD water
        .field("NAM4", noRefs())
        .field("DNAM", noRefs())   // land data
        .field("ICON", zstring())  // map image
        .field("MNAM", noRefs())   // map data
        .field("ONAM", noRefs())   // map offset
        .field("NAMA", noRefs())
        .field("DATA", noRefs())   // flags
        .field("NAM0", noRefs())
        .field("NAM9", noRefs())   // object bounds
        .field("ZNAM", formId())   // music
        .field("NNAM", zstring())
        .field("XWEM", zstring())
        .field("TNAM", zstring())
        .field("UNAM", zstring())  // HD LOD textures
        .field("WLEV", noRefs())
        .field("OFST", noRefs())
        .field("CLSZ", noRefs())
        .modelRegions(); // cloud model
    b.record("LAND", "Landscape", true)
        .field("DATA", noRefs())
        .field("VNML", noRefs())   // normals
        .field("VHGT", noRefs())   // heights
        .field("VCLR", noRefs())   // vertex colours
        .field("BTXT", structAt({0}, 8)) // base layer: landscape texture, quadrant, layer
        .field("ATXT", structAt({0}, 8)) // alpha layer header
        .field("VTXT", noRefs())   // alpha layer data
        .field("MPCD", noRefs());  // hi-res heightfield

    // Placed references (REFR, ACHR and the placed projectile types), from xEdit's REFR, ACHR and
    // ReferenceRecord definitions. They live inside cells, so they are analysed and checked, never compiled.
    auto placed = [&](const char* sig, const char* name) -> Builder& {
        b.record(sig, name, true)
            .field("NAME", formId())   // base object / projectile
            .field("XEZN", formId())   // encounter zone
            .field("XHTW", noRefs())
            .field("XFVC", noRefs())
            .field("XPWR", structAt({0}))   // reflected / refracted by: water reference, type
            .field("XLKR", structAt({0, 4})) // linked reference: keyword, reference
            .field("XAPD", noRefs())
            .field("XAPR", structAt({0}, 8)) // activate parent: reference, delay
            .field("XASP", formId())   // acoustic parent
            .field("XATP", noRefs())
            .field("XAMC", noRefs())
            .field("XLKT", noRefs())
            .field("XLYR", formId())   // layer
            .field("XMSP", formId())   // material swap
            .field("XRFG", formId())   // reference group
            .field("XCVR", noRefs())
            .field("XESP", structAt({0}, 8)) // enable parent: reference, flags
            .field("XOWN", structAt({0}))    // owner (Fallout 4: owner, unused, no crime)
            .field("XRNK", noRefs())
            .field("XRGD", noRefs())   // ragdoll bones
            .field("XRGB", noRefs())
            .field("XHLT", noRefs())
            .field("XEMI", formId())   // emittance
            .field("XMBR", formId())   // multibound reference
            .field("XIS2", noRefs())
            .field("XIBS", noRefs())
            .field("XLRT", formIdArray()) // location ref types
            .field("XLRL", formId())   // location reference
            .field("XSCL", noRefs())
            .field("XLOD", noRefs())
            .field("XLTW", formIdList()) // lit water
            .field("XPRD", noRefs())   // patrol idle time
            .field("XPPA", noRefs())
            .field("INAM", formId())   // patrol idle / image space
            .field("PDTO", unknown())  // topic reference or 4-character subtype: scanned
            .field("XLCM", noRefs())
            .field("XCNT", noRefs())
            .field("XRDS", noRefs())
            .field("XATR", formId())   // attach ref
            .field("XWCN", noRefs())
            .field("XWCU", noRefs())   // water current velocities
            .field("XPLK", structAt({0})) // spline connection
            .field("XACT", noRefs())   // action flag
            .field("XLOC", structAt({4})) // lock: level, key
            .field("ONAM", noRefs())   // open by default
            .field("DATA", noRefs())   // position, rotation
            .field("MNAM", zstring()); // comments
        return b;
    };
    placed("REFR", "Placed Object")
        .field("XMBO", noRefs())
        .field("XPRM", noRefs())   // primitive
        .field("XPOD", formIdArray(MergeMode::Multiset)) // portals: origin, destination
        .field("XPTL", noRefs())
        .field("XORD", formIdArray(MergeMode::Multiset)) // linked occlusion references
        .field("XOCP", noRefs())
        .field("XRMR", noRefs())
        .field("LNAM", formId())   // room lighting template
        .field("XLRM", formIdList()) // linked rooms
        .field("XMBP", noRefs())
        .field("XLIG", noRefs())   // light data
        .field("XALP", noRefs())
        .field("XTEL", structAt({0, 32})) // teleport: door, position/rotation, flags, transition interior
        .field("XTNM", formId())   // teleport location name
        .field("XRDO", noRefs())   // radio
        .field("XBSD", noRefs())   // spline
        .field("XPDD", noRefs())   // projected decal
        .field("XSPC", formId())   // spawn container
        .field("XLIB", formId())   // leveled item base object
        .field("XTRI", noRefs())   // collision layer index
        .field("XNDP", structAt({0}, 8)) // navmesh door link: navmesh, triangle
        .field("XMRK", noRefs())
        .field("FNAM", noRefs())   // map flags
        .field("TNAM", noRefs())   // map marker type
        .field("XWPG", noRefs())
        .field("XWPN", structAt({0, 4, 8}, 12)) // power grid connection: node, node, line
        .field("XCVL", noRefs())
        .field("XCZR", formId())   // water current reference
        .field("XCZC", formId())   // water current cell
        .field("XCZA", unknown()); // undecoded in xEdit: scanned
    placed("ACHR", "Placed NPC").field("TNAM", formId()); // patrol topic
    for (auto [sig, name] : {std::pair{"PARW", "Placed Arrow"}, {"PBAR", "Placed Barrier"}, {"PBEA", "Placed Beam"},
                             {"PCON", "Placed Cone/Voice"}, {"PFLA", "Placed Flame"}, {"PGRE", "Placed Projectile"},
                             {"PHZD", "Placed Hazard"}, {"PMIS", "Placed Missile"}})
        placed(sig, name);

    // Asset paths beyond the common model and icon subrecords. Names of every other record type
    // come from the catalogue (resources/record_catalogue.csv).
    // In armours and armour addons MODL holds FormIDs (armatures, additional races), not a mesh.
    b.edit("ARMO").asset("MODL", AssetRole::None);
    b.edit("ARMA").asset("MODL", AssetRole::None);
    b.edit("TXST");
    for (const char* t : {"TX00", "TX01", "TX02", "TX03", "TX04", "TX05", "TX06", "TX07"})
        b.asset(t, AssetRole::Texture);
    b.asset("MNAM", AssetRole::Material);
    // BNAM only names the material being replaced; the replacement (SNAM) is what loads.
    b.edit("MSWP").asset("SNAM", AssetRole::Material);
    b.edit("SNDR").asset("ANAM", AssetRole::Sound);
    b.edit("EFSH").asset("NAM7", AssetRole::Texture).asset("NAM8", AssetRole::Texture).asset("NAM9", AssetRole::Texture);
    b.edit("LIGH").asset("NAM0", AssetRole::Texture);
    b.edit("PROJ").asset("NAM1", AssetRole::Mesh);
    b.edit("AMMO").asset("NAM1", AssetRole::Mesh); // shell casing model
}

} // namespace

const RecordDefinitions& RecordDefinitions::get(RuntimeProfile profile)
{
    static const RecordDefinitions oldGen = [] {
        RecordDefinitions d;
        buildOldGen(d);
        d.applyCatalogue(RecordCatalogue::get());
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
        d.applyCatalogue(RecordCatalogue::get());
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
                std::to_string(f.stride) + "+" + std::to_string(f.arrayStart) + "/" + std::to_string(f.requiredSize) + "/" + std::to_string(f.partial) +
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
        text += "#" + r + (rd.compilable ? "+" : "-") + strategyName(strategy(Signature::fromString(r)));
        for (const auto& [k, f] : sortedFields(rd.fields))
            addField(Signature::fromString(k.c_str()), *f);
        std::vector<std::pair<std::string, int>> assets;
        for (const auto& [k, role] : rd.assets)
            assets.emplace_back(k.str(), int(role));
        std::sort(assets.begin(), assets.end());
        for (const auto& [k, role] : assets)
            text += "@" + k + "=" + std::to_string(role);
        for (const auto& reg : rd.regions) {
            text += "[" + reg.name + "]" + reg.end.str();
            for (auto s : reg.start)
                text += "<" + s.str();
            for (auto s : reg.members)
                text += ">" + s.str();
        }
    }
    std::vector<std::pair<std::string, int>> commonAssets;
    for (const auto& [k, role] : commonAssets_)
        commonAssets.emplace_back(k.str(), int(role));
    std::sort(commonAssets.begin(), commonAssets.end());
    for (const auto& [k, role] : commonAssets)
        text += "@" + k + "=" + std::to_string(role);
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
    if (auto e = catalogue_ ? catalogue_->find(s) : nullptr)
        return e->name;
    auto it = names_.find(s);
    return it == names_.end() ? s.str() : it->second;
}

void RecordDefinitions::applyCatalogue(const RecordCatalogue& catalogue)
{
    catalogue_ = &catalogue;
    strategyWithoutLayout_.clear();
    compilable_.clear();
    std::vector<Signature> sigs;
    for (const auto& [s, _] : records_)
        sigs.push_back(s);
    std::sort(sigs.begin(), sigs.end());
    for (Signature s : sigs) {
        auto& rd = records_[s];
        auto e = catalogue.find(s);
        // Only records in their own top-level group can be written: the plugin writer has no nested
        // groups (cells, worldspaces, dialogue topics), so those types never compile.
        rd.compilable = rd.layout && e && e->placement == RecordPlacement::Top &&
                        (e->strategy == MergeStrategy::Specialised || e->strategy == MergeStrategy::Winner);
        if (rd.compilable)
            compilable_.push_back(s);
    }
    for (const auto& e : catalogue.entries())
        if (e.strategy != MergeStrategy::Preserve && !hasLayout(e.sig))
            strategyWithoutLayout_.push_back(e.sig);
}

bool RecordDefinitions::hasLayout(Signature s) const
{
    auto r = record(s);
    return r && r->layout;
}

MergeStrategy RecordDefinitions::strategy(Signature s) const
{
    auto e = catalogue().find(s);
    if (!e || !hasLayout(s))
        return MergeStrategy::Preserve;
    // A described type that cannot be written is analysed and checked, never compiled.
    if (e->placement != RecordPlacement::Top && e->strategy != MergeStrategy::Preserve)
        return MergeStrategy::Candidate;
    return e->strategy;
}

bool RecordDefinitions::overridesOnly(Signature s) const
{
    auto e = catalogue().find(s);
    return e && e->overridesOnly;
}

std::string RecordDefinitions::supportSummary(Signature s) const
{
    auto e = catalogue_ ? catalogue_->find(s) : nullptr;
    if (!e)
        return "Unknown record type: not part of Fallout 4";
    switch (strategy(s)) {
    case MergeStrategy::Specialised: return "Compiled: described field by field";
    case MergeStrategy::Winner:
        return e->overridesOnly ? "Compiled: overrides only (new records keep their plugin), winning version taken whole"
                                : "Compiled: winning version taken whole (fields not merged)";
    case MergeStrategy::Candidate:
        if (e->placement != RecordPlacement::Top)
            return "Described (analysed and checked); not compiled: stored inside cells, worldspaces or dialogue "
                   "topics, which the app does not write";
        return std::string("Candidate: described, waiting for the definition check on a real load order (not compiled "
                           "yet") + (e->overridesOnly ? "; overrides only once enabled)" : ")");
    case MergeStrategy::Preserve: break;
    }
    switch (e->placement) {
    case RecordPlacement::Header: return "File header";
    case RecordPlacement::TopWithChildren: return "Preserved: owns child groups (stays in its plugin)";
    case RecordPlacement::Cell:
    case RecordPlacement::CellChild: return "Preserved: placed in cells (stays in its plugin)";
    case RecordPlacement::TopicChild: return "Preserved: dialogue (stays in its plugin)";
    case RecordPlacement::Top: break;
    }
    if (e->strategy != MergeStrategy::Preserve)
        return std::string("Preserved: marked ") + strategyName(e->strategy) + " but has no layout";
    return "Preserved: not described yet (stays in its plugin)";
}

AssetRole RecordDefinitions::assetRole(Signature rec, Signature sub) const
{
    if (auto r = record(rec)) {
        auto it = r->assets.find(sub);
        if (it != r->assets.end())
            return it->second;
    }
    auto it = commonAssets_.find(sub);
    return it == commonAssets_.end() ? AssetRole::None : it->second;
}

const char* assetRoleName(AssetRole r)
{
    switch (r) {
    case AssetRole::None: return "";
    case AssetRole::Mesh: return "Mesh";
    case AssetRole::Texture: return "Texture";
    case AssetRole::Material: return "Material";
    case AssetRole::Sound: return "Sound";
    case AssetRole::Icon: return "Icon";
    }
    return "";
}

const char* assetRoot(AssetRole r)
{
    switch (r) {
    case AssetRole::None: return "";
    case AssetRole::Mesh: return "meshes\\";
    case AssetRole::Texture:
    case AssetRole::Icon: return "textures\\";
    case AssetRole::Material: return "materials\\";
    case AssetRole::Sound: return "sound\\";
    }
    return "";
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
