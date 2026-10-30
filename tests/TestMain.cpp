#include "Test.hpp"

#include <cstdlib>

#include <spdlog/spdlog.h>

#include <cstring>

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::warn);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0, run = 0;
    for (const auto& t : fo4mx::test::registry()) {
        if (filter && !std::strstr(t.name, filter))
            continue;
        ++run;
        try {
            t.fn();
            std::printf("[ PASS ] %s\n", t.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %s\n         %s\n", t.name, e.what());
            // On GitHub Actions, also as an annotation: readable without the full log.
            if (std::getenv("GITHUB_ACTIONS"))
                std::printf("::error title=%s::%s\n", t.name, e.what());
        }
    }
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed ? 1 : 0;
}
