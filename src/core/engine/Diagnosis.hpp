#pragma once

#include "engine/Session.hpp"

#include <string>

// Plain-language "likely cause" explanations for findings, so a user can tell what probably went
// wrong in a mod (or in the app's understanding of it) and what to do about it.
namespace fo4mx::diagnosis {

// A reference from a record of `source` that cannot be found.
//   target: the referenced record (invalid when its master index is past the master list)
//   raw:    the FormID as stored in `source` (used when `target` is invalid)
//   sameTarget: how many missing references go from `source` to the same plugin (0 = unknown)
std::string missingReference(const Session& session, PluginIndex source, FormKey target, uint32_t raw = 0,
                             size_t sameTarget = 0);

// A general explanation for a finding category (Missing Assets, Archive Priority, ...); empty
// when there is nothing useful to add beyond the message.
std::string forCategory(const std::string& category, const std::string& message);

} // namespace fo4mx::diagnosis
