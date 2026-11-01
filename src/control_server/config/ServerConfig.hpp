#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lsl::server {

struct ServerConfig {
    struct Http {
        std::string address;
        std::uint16_t port;
        unsigned threads;
        unsigned max_body_bytes;
    } http;

    struct Database {
        bool enabled;
        std::string host;
        std::uint16_t port;
        std::string name;
        std::string user;
        std::string password_env;
        unsigned connections;
        unsigned timeout_seconds;
    } database;

    struct Addresses {
        std::string api_public_base;
        std::string rtmp_public_base;
        std::string playback_public_base;
        std::string srs_api_internal_base;
        std::string srs_app;
        std::string srs_vhost;
    } addresses;

    struct Srs {
        std::string expected_version;
        std::string hook_secret_env;
        std::vector<std::string> trusted_sources;
        unsigned request_timeout_seconds;
        unsigned reconcile_seconds;
    } srs;

    struct Lifecycle {
        unsigned prepare_seconds;
        unsigned reconnect_seconds;
        unsigned no_media_seconds;
        unsigned auth_ttl_seconds;
    } lifecycle;

    // Strict JSON; diagnostics never include values or parser excerpts.
    static ServerConfig load(const std::string& path);
};

} // namespace lsl::server
