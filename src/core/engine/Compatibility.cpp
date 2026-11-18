#include "engine/Compatibility.hpp"

#include "esp/Record.hpp"

#include <algorithm>

namespace fo4mx {

const char* compatClassName(CompatClass c)
{
    switch (c) {
    case CompatClass::Green: return "Green";
    case CompatClass::Yellow: return "Yellow";
    case CompatClass::Red: return "Red";
    case CompatClass::Unknown: return "Unknown";
    }
    return "";
}

const char* compatKindName(CompatKind k)
{
    switch (k) {
    case CompatKind::Independent: return "Independent";
    case CompatKind::Competing: return "Competing";
    case CompatKind::Dependent: return "Dependent";
    case CompatKind::Ambiguous: return "Ambiguous";
    case CompatKind::Complex: return "Complex";
    case CompatKind::Unsupported: return "Unsupported";
    }
    return "";
}

namespace {

// Record types whose structure has spatial, quest, dialogue or package meaning (research report, 8.2):
// never carried automatically.
bool redRecord(Signature s)
{
    static const Signature list[] = {
        Signature("QUST"), Signature("DIAL"), Signature("INFO"), Signature("SCEN"), Signature("DLBR"),
        Signature("DLVW"), Signature("PACK"), Signature("NAVM"), Signature("NAVI"), Signature("CELL"),
        Signature("WRLD"), Signature("LAND"), Signature("REFR"), Signature("ACHR"), Signature("PGRE"),
        Signature("PMIS"), Signature("PARW"), Signature("PBEA"), Signature("PFLA"), Signature("PCON"),
        Signature("PBAR"), Signature("PHZD"), Signature("SMQN"), Signature("SMBN"), Signature("SMEN")};
    return std::find(std::begin(list), std::end(list), s) != std::end(list);
}

// Field kinds with no validated element-level merge rule (scripts, conditions, object templates, object
// modification data, perk entries, navmesh data, package data, dialogue data).
const char* redKind(FieldKind k)
{
    switch (k) {
    case FieldKind::Vmad: return "scripts (VMAD)";
    case FieldKind::Condition: return "conditions";
    case FieldKind::ObjectTemplate: return "object template";
    case FieldKind::OmodData: return "object modification data";
    case FieldKind::PerkData:
    case FieldKind::PerkEntryData: return "perk entries";
    case FieldKind::Navmesh:
    case FieldKind::PrecutMap:
    case FieldKind::NavmeshInfo:
    case FieldKind::PrecomputedPathing: return "navmesh data";
    case FieldKind::PackageValue:
    case FieldKind::PackageLocation:
    case FieldKind::PackageTarget:
    case FieldKind::PackageInputName: return "package data";
    case FieldKind::ResponseData:
    case FieldKind::SceneHeadtracking:
    case FieldKind::TopicData: return "dialogue data";
    default: return nullptr;
    }
}

const Field* fieldOf(const ChangeEvent& c, const std::vector<FieldSet>& chain)
{
    if (c.version < chain.size())
        if (const Field* f = chain[c.version].find(c.fieldKey))
            return f;
    for (const auto& set : chain) // a removal: the field is gone from the version that removed it
        if (const Field* f = set.find(c.fieldKey))
            return f;
    return nullptr;
}

} // namespace

CompatFinding classifyChange(const ChangeEvent& c, const std::vector<FieldSet>& chain, const RecordDefinitions& defs,
                             const CompatContext& ctx)
{
    CompatFinding f;
    auto set = [&](CompatClass cls, CompatKind kind, std::string rule) {
        f.cls = cls;
        f.kind = kind;
        f.rule = std::move(rule);
        return f;
    };

    // Masters a patch carrying the value would need beyond the winner's (the patch masters the chain's
    // plugins anyway, as xEdit's "copy as override" adds the source plugin).
    bool outsideChain = false;
    for (const auto& k : c.references) {
        if (k.origin == ctx.winner || (ctx.masters && ctx.masters(ctx.winner, k.origin)))
            continue;
        if (std::find(f.requiredMasters.begin(), f.requiredMasters.end(), k.origin) == f.requiredMasters.end())
            f.requiredMasters.push_back(k.origin);
        if (std::find(ctx.chainPlugins.begin(), ctx.chainPlugins.end(), k.origin) == ctx.chainPlugins.end())
            outsideChain = true;
    }

    // Red and Unknown first: what cannot be judged is never judged lower.
    if (c.opaque)
        return set(CompatClass::Red, CompatKind::Unsupported, "undescribed data");
    if (redRecord(ctx.record))
        return set(CompatClass::Red, CompatKind::Complex, "record type " + ctx.record.str() + " (spatial, quest, dialogue or package structure)");
    if (std::any_of(chain.begin(), chain.end(), [](const FieldSet& s) { return (s.flags() & RecordFlags::Deleted) != 0; }))
        return set(CompatClass::Red, CompatKind::Complex, "deleted record");
    if (const Field* field = fieldOf(c, chain)) {
        auto check = [&](const std::vector<NormSub>& subs) -> const char* {
            for (const auto& s : subs)
                if (const char* why = redKind(defs.field(ctx.record, s.sig).kind))
                    return why;
            return nullptr;
        };
        const char* why = check(field->subs);
        for (const auto& item : field->items)
            if (!why)
                why = check(item.subs);
        if (why)
            return set(CompatClass::Red, CompatKind::Complex, why);
    }
    for (const auto& k : c.references)
        if (ctx.exists && !ctx.exists(k))
            return set(CompatClass::Red, CompatKind::Unsupported, "references a record not in the load order");
    if (!ctx.fieldsMerge)
        return set(CompatClass::Unknown, CompatKind::Unsupported, "no field rule for " + ctx.record.str());

    if (c.status == ChangeStatus::Survives)
        return set(CompatClass::Green, CompatKind::Independent, "in the winner");
    if (!c.competesWith.empty())
        return set(CompatClass::Yellow, CompatKind::Competing, "another plugin changed it differently");
    if (c.status != ChangeStatus::OmittedLater)
        return set(CompatClass::Yellow, CompatKind::Ambiguous, "a later plugin made with it as a master differs");
    if (c.fieldKey == "@flags")
        return set(CompatClass::Yellow, CompatKind::Ambiguous, "record flags");
    const Field* field = fieldOf(c, chain);
    if (c.op == ChangeOp::Reorder || (field && listOrderMatters(ctx.record, *field)))
        return set(CompatClass::Yellow, CompatKind::Ambiguous, "ordered list");
    if (c.op == ChangeOp::Remove)
        return set(CompatClass::Yellow, CompatKind::Ambiguous, "a removal (the winner still has the element)");
    if (outsideChain)
        return set(CompatClass::Yellow, CompatKind::Dependent, "needs a plugin outside the record's chain as a master");

    if (!field || !field->isList())
        return set(CompatClass::Green, CompatKind::Independent, "a field no other plugin changed");
    switch (field->merge) {
    case MergeMode::Set: return set(CompatClass::Green, CompatKind::Independent, "an element of a set (xEdit sorts it)");
    case MergeMode::Multiset: return set(CompatClass::Green, CompatKind::Independent, "an entry of a list that keeps repeats");
    case MergeMode::Keyed: return set(CompatClass::Green, CompatKind::Independent, "a keyed element no other plugin changed");
    case MergeMode::Scalar: break;
    }
    return set(CompatClass::Yellow, CompatKind::Ambiguous, "a list with no element rule");
}

} // namespace fo4mx
