#pragma once

#include <filesystem>
#include <string>

namespace fo4mx::diagnostics {

constexpr const char* kRecorderDllName = "MultiplexerDiagnosticRecorder.dll";
constexpr const char* kRecorderFolderName = "Diagnostic Recorder";

// The optional Diagnostic Recorder, ready to install: written beside the output package (never into
// its Data folder, so installing the package never installs the recorder):
//   <package>\Diagnostic Recorder\Data\F4SE\Plugins\MultiplexerDiagnosticRecorder.dll
//   <package>\Diagnostic Recorder\Multiplexer Diagnostic Recorder.zip   (Data\ at its root)
//   <package>\Diagnostic Recorder\Multiplexer Diagnostic Recorder - Install.txt
struct RecorderPackage {
    bool written = false;
    std::filesystem::path folder, dll, zip, instructions;
    std::string problem; // why it was not written (the DLL is not next to the application, ...)
};

RecorderPackage writeRecorderPackage(const std::filesystem::path& recorderDll, const std::filesystem::path& packageRoot);
// Removes what writeRecorderPackage wrote (the setting was turned off); other files are kept.
void removeRecorderPackage(const std::filesystem::path& packageRoot);

std::string recorderInstructions();

} // namespace fo4mx::diagnostics
