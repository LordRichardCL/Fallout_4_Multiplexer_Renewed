#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fo4mx {

// Interned strings addressed by a 32-bit id. Id 0 is always the empty string.
class StringPool {
public:
    StringPool() { strings_.emplace_back(); }

    uint32_t intern(std::string_view s)
    {
        if (s.empty())
            return 0;
        std::lock_guard lock(mutex_);
        auto it = index_.find(std::string(s));
        if (it != index_.end())
            return it->second;
        uint32_t id = static_cast<uint32_t>(strings_.size());
        strings_.emplace_back(s);
        index_.emplace(strings_.back(), id);
        return id;
    }

    // Not synchronised with intern(); call after concurrent interning has finished.
    const std::string& get(uint32_t id) const { return id < strings_.size() ? strings_[id] : strings_[0]; }
    size_t size() const { return strings_.size(); }

private:
    std::mutex mutex_;
    std::vector<std::string> strings_;
    std::unordered_map<std::string, uint32_t> index_;
};

} // namespace fo4mx
