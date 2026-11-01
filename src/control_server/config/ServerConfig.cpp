#include "ServerConfig.hpp"

#include <arpa/inet.h>
#include <json/json.h>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace lsl::server {
namespace {

[[noreturn]] void invalid(const std::string& field) {
    throw std::runtime_error("Invalid or missing configuration field: " + field);
}

void keys(const Json::Value& object, std::initializer_list<std::string_view> allowed,
          const std::string& section) {
    if (!object.isObject()) invalid(section);
    for (const auto& key : object.getMemberNames()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            // Do not echo an untrusted key: it may contain a pasted secret.
            throw std::runtime_error("Unknown configuration field in " + section);
        }
    }
}

std::string string(const Json::Value& object, const char* key, const std::string& section) {
    const auto& value = object[key];
    if (!value.isString()) invalid(section + "." + key);
    auto result = value.asString();
    if (result.empty() || result.size() > 2048 ||
        std::any_of(result.begin(), result.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
        invalid(section + "." + key);
    }
    return result;
}

unsigned number(const Json::Value& object, const char* key, unsigned min, unsigned max,
                const std::string& section) {
    const auto& value = object[key];
    if (!value.isUInt() || value.asUInt() < min || value.asUInt() > max) invalid(section + "." + key);
    return value.asUInt();
}

bool ip(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos) return false;
    in_addr v4{};
    in6_addr v6{};
    return inet_pton(AF_INET, value.c_str(), &v4) == 1 || inet_pton(AF_INET6, value.c_str(), &v6) == 1;
}

std::string envName(const Json::Value& object, const char* key, const std::string& section) {
    auto value = string(object, key, section);
    if (!std::regex_match(value, std::regex("[A-Z_][A-Z0-9_]{0,127}"))) invalid(section + "." + key);
    return value;
}

std::string url(const Json::Value& object, const char* key, const std::string& scheme) {
    auto value = string(object, key, "addresses");
    const auto separator = value.find("://");
    if (separator == std::string::npos) invalid(std::string("addresses.") + key);
    const auto protocol = value.substr(0, separator);
    if (scheme == "http" ? protocol != "http" && protocol != "https" : protocol != scheme) {
        invalid(std::string("addresses.") + key);
    }
    const auto authorityEnd = value.find('/', separator + 3);
    const auto authority = value.substr(separator + 3, authorityEnd - (separator + 3));
    // Base URLs contain no credentials/query/fragment or path; app is separate.
    if (authority.empty() || value.find_first_of("@?#\\%") != std::string::npos ||
        authorityEnd != std::string::npos) invalid(std::string("addresses.") + key);
    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) invalid(std::string("addresses.") + key);
        host = authority.substr(1, close - 1);
        in6_addr addr{};
        if (inet_pton(AF_INET6, host.c_str(), &addr) != 1) invalid(std::string("addresses.") + key);
        if (close + 1 != authority.size()) {
            if (authority[close + 1] != ':') invalid(std::string("addresses.") + key);
            port = authority.substr(close + 2);
            if (port.empty()) invalid(std::string("addresses.") + key);
        }
    } else {
        const auto colon = authority.find(':');
        host = authority.substr(0, colon);
        if (!std::regex_match(host, std::regex("[A-Za-z0-9][A-Za-z0-9.-]*"))) invalid(std::string("addresses.") + key);
        if (colon != std::string::npos) {
            port = authority.substr(colon + 1);
            if (port.empty()) invalid(std::string("addresses.") + key);
        }
    }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            invalid(std::string("addresses.") + key);
        }
        const auto parsed = std::stoul(port);
        if (parsed == 0 || parsed > 65535) invalid(std::string("addresses.") + key);
    }
    return value;
}

} // namespace

ServerConfig ServerConfig::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open server configuration file");
    // Bound the read itself, not only the size reported by the filesystem.
    std::string contents(65537, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || contents.empty() || contents.size() > 65536) {
        throw std::runtime_error("Server configuration must be a readable JSON file of at most 64 KiB");
    }
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["allowComments"] = false;
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 32;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(contents.data(), contents.data() + contents.size(), &root, &errors)) {
        throw std::runtime_error("Server configuration is not strict JSON");
    }
    keys(root, {"http", "database", "addresses", "srs", "lifecycle"}, "root");
    const auto& h = root["http"];
    keys(h, {"address", "port", "threads", "max_body_bytes"}, "http");
    const auto& d = root["database"];
    keys(d, {"enabled", "host", "port", "name", "user", "password_env", "connections", "timeout_seconds"}, "database");
    const auto& a = root["addresses"];
    keys(a, {"api_public_base", "rtmp_public_base", "playback_public_base", "srs_api_internal_base", "srs_app", "srs_vhost"}, "addresses");
    const auto& s = root["srs"];
    keys(s, {"expected_version", "hook_secret_env", "trusted_sources", "request_timeout_seconds", "reconcile_seconds"}, "srs");
    const auto& l = root["lifecycle"];
    keys(l, {"prepare_seconds", "reconnect_seconds", "no_media_seconds", "auth_ttl_seconds"}, "lifecycle");
    ServerConfig result{};
    result.http = {string(h, "address", "http"), static_cast<std::uint16_t>(number(h, "port", 1, 65535, "http")),
                   number(h, "threads", 1, 64, "http"), number(h, "max_body_bytes", 1024, 1048576, "http")};
    if (!ip(result.http.address)) invalid("http.address");
    if (!d["enabled"].isBool()) invalid("database.enabled");
    result.database = {d["enabled"].asBool(), string(d, "host", "database"),
        static_cast<std::uint16_t>(number(d, "port", 1, 65535, "database")), string(d, "name", "database"),
        string(d, "user", "database"), envName(d, "password_env", "database"),
        number(d, "connections", 1, 64, "database"), number(d, "timeout_seconds", 1, 60, "database")};
    if (!std::regex_match(result.database.name, std::regex("[A-Za-z0-9_]{1,64}"))) invalid("database.name");
    result.addresses = {url(a, "api_public_base", "http"), url(a, "rtmp_public_base", "rtmp"),
        url(a, "playback_public_base", "http"), url(a, "srs_api_internal_base", "http"), string(a, "srs_app", "addresses"),
        a.isMember("srs_vhost") ? string(a, "srs_vhost", "addresses") : "__defaultVhost__"};
    if (!std::regex_match(result.addresses.srs_app, std::regex("[A-Za-z0-9_-]{1,64}"))) invalid("addresses.srs_app");
    if (!std::regex_match(result.addresses.srs_vhost, std::regex("[A-Za-z0-9_.-]{1,128}"))) invalid("addresses.srs_vhost");
    if (result.addresses.srs_vhost != "__defaultVhost__") {
        // Named vhosts are selected by the public URL's hostname. Never accept
        // an IP-based URL that would route to the default vhost instead.
        const auto matchesVhost = [&result](const std::string& base) {
            const auto authority = base.substr(base.find("://") + 3);
            return authority.substr(0, authority.find(':')) == result.addresses.srs_vhost;
        };
        if (!matchesVhost(result.addresses.rtmp_public_base) ||
            !matchesVhost(result.addresses.playback_public_base)) invalid("addresses.srs_vhost");
    }
    result.srs.expected_version = string(s, "expected_version", "srs");
    if (!std::regex_match(result.srs.expected_version, std::regex("[0-9]+\\.[0-9]+\\.[0-9]+"))) invalid("srs.expected_version");
    result.srs.hook_secret_env = envName(s, "hook_secret_env", "srs");
    if (!s["trusted_sources"].isArray() || s["trusted_sources"].empty() || s["trusted_sources"].size() > 32) invalid("srs.trusted_sources");
    for (const auto& value : s["trusted_sources"]) {
        if (!value.isString() || !ip(value.asString())) invalid("srs.trusted_sources");
        result.srs.trusted_sources.push_back(value.asString());
    }
    result.srs.request_timeout_seconds = number(s, "request_timeout_seconds", 1, 30, "srs");
    result.srs.reconcile_seconds = number(s, "reconcile_seconds", 1, 60, "srs");
    result.lifecycle = {number(l, "prepare_seconds", 1, 600, "lifecycle"), number(l, "reconnect_seconds", 1, 300, "lifecycle"),
        number(l, "no_media_seconds", 1, 300, "lifecycle"), number(l, "auth_ttl_seconds", 60, 604800, "lifecycle")};
    if (result.lifecycle.no_media_seconds <= result.srs.reconcile_seconds ||
        result.lifecycle.reconnect_seconds <= result.srs.reconcile_seconds) invalid("lifecycle");
    return result;
}

} // namespace lsl::server
