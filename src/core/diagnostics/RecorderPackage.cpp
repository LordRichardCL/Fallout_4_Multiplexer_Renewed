#include "diagnostics/RecorderPackage.hpp"

#include "util/Zip.hpp"

#include <fstream>

namespace fo4mx::diagnostics {

namespace fs = std::filesystem;

namespace {
const char* kZipName = "Multiplexer Diagnostic Recorder.zip";
const char* kInstructionsName = "Multiplexer Diagnostic Recorder - Install.txt";
} // namespace

std::string recorderInstructions()
{
    return "Multiplexer Diagnostic Recorder (optional)\n"
           "==========================================\n"
           "\n"
           "An F4SE plugin that records a small, fixed set of events while you play, so problems can be traced\n"
           "afterwards: the game, F4SE and recorder versions; F4SE's start-up, game data loading, new game, save\n"
           "loading and saving messages; the active plugins (plugins.txt and Fallout4.ccc) as the game reads them;\n"
           "the F4SE plugin DLLs present and which of them loaded; F4SE's own plugin load results (f4se.log); and\n"
           "whether the game exited normally.\n"
           "\n"
           "It does not change plugins, saves, game objects or gameplay, installs no hooks, uses no game memory\n"
           "addresses and reads no game structures. It is not a crash logger: keep Buffout 4 for crash logs.\n"
           "\n"
           "Requirements\n"
           "------------\n"
           "F4SE: 0.6.23 for Fallout 4 1.10.163 (Old Gen) or 0.7.x for Next Gen. Start the game through F4SE\n"
           "(f4se_loader.exe, or your mod manager's F4SE entry).\n"
           "\n"
           "Install (choose one)\n"
           "--------------------\n"
           "Vortex:   drag \"Multiplexer Diagnostic Recorder.zip\" into the Mods page, then enable and deploy it.\n"
           "MO2:      Install a new mod from archive and pick \"Multiplexer Diagnostic Recorder.zip\"; enable it.\n"
           "Manually: copy Data\\F4SE\\Plugins\\MultiplexerDiagnosticRecorder.dll into\n"
           "          <Fallout 4 folder>\\Data\\F4SE\\Plugins\\. F4SE reads plugins from that folder only:\n"
           "          a copy next to Fallout4.exe is never loaded.\n"
           "\n"
           "Did it load?\n"
           "------------\n"
           "After starting the game, Documents\\My Games\\Fallout4\\F4SE\\MultiplexerDiagnosticRecorder.log says\n"
           "\"MultiplexerDiagnosticRecorder ... loaded by F4SE\" and \"Main menu reached\". Multiplexer's Diagnostics\n"
           "page shows it too (Check Installation explains what is wrong when it did not load).\n"
           "\n"
           "The logs\n"
           "--------\n"
           "Documents\\My Games\\Fallout4\\F4SE\\MultiplexerDiagnosticRecorder\\session-<date>-<time>-<process>.jsonl,\n"
           "one file per game session; the newest 10 are kept. Open one in Multiplexer (Diagnostics page) or run\n"
           "\"fo4mx diagnose\" to see what it recognises: likely causes, the evidence, and what to check.\n"
           "\n"
           "Sharing a log: paths inside your user folder are written as %USERPROFILE% (so your account name is not\n"
           "in the file), and save files appear by name only. The log does list your plugins and F4SE plugin DLLs.\n"
           "\n"
           "Uninstall\n"
           "---------\n"
           "Disable or remove the mod, or delete Data\\F4SE\\Plugins\\MultiplexerDiagnosticRecorder.dll.\n";
}

RecorderPackage writeRecorderPackage(const fs::path& recorderDll, const fs::path& packageRoot)
{
    RecorderPackage p;
    std::error_code ec;
    if (recorderDll.empty() || !fs::exists(recorderDll, ec)) {
        p.problem = "MultiplexerDiagnosticRecorder.dll was not found next to the application (" +
                    recorderDll.string() + "); it is part of the Windows build";
        return p;
    }
    p.folder = packageRoot / kRecorderFolderName;
    const fs::path plugins = p.folder / "Data" / "F4SE" / "Plugins";
    fs::create_directories(plugins, ec);
    if (ec) {
        p.problem = "could not create " + plugins.string() + ": " + ec.message();
        return p;
    }
    p.dll = plugins / kRecorderDllName;
    fs::copy_file(recorderDll, p.dll, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        p.problem = "could not copy the DLL: " + ec.message();
        return p;
    }
    p.instructions = p.folder / kInstructionsName;
    {
        std::ofstream out(p.instructions, std::ios::binary | std::ios::trunc);
        out << recorderInstructions();
    }
    p.zip = p.folder / kZipName;
    try {
        writeZip(p.zip, p.folder / "Data", "Data/");
    } catch (const std::exception& e) {
        p.problem = std::string("could not write the zip: ") + e.what();
        return p;
    }
    p.written = true;
    return p;
}

void removeRecorderPackage(const fs::path& packageRoot)
{
    const fs::path folder = packageRoot / kRecorderFolderName;
    std::error_code ec;
    fs::remove(folder / "Data" / "F4SE" / "Plugins" / kRecorderDllName, ec);
    fs::remove(folder / kZipName, ec);
    fs::remove(folder / kInstructionsName, ec);
    // Empty folders only: anything else the user put there stays.
    for (const fs::path& f : {folder / "Data" / "F4SE" / "Plugins", folder / "Data" / "F4SE", folder / "Data", folder})
        if (fs::is_directory(f, ec) && fs::is_empty(f, ec))
            fs::remove(f, ec);
}

} // namespace fo4mx::diagnostics
