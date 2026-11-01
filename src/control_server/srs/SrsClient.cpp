#include "SrsClient.hpp"
#include "control_server/config/ServerConfig.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace lsl::server {
namespace {

using Clock = std::chrono::steady_clock;
using JsonReply = std::function<void(bool, int, Json::Value)>;
constexpr unsigned pageSize = 100;
constexpr unsigned maxPages = 32;
constexpr unsigned maxVhosts = 256;
constexpr std::size_t maxResponseBytes = 1024 * 1024;
constexpr int streamNotFound = 2048;
constexpr int clientNotFound = 2049;

bool text(const Json::Value& value, std::size_t max = 128) {
    if (!value.isString()) return false;
    const auto& content = value.asString();
    return !content.empty() && content.size() <= max &&
        std::all_of(content.begin(), content.end(), [](unsigned char c) { return c > 32 && c != 127; });
}

bool validId(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

bool envelope(const Json::Value& root) {
    return root.isObject() && root["code"].isInt64() && root["code"].asInt64() == 0 &&
           text(root["server"]) && text(root["service"]);
}

bool matches(const Json::Value& root, const std::string& server, const std::string& service) {
    return envelope(root) && root["server"].asString() == server && root["service"].asString() == service;
}

bool nativeCode(const Json::Value& root, int code) {
    return root.isObject() && root["code"].isInt64() && root["code"].asInt64() == code;
}

bool version(const Json::Value& root, const std::string& expected) {
    const auto& data = root["data"];
    return envelope(root) && data.isObject() && data["version"].isString() &&
           data["version"].asString() == expected && data["major"].isUInt() &&
           data["minor"].isUInt() && data["revision"].isUInt() &&
           std::to_string(data["major"].asUInt()) + "." + std::to_string(data["minor"].asUInt()) + "." +
               std::to_string(data["revision"].asUInt()) == expected;
}

std::string encodePath(const std::string& value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(digits[c >> 4]);
            result.push_back(digits[c & 15]);
        }
    }
    return result;
}

bool parseStream(const Json::Value& raw, const std::string& server, const std::string& service,
                 const std::string& vhost, SrsStream& stream) {
    if (!raw.isObject() || !text(raw["id"]) || !text(raw["name"], 512) ||
        !text(raw["app"], 512) || !text(raw["vhost"]) || !raw["publish"].isObject() ||
        !raw["publish"]["active"].isBool() || !raw["audio_frames"].isUInt64() ||
        !raw["video_frames"].isUInt64() || !raw["recv_bytes"].isUInt64()) return false;
    stream.identity = {server, service, raw["id"].asString(), ""};
    stream.active = raw["publish"]["active"].asBool();
    const auto& cid = raw["publish"]["cid"];
    if (!cid.isNull()) {
        if (!text(cid)) return false;
        stream.identity.client = cid.asString();
    }
    if (stream.active && stream.identity.client.empty()) return false;
    stream.name = raw["name"].asString();
    stream.app = raw["app"].asString();
    stream.vhost = vhost;
    stream.audio_frames = raw["audio_frames"].asUInt64();
    stream.video_frames = raw["video_frames"].asUInt64();
    stream.recv_bytes = raw["recv_bytes"].asUInt64();
    const auto& audio = raw["audio"];
    const auto& video = raw["video"];
    if ((!audio.isNull() && !audio.isObject()) || (!video.isNull() && !video.isObject())) return false;
    // The platform's first media contract is AAC/H.264. Other codecs must not
    // accidentally certify a stream as ready for this client implementation.
    stream.has_audio = audio.isObject() && audio["codec"].isString() && audio["codec"].asString() == "AAC";
    stream.has_video = video.isObject() && video["codec"].isString() && video["codec"].asString() == "H264";
    return true;
}

} // namespace

struct SrsClient::Impl : std::enable_shared_from_this<SrsClient::Impl> {
    struct SnapshotOperation;
    struct InspectOperation;
    struct ConnectionOperation;
    struct CloseOperation;

    explicit Impl(const ServerConfig& config)
        : client(drogon::HttpClient::newHttpClient(config.addresses.srs_api_internal_base,
                                                   drogon::app().getLoop(), false, true)),
          expectedVersion(config.srs.expected_version), timeout(config.srs.request_timeout_seconds) {
        client->setUserAgent("LiveStreamLab-control");
        client->enableCookies(false);
        client->setPipeliningDepth(0);
    }

    double remaining(Clock::time_point deadline) const {
        return std::min(timeout, std::chrono::duration<double>(deadline - Clock::now()).count());
    }

    Clock::time_point deadline() const {
        return Clock::now() + std::chrono::seconds(static_cast<int>(std::max(5.0, timeout * 4.0)));
    }

    void request(drogon::HttpMethod method, const std::string& path,
                 const std::map<std::string, std::string>& parameters, Clock::time_point until,
                 JsonReply done) {
        const double requestTimeout = remaining(until);
        if (requestTimeout <= 0 || inflight.fetch_add(1) >= 32) {
            if (requestTimeout > 0) inflight.fetch_sub(1);
            done(false, 0, {});
            return;
        }
        auto self = shared_from_this();
        try {
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setMethod(method);
            req->setPath(path);
            req->addHeader("Accept", "application/json");
            for (const auto& entry : parameters) req->setParameter(entry.first, entry.second);
            client->sendRequest(req, [self, done](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
                self->inflight.fetch_sub(1);
                bool ok = false;
                int status = 0;
                Json::Value root;
                try {
                    if (result == drogon::ReqResult::Ok && response &&
                        response->body().size() <= maxResponseBytes) {
                        status = static_cast<int>(response->statusCode());
                        const auto body = response->body();
                        Json::CharReaderBuilder builder;
                        Json::CharReaderBuilder::strictMode(&builder.settings_);
                        builder["collectComments"] = false;
                        builder["allowComments"] = false;
                        builder["rejectDupKeys"] = true;
                        builder["stackLimit"] = 32;
                        std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                        std::string error;
                        ok = reader->parse(body.data(), body.data() + body.size(), &root, &error) && root.isObject();
                    }
                } catch (...) {
                    ok = false;
                }
                done(ok, status, std::move(root));
            }, requestTimeout);
        } catch (...) {
            inflight.fetch_sub(1);
            done(false, 0, {});
        }
    }

    drogon::HttpClientPtr client;
    std::string expectedVersion;
    double timeout;
    std::atomic<unsigned> inflight{0};
};

struct SrsClient::Impl::SnapshotOperation : std::enable_shared_from_this<SnapshotOperation> {
    SnapshotOperation(std::shared_ptr<Impl> impl, std::function<void(SrsSnapshot)> done,
                      std::function<void()> failure)
        : impl(std::move(impl)), done(std::move(done)), failure(std::move(failure)), until(this->impl->deadline()) {}

    void fail() {
        if (finished) return;
        finished = true;
        failure();
    }

    void start() {
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/versions", {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !version(root, self->impl->expectedVersion)) return self->fail();
                self->result.server = root["server"].asString();
                self->result.service = root["service"].asString();
                self->vhosts();
            });
    }

    void vhosts() {
        auto self = shared_from_this();
        // SRS redirects collection paths without the trailing slash. Drogon's
        // HTTP client does not follow that redirect, so use the canonical path.
        impl->request(drogon::Get, "/api/v1/vhosts/", {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !matches(root, self->result.server, self->result.service) ||
                    !root["vhosts"].isArray() || root["vhosts"].size() > maxVhosts) return self->fail();
                for (const auto& vhost : root["vhosts"]) {
                    if (!vhost.isObject() || !text(vhost["id"]) || !text(vhost["name"], 512) ||
                        !self->vhostNames.emplace(vhost["id"].asString(), vhost["name"].asString()).second) return self->fail();
                }
                self->streams(0);
            });
    }

    void streams(unsigned start) {
        if (start / pageSize >= maxPages) return fail();
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/streams/", {{"start", std::to_string(start)}, {"count", std::to_string(pageSize)}}, until,
            [self, start](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !matches(root, self->result.server, self->result.service) ||
                    !root["total"].isUInt64() || root["total"].asUInt64() > pageSize * maxPages ||
                    !root["streams"].isArray() || root["streams"].size() > pageSize) return self->fail();
                const auto total = root["total"].asUInt64();
                if (start == 0) self->total = total;
                // SRS pagination has no snapshot token. Refuse visibly shifting
                // inventories; point lookups additionally confirm omissions.
                if (total != self->total) return self->fail();
                const auto& streams = root["streams"];
                for (const auto& raw : streams) {
                    if (!raw.isObject() || !text(raw["vhost"])) return self->fail();
                    auto vhost = self->vhostNames.find(raw["vhost"].asString());
                    if (vhost == self->vhostNames.end()) return self->fail();
                    SrsStream parsed;
                    if (!parseStream(raw, self->result.server, self->result.service, vhost->second, parsed) ||
                        !self->seen.emplace(parsed.identity.stream).second) return self->fail();
                    self->result.streams.push_back(std::move(parsed));
                }
                const auto received = static_cast<std::uint64_t>(self->result.streams.size());
                if (received > total || (streams.size() < pageSize && received != total)) return self->fail();
                if (received == total) {
                    self->finished = true;
                    self->done(std::move(self->result));
                    return;
                }
                self->streams(start + pageSize);
            });
    }

    std::shared_ptr<Impl> impl;
    std::function<void(SrsSnapshot)> done;
    std::function<void()> failure;
    Clock::time_point until;
    bool finished = false;
    SrsSnapshot result;
    std::map<std::string, std::string> vhostNames;
    std::set<std::string> seen;
    std::uint64_t total = 0;
};

struct SrsClient::Impl::InspectOperation : std::enable_shared_from_this<InspectOperation> {
    InspectOperation(std::shared_ptr<Impl> impl, SrsIdentity identity,
                     std::function<void(std::optional<SrsStream>)> done, std::function<void()> failure)
        : impl(std::move(impl)), identity(std::move(identity)), done(std::move(done)),
          failure(std::move(failure)), until(this->impl->deadline()) {}

    void fail() {
        if (finished) return;
        finished = true;
        failure();
    }

    void absent() {
        if (finished) return;
        finished = true;
        done(std::nullopt);
    }

    void start() {
        if (!validId(identity.server) || !validId(identity.service) || !validId(identity.stream) || !validId(identity.client)) return fail();
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/versions", {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !version(root, self->impl->expectedVersion)) return self->fail();
                if (!matches(root, self->identity.server, self->identity.service)) return self->absent();
                self->stream();
            });
    }

    void stream() {
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/streams/" + encodePath(identity.stream), {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok) return self->fail();
                if (status == 200 && nativeCode(root, streamNotFound)) return self->absent();
                if (status != 200 || !envelope(root)) return self->fail();
                if (!matches(root, self->identity.server, self->identity.service)) return self->absent();
                const auto& raw = root["stream"];
                if (!raw.isObject() || !text(raw["id"]) || raw["id"].asString() != self->identity.stream ||
                    !raw["publish"].isObject() || !raw["publish"]["active"].isBool()) return self->fail();
                if (!raw["publish"]["active"].asBool()) return self->absent();
                if (!text(raw["publish"]["cid"])) return self->fail();
                if (raw["publish"]["cid"].asString() != self->identity.client) return self->absent();
                if (!text(raw["vhost"])) return self->fail();
                self->rawStream = raw;
                self->vhost(raw["vhost"].asString());
            });
    }

    void vhost(const std::string& id) {
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/vhosts/" + encodePath(id), {}, until,
            [self, id](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !matches(root, self->identity.server, self->identity.service) ||
                    !root["vhost"].isObject() || !text(root["vhost"]["id"]) ||
                    root["vhost"]["id"].asString() != id || !text(root["vhost"]["name"], 512)) return self->fail();
                SrsStream stream;
                if (!parseStream(self->rawStream, self->identity.server, self->identity.service,
                                 root["vhost"]["name"].asString(), stream)) return self->fail();
                self->finished = true;
                self->done(std::move(stream));
            });
    }

    std::shared_ptr<Impl> impl;
    SrsIdentity identity;
    std::function<void(std::optional<SrsStream>)> done;
    std::function<void()> failure;
    Clock::time_point until;
    Json::Value rawStream;
    bool finished = false;
};

struct SrsClient::Impl::CloseOperation : std::enable_shared_from_this<CloseOperation> {
    CloseOperation(std::shared_ptr<Impl> impl, SrsIdentity identity, std::function<void(bool)> done)
        : impl(std::move(impl)), identity(std::move(identity)), done(std::move(done)), until(this->impl->deadline()) {}

    void finish(bool result) {
        if (finished) return;
        finished = true;
        done(result);
    }

    void start() {
        if (!validId(identity.server) || !validId(identity.service) ||
            (!identity.stream.empty() && !validId(identity.stream)) || !validId(identity.client)) return finish(false);
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/versions", {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !version(root, self->impl->expectedVersion) ||
                    !matches(root, self->identity.server, self->identity.service)) return self->finish(false);
                self->inspect();
            });
    }

    void inspect() {
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/clients/" + encodePath(identity.client), {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok) return self->finish(false);
                if (status == 200 && nativeCode(root, clientNotFound)) return self->finish(true);
                if (status != 200 || !matches(root, self->identity.server, self->identity.service)) return self->finish(false);
                const auto& client = root["client"];
                if (!client.isObject() || !text(client["id"]) || !text(client["stream"]) ||
                    !client["publish"].isBool() || !client["publish"].asBool() ||
                    client["id"].asString() != self->identity.client ||
                    (!self->identity.stream.empty() && client["stream"].asString() != self->identity.stream)) return self->finish(false);
                self->identity.stream = client["stream"].asString();
                self->remove();
            });
    }

    void remove() {
        auto self = shared_from_this();
        impl->request(drogon::Delete, "/api/v1/clients/" + encodePath(identity.client), {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200) return self->finish(false);
                if (nativeCode(root, clientNotFound)) return self->finish(true);
                self->finish(matches(root, self->identity.server, self->identity.service));
            });
    }

    std::shared_ptr<Impl> impl;
    SrsIdentity identity;
    std::function<void(bool)> done;
    Clock::time_point until;
    bool finished = false;
};

struct SrsClient::Impl::ConnectionOperation : std::enable_shared_from_this<ConnectionOperation> {
    ConnectionOperation(std::shared_ptr<Impl> impl, SrsIdentity identity,
                        std::function<void(bool)> done, std::function<void()> failure)
        : impl(std::move(impl)), identity(std::move(identity)), done(std::move(done)),
          failure(std::move(failure)), until(this->impl->deadline()) {}

    void fail() {
        if (finished) return;
        finished = true;
        failure();
    }

    void finish(bool present) {
        if (finished) return;
        finished = true;
        done(present);
    }

    void start() {
        if (!validId(identity.server) || !validId(identity.service) ||
            (!identity.stream.empty() && !validId(identity.stream)) || !validId(identity.client)) return fail();
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/versions", {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200 || !version(root, self->impl->expectedVersion)) return self->fail();
                if (!matches(root, self->identity.server, self->identity.service)) return self->finish(false);
                self->inspect();
            });
    }

    void inspect() {
        auto self = shared_from_this();
        impl->request(drogon::Get, "/api/v1/clients/" + encodePath(identity.client), {}, until,
            [self](bool ok, int status, Json::Value root) {
                if (!ok || status != 200) return self->fail();
                if (nativeCode(root, clientNotFound)) return self->finish(false);
                if (!envelope(root)) return self->fail();
                if (!matches(root, self->identity.server, self->identity.service)) return self->finish(false);
                const auto& client = root["client"];
                if (!client.isObject() || !text(client["id"]) || !text(client["stream"]) ||
                    !client["publish"].isBool() || client["id"].asString() != self->identity.client) return self->fail();
                self->finish(client["publish"].asBool() && (self->identity.stream.empty() ||
                    client["stream"].asString() == self->identity.stream));
            });
    }

    std::shared_ptr<Impl> impl;
    SrsIdentity identity;
    std::function<void(bool)> done;
    std::function<void()> failure;
    Clock::time_point until;
    bool finished = false;
};

SrsClient::SrsClient(const ServerConfig& config) : impl_(std::make_shared<Impl>(config)) {}

void SrsClient::snapshot(std::function<void(SrsSnapshot)> done, std::function<void()> failure) {
    std::make_shared<Impl::SnapshotOperation>(impl_, std::move(done), std::move(failure))->start();
}

void SrsClient::inspectPublisher(const SrsIdentity& identity,
                                 std::function<void(std::optional<SrsStream>)> done,
                                 std::function<void()> failure) {
    std::make_shared<Impl::InspectOperation>(impl_, identity, std::move(done), std::move(failure))->start();
}

void SrsClient::closePublisher(const SrsIdentity& identity, std::function<void(bool)> done) {
    std::make_shared<Impl::CloseOperation>(impl_, identity, std::move(done))->start();
}

void SrsClient::connectionExists(const SrsIdentity& identity, std::function<void(bool)> done,
                                  std::function<void()> failure) {
    std::make_shared<Impl::ConnectionOperation>(impl_, identity, std::move(done), std::move(failure))->start();
}

} // namespace lsl::server
