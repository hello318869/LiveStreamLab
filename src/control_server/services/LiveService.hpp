#pragma once

#include "control_server/common/Support.hpp"
#include "control_server/config/ServerConfig.hpp"
#include "control_server/srs/SrsClient.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace lsl::server {

class LiveService : public std::enable_shared_from_this<LiveService> {
public:
    LiveService(drogon::orm::DbClientPtr db, const ServerConfig& config,
                std::shared_ptr<SrsClient> srs);
    ~LiveService();
    void start();
    void stop();
    Json::Value mediaMeta() const;
    void create(std::uint64_t userId, std::string requestId, Reply reply, Fail fail);
    void getSession(std::uint64_t userId, std::string id, Reply reply, Fail fail);
    void stopSession(std::uint64_t userId, std::string id, Reply reply, Fail fail);
    void list(unsigned page, unsigned pageSize, Reply reply, Fail fail);
    void room(std::uint64_t roomId, Reply reply, Fail fail);
    void playback(std::uint64_t roomId, Reply reply, Fail fail);
    void hook(Json::Value body, Reply reply, Fail fail);
    bool trustedHook(const std::string& peer, const std::string& key) const;

private:
    struct Record;
    struct Observation {
        SrsIdentity identity;
        std::uint64_t generation = 0;
        std::uint64_t audio = 0;
        std::uint64_t video = 0;
        std::chrono::steady_clock::time_point progress;
        bool mediaConfirmed = false;
    };
    bool mediaAvailable() const;
    void reconcile();
    void unavailable();
    void confirm(const Record& record, const SrsSnapshot& snapshot,
                 std::function<void(bool)> done, std::function<void()> pendingMedia);
    void finish(const Record& record, const std::string& status,
                const std::string& reason, std::function<void(bool)> done);
    void recover(const Record& record, bool close, std::function<void(bool)> done);

    drogon::orm::DbClientPtr db_;
    ServerConfig config_;
    std::shared_ptr<SrsClient> srs_;
    std::string hook_secret_;
    std::atomic<bool> running_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> available_{false};
    std::uint64_t timer_ = 0;
    mutable std::mutex mutex_;
    std::string last_confirmed_;
    std::chrono::steady_clock::time_point last_success_{};
    std::unordered_map<std::string, Observation> observations_;
};

} // namespace lsl::server
