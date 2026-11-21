#include "engine/SlotUsage.hpp"

#include "engine/CompilationEngine.hpp"
#include "engine/Session.hpp"

namespace fo4mx {

SlotUsage slotsNow(const Session& session)
{
    SlotUsage u;
    for (auto pi : session.loadOrder)
        ++(session.plugins[pi].isLight() ? u.light : u.full);
    return u;
}

SlotUsage slotsAfter(const Session& session, const CompiledOutput& c)
{
    SlotUsage u;
    for (auto pi : session.loadOrder) {
        const auto& p = session.plugins[pi];
        if (p.generated || c.isCompiled(pi))
            continue;
        ++(p.isLight() ? u.light : u.full);
    }
    for (const auto& op : c.outputs)
        if (!op.records.empty())
            ++(op.light ? u.light : u.full);
    u.light += c.archiveLoaders.size(); // ESL-flagged loaders (placeholders among them)
    return u;
}

} // namespace fo4mx
