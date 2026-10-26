#include "esp/RecordDefinitions.hpp"

#include <algorithm>
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

    for (const char* m : {"YNAM", "ZNAM", "ETYP", "BIDS", "BAMT", "EITM", "INRD", "FIMD", "EFID"})
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

    b.record("WEAP", "Weapon", true)
        .field("DNAM", structAt({0}, 0, true)) // ammo; remaining layout scanned heuristically
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

    b.record("BOOK", "Book", true)
        .field("DATA", noRefs())
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

    // Record types analysed for conflicts but not yet compiled (future phases).
    const std::pair<const char*, const char*> future[] = {
        {"NPC_", "Non-Player Character"}, {"QUST", "Quest"},    {"WRLD", "Worldspace"},
        {"CELL", "Cell"},                 {"REFR", "Reference"}, {"ACHR", "Placed NPC"},
        {"CONT", "Container"},            {"DIAL", "Dialogue Topic"}, {"INFO", "Dialogue Response"},
        {"GMST", "Game Setting"},         {"GLOB", "Global"},   {"STAT", "Static"},
        {"MGEF", "Magic Effect"},         {"ENCH", "Enchantment"}, {"LVLN", "Leveled NPC"},
        {"RACE", "Race"},                 {"ARMA", "Armor Addon"}, {"PROJ", "Projectile"},
        {"NAVM", "Navmesh"},              {"LAND", "Landscape"}, {"PGRE", "Placed Grenade"},
        {"PHZD", "Placed Hazard"},        {"SCOL", "Static Collection"}, {"TERM", "Terminal"},
        {"CMPO", "Component"},            {"AVIF", "Actor Value"}, {"INNR", "Instance Naming Rules"},
        {"MSWP", "Material Swap"},        {"OTFT", "Outfit"},   {"ACTI", "Activator"},
        {"FURN", "Furniture"},            {"DOOR", "Door"},     {"LIGH", "Light"},
        {"SNDR", "Sound Descriptor"},     {"PACK", "Package"},  {"SCEN", "Scene"},
        {"LCTN", "Location"},             {"IDLE", "Idle Animation"}, {"TXST", "Texture Set"},
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
        return d;
    }();
    static const RecordDefinitions nextGen = [] {
        // Next Gen changes several layouts (e.g. WEAP DAMA gains a curve
        // table). Until those are described, Next Gen shares the Old Gen
        // definitions and relies on size validation to catch differences.
        RecordDefinitions d;
        buildOldGen(d);
        d.profileName_ = "Fallout 4 Next Gen (experimental, Old Gen layouts)";
        return d;
    }();
    return profile == RuntimeProfile::NextGen ? nextGen : oldGen;
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
