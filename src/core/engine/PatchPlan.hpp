#pragma once

#include "engine/ConflictEngine.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <vector>

namespace fo4mx {

// The reviewable compatibility patch plan (docs/COMPATIBILITY_PATCH_RESEARCH.md, phase 7). Built from the
// traced and classified changes the winning versions do not hold: per record, start from the winner and
// say, change by change, whether it would be carried into a patch, kept as the winner has it, or waits for
// the user. A plan only: nothing is written from it (that is phase 9).

enum class PlanAction : uint8_t {
    CarryForward, // bring the change into the patch
    KeepWinner,   // leave the winner's value (the change stays lost)
};
const char* planActionName(PlanAction a);

enum class PlanDecision : uint8_t {
    Auto,    // the class's default (Green: carried)
    User,    // the user's saved choice
    Pending, // Yellow without a choice: kept as the winner has it until the user decides
    Locked,  // Red or Unknown: never carried; a saved choice to carry is ignored
};
const char* planDecisionName(PlanDecision d);

struct PlanEntry {
    size_t change = 0; // index into the conflict entry's changes
    PlanAction action = PlanAction::KeepWinner;
    PlanDecision decision = PlanDecision::Pending;
    std::string note;
};

enum class PlanStatus : uint8_t {
    Ready,         // something to carry, every choice made, no problem found
    NeedsDecision, // a Yellow change waits for the user
    Blocked,       // carried changes contradict each other, or the masters do not fit
    NothingToCarry,
};
const char* planStatusName(PlanStatus s);

struct PlanRecord {
    const ConflictEntry* conflict = nullptr; // the analysed record (key, chain, winner, changes)
    std::vector<PlanEntry> entries;          // one per lost change
    std::vector<PluginIndex> masters;        // beyond the winner's, for the changes carried
    PlanStatus status = PlanStatus::NothingToCarry;
    std::vector<std::string> problems;
};

struct PatchPlan {
    std::vector<PlanRecord> records; // records with at least one lost change
    size_t carried = 0, kept = 0, pending = 0, locked = 0;
    std::array<size_t, 4> byStatus{}; // PlanStatus
    std::vector<PluginIndex> masters; // every plugin a patch carrying the plan needs beyond the winners'
};

// Saved user choices, keyed by record ("Plugin.esp:objectID" of the defining plugin) and change id, so they
// survive rescans and load order changes. Stored as JSON.
struct PlanDecisions {
    std::map<std::string, PlanAction> choices; // recordId + "|" + changeId -> action

    static std::string recordId(const Session& session, FormKey key);
    std::optional<PlanAction> find(const std::string& recordId, const std::string& changeId) const;
    void set(const std::string& recordId, const std::string& changeId, std::optional<PlanAction> action);

    static PlanDecisions load(const std::filesystem::path& path); // empty when missing or unreadable
    void save(const std::filesystem::path& path) const;
};

// `masterLimit`: masters a patch plugin may have (one patch per plan for now).
PatchPlan buildPatchPlan(const Session& session, const ConflictReport& conflicts, const PlanDecisions& decisions,
                         size_t masterLimit = 254);

} // namespace fo4mx
