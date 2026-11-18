#include "engine/PatchPlan.hpp"

#include "util/Json.hpp"
#include "util/Text.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

namespace fo4mx {

const char* planActionName(PlanAction a)
{
    return a == PlanAction::CarryForward ? "Carry forward" : "Keep winner";
}

const char* planDecisionName(PlanDecision d)
{
    switch (d) {
    case PlanDecision::Auto: return "Default";
    case PlanDecision::User: return "Your choice";
    case PlanDecision::Pending: return "Needs your decision";
    case PlanDecision::Locked: return "Not carried";
    }
    return "";
}

const char* planStatusName(PlanStatus s)
{
    switch (s) {
    case PlanStatus::Ready: return "Ready";
    case PlanStatus::NeedsDecision: return "Needs decision";
    case PlanStatus::Blocked: return "Blocked";
    case PlanStatus::NothingToCarry: return "Nothing to carry";
    }
    return "";
}

std::string PlanDecisions::recordId(const Session& session, FormKey key)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), ":%06X", key.objectId);
    return session.pluginName(key.origin) + buf;
}

std::optional<PlanAction> PlanDecisions::find(const std::string& rec, const std::string& change) const
{
    auto it = choices.find(rec + "|" + change);
    if (it == choices.end())
        return std::nullopt;
    return it->second;
}

void PlanDecisions::set(const std::string& rec, const std::string& change, std::optional<PlanAction> action)
{
    if (action)
        choices[rec + "|" + change] = *action;
    else
        choices.erase(rec + "|" + change);
}

PlanDecisions PlanDecisions::load(const std::filesystem::path& path)
{
    PlanDecisions d;
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return d;
    try {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const auto root = json::Value::parse(ss.str());
        if (!root["decisions"].isArray())
            return d;
        for (const auto& v : root["decisions"].asArray()) {
            const std::string rec = v["record"].asString(), change = v["change"].asString(), action = v["action"].asString();
            if (rec.empty() || change.empty())
                continue;
            if (action == "carry")
                d.choices[rec + "|" + change] = PlanAction::CarryForward;
            else if (action == "keep")
                d.choices[rec + "|" + change] = PlanAction::KeepWinner;
        }
    } catch (const std::exception& ex) {
        spdlog::warn("Patch plan decisions {} could not be read: {}", pathToUtf8(path), ex.what());
    }
    return d;
}

void PlanDecisions::save(const std::filesystem::path& path) const
{
    json::Value root;
    root.set("version", 1);
    json::Value list;
    for (const auto& [key, action] : choices) {
        const auto bar = key.rfind('|');
        json::Value v;
        v.set("record", key.substr(0, bar));
        v.set("change", key.substr(bar + 1));
        v.set("action", action == PlanAction::CarryForward ? "carry" : "keep");
        list.push(v);
    }
    root.set("decisions", list);
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << root.dump();
}

PatchPlan buildPatchPlan(const Session& session, const ConflictReport& conflicts, const PlanDecisions& decisions,
                         size_t masterLimit)
{
    PatchPlan plan;
    for (const auto& e : conflicts.entries) {
        PlanRecord pr;
        pr.conflict = &e;
        const std::string rec = PlanDecisions::recordId(session, e.key);
        for (size_t i = 0; i < e.changes.size(); ++i) {
            const auto& t = e.changes[i];
            if (!isLost(t.status))
                continue;
            PlanEntry pe;
            pe.change = i;
            const auto choice = decisions.find(rec, t.changeId);
            switch (t.cls) {
            case CompatClass::Red:
            case CompatClass::Unknown:
                pe.action = PlanAction::KeepWinner;
                pe.decision = PlanDecision::Locked;
                if (choice == PlanAction::CarryForward)
                    pe.note = "your choice to carry it is ignored: " + std::string(compatClassName(t.cls)) +
                              " changes are never carried";
                break;
            case CompatClass::Green:
                pe.action = choice.value_or(PlanAction::CarryForward);
                pe.decision = choice ? PlanDecision::User : PlanDecision::Auto;
                break;
            case CompatClass::Yellow:
                pe.action = choice.value_or(PlanAction::KeepWinner);
                pe.decision = choice ? PlanDecision::User : PlanDecision::Pending;
                break;
            }
            pr.entries.push_back(std::move(pe));
        }
        if (pr.entries.empty())
            continue;

        // Validation of the plan for this record: carried changes must not contradict each other (the same
        // field, or the same list element, given different values), and the masters must fit.
        std::vector<const PlanEntry*> carried;
        for (const auto& pe : pr.entries)
            if (pe.action == PlanAction::CarryForward)
                carried.push_back(&pe);
        for (size_t x = 0; x < carried.size(); ++x)
            for (size_t y = x + 1; y < carried.size(); ++y) {
                const auto& a = e.changes[carried[x]->change];
                const auto& b = e.changes[carried[y]->change];
                if (a.fieldKey != b.fieldKey || a.item != b.item)
                    continue;
                if (a.after != b.after || a.op != b.op)
                    pr.problems.push_back(a.field + (a.item.empty() ? "" : " " + a.item) + ": " +
                                          session.pluginName(a.source) + " and " + session.pluginName(b.source) +
                                          " would both be carried with different values; keep one");
            }
        for (const auto* pe : carried)
            for (auto m : e.changes[pe->change].requiredMasters)
                if (std::find(pr.masters.begin(), pr.masters.end(), m) == pr.masters.end())
                    pr.masters.push_back(m);
        if (e.winner != kInvalidPlugin && session.plugins[e.winner].masters.size() + pr.masters.size() + 1 > masterLimit)
            pr.problems.push_back("a patch carrying it would need more than " + std::to_string(masterLimit) + " masters");

        const bool pending = std::any_of(pr.entries.begin(), pr.entries.end(),
                                         [](const PlanEntry& pe) { return pe.decision == PlanDecision::Pending; });
        pr.status = !pr.problems.empty() ? PlanStatus::Blocked
                    : pending            ? PlanStatus::NeedsDecision
                    : carried.empty()    ? PlanStatus::NothingToCarry
                                         : PlanStatus::Ready;
        for (const auto& pe : pr.entries) {
            if (pe.decision == PlanDecision::Locked)
                plan.locked++;
            else if (pe.decision == PlanDecision::Pending)
                plan.pending++;
            else if (pe.action == PlanAction::CarryForward)
                plan.carried++;
            else
                plan.kept++;
        }
        for (auto m : pr.masters)
            if (std::find(plan.masters.begin(), plan.masters.end(), m) == plan.masters.end())
                plan.masters.push_back(m);
        plan.byStatus[static_cast<size_t>(pr.status)]++;
        plan.records.push_back(std::move(pr));
    }
    return plan;
}

} // namespace fo4mx
