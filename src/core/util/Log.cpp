#include "util/Log.hpp"

#include <algorithm>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace fo4mx::log {

void init(const std::filesystem::path& logFile, bool verbose)
{
    std::vector<spdlog::sink_ptr> sinks;
    auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    console->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    sinks.push_back(console);

    if (!logFile.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(logFile.parent_path(), ec);
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile.string(), 10 * 1024 * 1024, 3);
        file->set_level(spdlog::level::debug);
        sinks.push_back(file);
    }

    auto logger = std::make_shared<spdlog::logger>("fo4mx", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
}

void addSink(std::shared_ptr<spdlog::sinks::sink> sink)
{
    spdlog::default_logger()->sinks().push_back(std::move(sink));
}

void removeSink(const std::shared_ptr<spdlog::sinks::sink>& sink)
{
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
}

} // namespace fo4mx::log
