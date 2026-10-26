#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fo4mx {

class CancelledError : public std::runtime_error {
public:
    CancelledError() : std::runtime_error("operation cancelled") {}
};

// Receives progress from long-running engine work. All methods may be called
// from a worker thread; implementations must be thread-safe.
class ProgressSink {
public:
    virtual ~ProgressSink() = default;
    virtual void stage(std::string_view /*name*/) {}
    virtual void progress(uint64_t /*done*/, uint64_t /*total*/, std::string_view /*detail*/) {}

    void requestCancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
    void throwIfCancelled() const
    {
        if (cancelled_)
            throw CancelledError();
    }

private:
    std::atomic<bool> cancelled_{false};
};

// Shared do-nothing sink so engines never need null checks.
inline ProgressSink& nullProgress()
{
    static ProgressSink sink;
    return sink;
}

} // namespace fo4mx
