#pragma once

#include <filesystem>
#include <memory>

#include <spdlog/spdlog.h>

namespace fo4mx::log {

// Configure the default logger: console plus an optional rotating log file.
void init(const std::filesystem::path& logFile = {}, bool verbose = false);

// Attach an extra sink (the GUI uses this to stream log lines into the UI).
void addSink(std::shared_ptr<spdlog::sinks::sink> sink);
void removeSink(const std::shared_ptr<spdlog::sinks::sink>& sink);

} // namespace fo4mx::log
