#pragma once

#include <cstddef>

namespace fo4mx {

class Session;
struct CompiledOutput;

// Plugin slots: Fallout 4 loads at most 254 full plugins (00..FD) and 4,096 light ones (FE 000..FFF).
// The reason to compile at all, so the Dashboard shows them now and after the build.
struct SlotUsage {
    static constexpr size_t kFullLimit = 254;
    static constexpr size_t kLightLimit = 4096;
    size_t full = 0, light = 0;
};

// The load order as the game has it now (active plugins, this app's earlier outputs included).
SlotUsage slotsNow(const Session& session);
// After switching to `compiled`: compiled plugins gone, earlier outputs replaced by this build's (its
// outputs, patch, archive loaders and placeholders); redirected copies replace their plugin one for one.
SlotUsage slotsAfter(const Session& session, const CompiledOutput& compiled);

} // namespace fo4mx
