#pragma once

#include "esp/PluginFile.hpp"

#include <string>
#include <vector>

namespace fo4mx {

// Checks the nested group writer against a real plugin (cells, worldspaces, dialogue topics):
//   - placement: every cell is in the block and sub-block the writer computes for it, and every
//     child is in its cell's persistent or temporary group;
//   - rebuild (when `rebuild`): the plugin is written again by PluginWriter and read back; every
//     record must come back with the same content and place, and the file the same groups.
// Group order may differ (the writer orders like xEdit); that is reported, not a problem.
struct LayoutCheck {
    bool nested = false;          // the plugin has CELL, WRLD or DIAL groups
    bool rebuilt = false;         // the full rebuild ran
    size_t records = 0, groups = 0;
    size_t cells = 0, children = 0, topicChildren = 0;
    bool sameOrder = false;       // rebuild: records and groups in the same order as the original
    std::vector<std::string> problems;
};

LayoutCheck checkLayout(const PluginFile& file, bool rebuild);

} // namespace fo4mx
