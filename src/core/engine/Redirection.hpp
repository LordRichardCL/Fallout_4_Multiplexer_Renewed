#pragma once

#include "engine/CompilationEngine.hpp"

namespace fo4mx {

// Master Redirection: builds the copy of each plugin in Session::redirected (CompiledOutput::redirected).
// The copy loads after the outputs and reproduces what the game shows today:
// - records only it changes last: its own version, every FormID translated (a compiled plugin's record
//   now lives in an output);
// - overrides the outputs hold, or that a later plugin changes: left out, so the outputs or that plugin
//   win as today; when such a record holds records the copy keeps (a cell its references are in), the
//   record is written as the game shows it today instead;
// - its own new records: kept, with their object IDs (saves keep them).
// A plugin that cannot be copied faithfully gets `failure` and nothing is written for it.
void buildRedirected(Session& session, CompiledOutput& out);

// Reads a redirected copy back from its written bytes: every record and FormID as planned. Empty when it
// matches; otherwise what differs.
std::string checkRedirectedImage(const RedirectedPlugin& r, const std::vector<uint8_t>& image);

// Parses "<kRedirectTag>; loads after: <name>|..." from a copy's description (nullopt: not a copy).
std::optional<std::string> redirectedAfter(const std::string& description);

} // namespace fo4mx
