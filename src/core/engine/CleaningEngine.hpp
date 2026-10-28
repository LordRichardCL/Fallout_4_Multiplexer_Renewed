#pragma once

#include "engine/ConflictEngine.hpp"
#include "engine/DependencyEngine.hpp"
#include "engine/Session.hpp"

namespace fo4mx {

// Mod-cleaning and error checks, reported as in xEdit's documentation (chapter 7): identical to
// master records, deleted references and navmeshes, other deleted records, injected records and
// circular leveled lists. Reporting only: plugins are never edited. Findings in official files
// (base game, DLC, Creation Club) are informational.
class CleaningEngine {
public:
    // Appends findings to `report.issues` (marked `cleaning`). Needs the conflict analysis for ITMs
    // and the record references of the dependency analysis for leveled lists.
    static void analyse(const Session& session, const ConflictReport& conflicts, DependencyReport& report);
};

} // namespace fo4mx
