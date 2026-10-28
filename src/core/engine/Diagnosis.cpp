#include "engine/Diagnosis.hpp"

#include "util/Text.hpp"

namespace fo4mx::diagnosis {

namespace {

bool looksLikePatch(const PluginInfo& p)
{
    std::string key = p.key;
    return key.find("patch") != std::string::npos || key.find("compat") != std::string::npos ||
           (p.newRecords == 0 && p.overrides > 0);
}

} // namespace

std::string missingReference(const Session& session, PluginIndex source, FormKey target, uint32_t raw, size_t sameTarget)
{
    const std::string src = source < session.plugins.size() ? session.plugins[source].name : std::string("The plugin");

    if (!target.valid()) {
        if (masterIndexOf(raw) >= 0xFE)
            return "A load-order FormID (" + formatFormId(raw) + ", FE/FF prefix) was saved into " + src +
                   ". Those only exist while the game runs; a script, console command or tool wrote it into the file. "
                   "The game cannot resolve it, so it reads as none.";
        return src + " uses a FormID from a plugin that is not one of its masters. Usually the reference was added in "
                     "xEdit without adding that master, or a master was removed from the plugin afterwards.";
    }

    const PluginInfo& to = session.plugins[target.origin];
    const std::string id = formatFormId(target.objectId).substr(2);
    if (to.isLight() && target.objectId > 0xFFF)
        return to.name + " is ESL-flagged, so it cannot contain object " + id +
               " (light plugins only use 800-FFF). The reference was made against an older version of " + to.name +
               ", before it was compacted and ESL-flagged. Use a version of " + src + " made for your version of " +
               to.name + ".";
    if (target.origin == source)
        return src + " refers to its own record " + id + ", which it no longer contains. Usually the record was deleted "
                     "or renumbered (for example by ESL compaction) in an update and this reference was left behind. "
                     "The game reads it as none; reporting it to the mod author is the only fix.";
    if (to.baseGame)
        return to.name + " has no record " + id + ". Usually " + src + " was made for a different game version "
               "(the Next-Gen update added, moved and removed base-game records) or the record was removed by an "
               "official update.";
    if (looksLikePatch(session.plugins[source]) || sameTarget >= 5)
        return src + " expects records in " + to.name + " that your installed version of " + to.name +
               " does not have" + (sameTarget > 1 ? " (" + std::to_string(sameTarget) + " references)" : std::string()) +
               ". Most likely " + src + " was made for a different version of " + to.name +
               ": install the versions that match (check both mods' requirements and changelogs).";
    return to.name + " does not contain record " + id + ". Either " + src + " expects a different version of " +
           to.name + ", or the record was removed from " + to.name + " in an update.";
}

std::string forCategory(const std::string& category, const std::string& message)
{
    if (category == "Missing Assets")
        return "The file is not in any enabled mod or archive. Usually a required resource mod or optional file is "
               "not installed or not enabled, or the mod was made for a different version of the asset pack it uses. "
               "The record compiles regardless; the game shows the usual missing-mesh or missing-texture result.";
    if (category == "Archive Priority")
        return "Archives listed in the INI load before every plugin's own archives (DLC included). With the INI method "
               "a compiled mod's archive can lose files it used to win. Mod Organizer 2 avoids this with archive "
               "loaders; otherwise exclude the mod from compilation or provide the file loose.";
    if (category == "Kept Out Automatically")
        return "A record from this plugin contained data the compiler could not translate safely (see the message). "
               "The plugin simply stays in the load order as it is, so nothing is lost; it is compiled again once the "
               "field is understood.";
    if (category == "Broken in Source")
        return "The reference was already broken in the mod itself. The game reads it as none, so the compiled "
               "plugin behaves exactly like the original.";
    if (category == "Missing Masters" || category == "Missing Master")
        return "A plugin needs a master that is not installed, not enabled or loads after it. The game does not start "
               "with a missing master. Install or enable it, or move it above the plugin that needs it.";
    if (category == "Record Decoding" && message.find("master index beyond") != std::string::npos)
        return "The plugin stores a FormID whose master index is past its master list: a reference added in xEdit "
               "without its master, or a load-order FormID saved by a tool. The game reads it as none.";
    if (category == "Record Decoding")
        return "Part of the record does not match the layout the app expects. That part is copied as it is and "
               "scanned for references instead of being decoded.";
    if (category == "Definitions" && message.find("records decoded") != std::string::npos)
        return {}; // a per-type summary line, not a finding
    if (category == "Definitions")
        return "A self-check of the app's record layouts against your files. It never blocks export; send these "
               "findings to the developer so the layout can be described.";
    if (category == "Archive Loaders")
        return "Compiled plugins with archives are replaced by empty ESL plugins of the same name in the output mod, "
               "so their archives keep loading at the same priority. The output mod must override the originals.";
    if (category == "Orphaned Records")
        return "Nothing in the compiled output refers to the record. Usually it is used by a script, a quest or the "
               "game engine directly, which is fine.";
    if (category == "Unreadable Record")
        return "The record could not be read: the plugin is probably damaged or was saved by a broken tool. "
               "Re-download the mod, or open it in xEdit to check.";
    return {};
}

} // namespace fo4mx::diagnosis
