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
// Group order may differ (the writer orders like xEdit); that is reported, not a problem. Empty
// groups (the Creation Kit leaves some) are not written and not compared.
struct LayoutCheck {
    bool nested = false;          // the plugin has CELL, WRLD or DIAL groups
    bool rebuilt = false;         // the full rebuild ran
    size_t records = 0, groups = 0;
    size_t cells = 0, children = 0, topicChildren = 0;
    bool sameOrder = false;       // rebuild: records in the same order as the original
    size_t emptyGroups = 0;       // groups holding no record: left out by the writer, no effect in game
    size_t repeatedTopGroups = 0; // top-level groups the plugin repeats: written once
    std::vector<std::string> problems; // differences between the plugin and what the writer makes of it
    std::vector<std::string> defects;  // faults of the plugin itself (duplicate FormIDs, an exterior cell
                                       // without its grid): no rebuild
};

LayoutCheck checkLayout(const PluginFile& file, bool rebuild);

} // namespace fo4mx
