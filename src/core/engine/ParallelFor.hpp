#pragma once

#include "util/Progress.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fo4mx {

inline unsigned defaultThreadCount()
{
    unsigned n = std::thread::hardware_concurrency();
    return std::clamp(n == 0 ? 4u : n, 1u, 16u);
}

// Runs fn(i) for i in [0, count) on a small pool. The first exception is
// rethrown on the calling thread after all workers stop.
inline void parallelFor(size_t count, const std::function<void(size_t)>& fn, ProgressSink& progress = nullProgress(),
                        unsigned threads = 0)
{
    if (count == 0)
        return;
    if (threads == 0)
        threads = defaultThreadCount();
    threads = static_cast<unsigned>(std::min<size_t>(threads, count));

    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto worker = [&] {
        while (true) {
            if (progress.cancelled())
                return;
            size_t i = next.fetch_add(1);
            if (i >= count)
                return;
            try {
                fn(i);
            } catch (...) {
                std::lock_guard lock(errorMutex);
                if (!error)
                    error = std::current_exception();
                next = count;
                return;
            }
        }
    };
    if (threads == 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back(worker);
        for (auto& t : pool)
            t.join();
    }
    if (error)
        std::rethrow_exception(error);
    progress.throwIfCancelled();
}

} // namespace fo4mx
