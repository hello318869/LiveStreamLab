#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lsl::server {

// Fixed windows, bounded memory; do not evict active entries to admit a flood.
class RateLimiter {
public:
    bool allow(const std::string& key, unsigned limit, unsigned seconds) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() >= 4096) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->second.until <= now) it = entries_.erase(it);
                else ++it;
            }
        }
        auto found = entries_.find(key);
        if (found == entries_.end()) {
            if (entries_.size() >= 4096) return false;
            found = entries_.emplace(key, Entry{now + std::chrono::seconds(seconds), 0}).first;
        }
        if (found->second.until <= now) found->second = {now + std::chrono::seconds(seconds), 0};
        if (found->second.count >= limit) return false;
        ++found->second.count;
        return true;
    }
private:
    struct Entry { std::chrono::steady_clock::time_point until; unsigned count; };
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace lsl::server
