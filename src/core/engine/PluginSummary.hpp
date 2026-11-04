#pragma once

#include <cstddef>
#include <string_view>

namespace fo4mx {

class Session;

// Counts of plugins by file type. The type comes from the analysis (PluginKind, stored as
// Plugins.type): the file is an ESL, ESP or ESM; an ESP carrying the ESL flag is counted as an
// "ESL-flagged ESP", and an ESM carrying it stays an ESM (eslFlaggedEsm says how many of them).
struct PluginTypeCounts {
    size_t total = 0;
    size_t esl = 0;
    size_t eslFlaggedEsp = 0;
    size_t esp = 0;
    size_t esm = 0;           // includes eslFlaggedEsm
    size_t eslFlaggedEsm = 0; // ESM files with the ESL flag (part of esm)
    size_t other = 0;         // a type name this summary does not know (should stay 0)

    void add(std::string_view typeName);
};

// The Plugins tab summary: every analysed plugin is either Eligible (it will be compiled) or not.
// "Ineligible" here is everything else, with the analysis' reasons kept apart: Ineligible (holds
// something the compiler cannot reproduce), Retained (base game, Creation Club, excluded), Inactive
// (not enabled) and Error (unreadable, missing masters). eligible.total + ineligible.total == total.
struct PluginSummary {
    size_t total = 0;
    PluginTypeCounts eligible, ineligible;
    size_t notEligibleIneligible = 0, retained = 0, inactive = 0, error = 0, otherStatus = 0;

    // One plugin, by the names the analysis stores (pluginKindName, eligibilityName).
    void add(std::string_view typeName, std::string_view eligibilityName, size_t count = 1);
};

PluginSummary summarisePlugins(const Session& session);

} // namespace fo4mx
