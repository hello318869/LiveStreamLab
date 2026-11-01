#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lsl::server {

struct ServerConfig;

struct SrsIdentity {
    std::string server;
    std::string service;
    std::string stream;
    std::string client;
};

struct SrsStream {
    SrsIdentity identity;
    std::string name;
    std::string app;
    // Actual vhost name, resolved from the opaque ID in the SRS stream JSON.
    std::string vhost;
    bool active = false;
    std::uint64_t audio_frames = 0;
    std::uint64_t video_frames = 0;
    std::uint64_t recv_bytes = 0;
    bool has_audio = false;
    bool has_video = false;
};

struct SrsSnapshot {
    std::string server;
    std::string service;
    std::vector<SrsStream> streams;
};

// Exact SRS 8.0.58 HTTP adapter. Methods are asynchronous, have bounded
// pagination/timeouts, and never write request/response bodies to logs.
class SrsClient {
public:
    explicit SrsClient(const ServerConfig& config);

    void snapshot(std::function<void(SrsSnapshot)> done,
                  std::function<void()> failure);

    // The inventory is not an atomic SRS snapshot. Confirm a missing known
    // publisher with this point lookup before treating it as disconnected.
    // nullopt means this exact identity is no longer publishing; transport,
    // version or malformed-response failures call failure instead.
    void inspectPublisher(const SrsIdentity& identity,
                          std::function<void(std::optional<SrsStream>)> done,
                          std::function<void()> failure);

    // Checks the exact publishing client even before media is active. The RTMP
    // client is registered before SRS issues on_publish; this distinguishes a
    // pending admitted connection from an absent publisher. stream may be empty
    // until its ID is resolved from the authenticated client's SRS record.
    void connectionExists(const SrsIdentity& identity,
                          std::function<void(bool)> done,
                          std::function<void()> failure);

    // Rechecks current SRS instance and client/stream ownership before DELETE.
    // An unknown stream ID is resolved from the exact pending publishing client.
    // true acknowledges a successful stop request (or confirmed absent client),
    // not final stream disappearance. SRS has no conditional DELETE operation.
    void closePublisher(const SrsIdentity& identity,
                        std::function<void(bool)> done);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lsl::server
