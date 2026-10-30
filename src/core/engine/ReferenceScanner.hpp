#pragma once

#include "engine/Session.hpp"
#include "util/Progress.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fo4mx {

struct ReferenceScanOptions {
    bool scripts = true; // .pex and .psc
    bool configs = true; // INI, JSON, TOML, XML, TXT under F4SE\Plugins and MCM
    bool binaries = true; // F4SE plugin DLLs
    bool paths = true;   // folders and file names
    unsigned threads = 0;
};

// Finds every place an active plugin's file name appears in the load order's files (loose files
// and archives) and classifies each one by what it is and how it is used, never by how many there
// are. Detectors only record facts (a GetFormFromFile call, a comment line, a "Plugin|FormID" pair,
// a DLL string, a folder); classify() turns each fact into a ReferenceClass and a reason.
class ReferenceScanner {
public:
    // Fills Session::nameReferences.
    static void scan(Session& session, const ReferenceScanOptions& options, ProgressSink& progress = nullProgress());

    // lowercase plugin file name -> plugin, for every active plugin that is not part of the base game.
    using PluginNames = std::unordered_map<std::string, PluginIndex>;
    static PluginNames pluginNames(const Session& session);

    // The file's content as the load order provides it without the Auto-Merger's own copies
    // (Session::relinkedCopies): the winning source that is not such a copy.
    static std::vector<uint8_t> readOriginal(const Session& session, const AssetEntry& entry,
                                             size_t maxBytes = 64u * 1024 * 1024);

    // References in one file's content (also used to re-check a transformed file).
    static std::vector<PluginReference> inspect(const PluginNames& names, const std::string& path,
                                                const std::vector<uint8_t>& bytes, const std::string& source = {});

    // A plugin name in text, used as a whole name ("Armor.esp" is not found in "Power Armor.esp"),
    // with the "|FormID" that may follow it.
    struct Mention {
        size_t begin = 0, end = 0;
        bool pair = false;
        size_t idBegin = 0, idEnd = 0;
        uint32_t formId = 0;
    };
    static std::vector<Mention> mentions(std::string_view text, const std::string& lowerName);

    // Verdict for one plugin: the most restrictive class among its references.
    static ReferenceDecision decide(const std::vector<const PluginReference*>& references);
};

} // namespace fo4mx
