#include "LiveService.hpp"
#include "control_server/repositories/LiveRepository.hpp"

#include <drogon/HttpAppFramework.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace lsl::server {
namespace {

bool sameIdentity(const SrsIdentity& a, const SrsIdentity& b) {
    return a.server == b.server && a.service == b.service &&
           a.stream == b.stream && a.client == b.client;
}

bool sameConnection(const SrsIdentity& a, const SrsIdentity& b) {
    return a.server == b.server && a.service == b.service && a.client == b.client;
}

ApiError mediaError() { return {503, "MEDIA_SERVICE_UNAVAILABLE", "Media service unavailable", {}}; }
ApiError missingSession() { return {404, "LIVE_SESSION_NOT_FOUND", "Live session not found", {}}; }
ApiError forbidden() { return {403, "FORBIDDEN", "Access denied", {}}; }

Fail transactionFailure(const std::shared_ptr<drogon::orm::Transaction>& tx, Fail fail) {
    std::weak_ptr<drogon::orm::Transaction> weak(tx);
    return [weak, fail = std::move(fail)](ApiError error) {
        if (auto transaction = weak.lock()) transaction->rollback();
        fail(std::move(error));
    };
}

Json::Value sessionJson(const drogon::orm::Row& row) {
    Json::Value value(Json::objectValue);
    value["room_id"] = row["room_id"].as<std::string>();
    value["live_session_id"] = row["id"].as<std::string>();
    value["client_request_id"] = row["client_request_id"].as<std::string>();
    value["status"] = row["status"].as<std::string>();
    value["allow_publish"] = row["allow_publish"].as<unsigned>() != 0;
    for (const auto* key : {"created_at", "prepare_deadline", "started_at", "ended_at", "reconnect_deadline"})
        value[key] = jsonTime(row[key]);
    value["end_reason"] = row["end_reason"].isNull() ? Json::Value() : Json::Value(row["end_reason"].as<std::string>());
    return value;
}

Json::Value conflictJson(const drogon::orm::Row& row) {
    Json::Value value(Json::objectValue);
    value["room_id"] = row["room_id"].as<std::string>();
    value["live_session_id"] = row["id"].as<std::string>();
    value["client_request_id"] = row["client_request_id"].as<std::string>();
    value["status"] = row["status"].as<std::string>();
    return value;
}

std::string utcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool tokenShape(const std::string& token) {
    return token.size() == 43 && std::all_of(token.begin(), token.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

std::string publishToken(const std::string& parameters) {
    if (parameters.size() > 4096) throw invalidArgument();
    std::string result;
    std::size_t cursor = parameters.empty() || parameters.front() != '?' ? 0 : 1;
    while (cursor < parameters.size()) {
        const auto end = parameters.find('&', cursor);
        const auto pair = parameters.substr(cursor, end == std::string::npos ? end : end - cursor);
        const auto equal = pair.find('=');
        if (pair.substr(0, equal) == "token") {
            if (!result.empty() || equal == std::string::npos) throw invalidArgument();
            result = pair.substr(equal + 1);
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    if (!tokenShape(result)) throw invalidArgument();
    return result;
}

std::string hookString(const Json::Value& body, const char* key, bool required = true) {
    if (!body.isMember(key) && !required) return {};
    if (!body[key].isString()) throw invalidArgument();
    auto value = body[key].asString();
    if ((required && value.empty()) || value.size() > 128 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c == 127; })) throw invalidArgument();
    return value;
}

} // namespace

struct LiveService::Record {
    std::string id;
    std::uint64_t room = 0;
    std::uint64_t owner = 0;
    std::string current;
    std::string name;
    std::string status;
    std::string reason;
    SrsIdentity identity;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    bool connected = false;
    bool prepareExpired = false;
    bool reconnectExpired = false;
    bool started = false;

    explicit Record(const drogon::orm::Row& row)
        : id(row["id"].as<std::string>()), room(row["room_id"].as<std::uint64_t>()),
          owner(row["owner_id"].as<std::uint64_t>()),
          current(row["current_session_id"].isNull() ? "" : row["current_session_id"].as<std::string>()),
          name(row["stream_name"].as<std::string>()), status(row["status"].as<std::string>()),
          reason(row["end_reason"].isNull() ? "" : row["end_reason"].as<std::string>()),
          identity{row["srs_server_id"].isNull() ? "" : row["srs_server_id"].as<std::string>(),
                   row["srs_service_id"].isNull() ? "" : row["srs_service_id"].as<std::string>(),
                   row["srs_stream_id"].isNull() ? "" : row["srs_stream_id"].as<std::string>(),
                   row["srs_client_id"].isNull() ? "" : row["srs_client_id"].as<std::string>()},
          revision(row["revision"].as<std::uint64_t>()), generation(row["publish_generation"].as<std::uint64_t>()),
          connected(row["publish_connected"].as<unsigned>() != 0),
          prepareExpired(!row["prepare_expired"].isNull() && row["prepare_expired"].as<unsigned>() != 0),
          reconnectExpired(!row["reconnect_expired"].isNull() && row["reconnect_expired"].as<unsigned>() != 0),
          started(!row["started_at"].isNull()) {}
};

LiveService::LiveService(drogon::orm::DbClientPtr db, const ServerConfig& config,
                         std::shared_ptr<SrsClient> srs)
    : db_(std::move(db)), config_(config), srs_(std::move(srs)) {
    const auto* secret = std::getenv(config_.srs.hook_secret_env.c_str());
    if (secret != nullptr && tokenShape(secret)) hook_secret_ = secret;
}

LiveService::~LiveService() { stop(); }

void LiveService::start() {
    if (running_.exchange(true)) return;
    std::weak_ptr<LiveService> weak = shared_from_this();
    timer_ = drogon::app().getLoop()->runEvery(config_.srs.reconcile_seconds, [weak] {
        if (auto self = weak.lock()) self->reconcile();
    });
    reconcile();
}

void LiveService::stop() {
    if (!running_.exchange(false)) return;
    if (timer_ != 0) drogon::app().getLoop()->invalidateTimer(timer_);
    timer_ = 0;
    available_ = false;
}

bool LiveService::mediaAvailable() const {
    if (!running_ || !available_ || hook_secret_.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return std::chrono::steady_clock::now() - last_success_ <=
        std::chrono::seconds(2 * config_.srs.reconcile_seconds + config_.srs.request_timeout_seconds);
}

Json::Value LiveService::mediaMeta() const {
    Json::Value meta(Json::objectValue);
    meta["media_service_status"] = mediaAvailable() ? "AVAILABLE" : "UNAVAILABLE";
    std::lock_guard<std::mutex> lock(mutex_);
    meta["last_confirmed_at"] = last_confirmed_.empty() ? Json::Value() : Json::Value(last_confirmed_);
    return meta;
}

bool LiveService::trustedHook(const std::string& peer, const std::string& key) const {
    if (hook_secret_.empty() || !constantTimeEqual(key, hook_secret_)) return false;
    const auto canonical = canonicalIp(peer);
    return std::any_of(config_.srs.trusted_sources.begin(), config_.srs.trusted_sources.end(),
        [&canonical](const std::string& allowed) { return canonical == canonicalIp(allowed); });
}

void LiveService::create(std::uint64_t userId, std::string requestId, Reply reply, Fail fail) {
    if (!db_) return fail(databaseError());
    if (!mediaAvailable()) return fail(mediaError());
    auto self = shared_from_this();
    beginTransaction(db_, [self, userId, requestId = std::move(requestId), reply, fail](auto tx) {
        const auto error = transactionFailure(tx, fail);
        LiveRepository::exec(tx, LiveRepository::Operation::LockOwnedRoom, [self, tx, requestId, reply, error](const auto& rooms) {
                if (rooms.empty()) return error({404, "ROOM_NOT_FOUND", "Room not found", {}});
                const auto roomId = rooms[0]["id"].template as<std::uint64_t>();
                LiveRepository::exec(tx, LiveRepository::Operation::ExistingSession, [self, tx, roomId, requestId, reply, error](const auto& existing) {
                        if (!existing.empty()) {
                            const auto status = existing[0]["status"].template as<std::string>();
                            return error({409, status == "ENDED" || status == "FAILED" ? "LIVE_REQUEST_ALREADY_USED" : "LIVE_SESSION_EXISTS",
                                          "Live session already exists", conflictJson(existing[0])});
                        }
                        if (!self->mediaAvailable()) return error(mediaError());
                        const auto id = randomHex(16);
                        const auto token = randomToken();
                        const auto name = "s_" + id;
                        LiveRepository::exec(tx, LiveRepository::Operation::InsertSession, [self, tx, roomId, id, name, token, reply, error](const auto&) {
                                LiveRepository::exec(tx, LiveRepository::Operation::AttachSession, [self, tx, id, name, token, reply, error](const auto&) {
                                        LiveRepository::exec(tx, LiveRepository::Operation::RawSession, [self, tx, name, token, reply, error](const auto& rows) {
                                                if (rows.empty()) return error(databaseError());
                                                auto data = conflictJson(rows[0]);
                                                data["publish_url"] = self->config_.addresses.rtmp_public_base + "/" +
                                                    self->config_.addresses.srs_app + "/" + name + "?token=" + token;
                                                data["prepare_deadline"] = jsonTime(rows[0]["prepare_deadline"]);
                                                commit(tx, reply, std::move(data), error);
                                            }, error, id);
                                    }, error, id, roomId);
                            }, error, id, roomId, requestId, name, tokenHashHex(token), self->config_.lifecycle.prepare_seconds);
                    }, error, roomId, requestId, requestId);
            }, error, userId);
    }, fail);
}

void LiveService::getSession(std::uint64_t userId, std::string id, Reply reply, Fail fail) {
    LiveRepository::exec(db_, LiveRepository::Operation::Session, [userId, reply, fail](const auto& rows) {
            if (rows.empty()) return fail(missingSession());
            if (rows[0]["owner_id"].template as<std::uint64_t>() != userId) return fail(forbidden());
            reply(sessionJson(rows[0]));
        }, fail, id);
}

void LiveService::stopSession(std::uint64_t userId, std::string id, Reply reply, Fail fail) {
    if (!db_) return fail(databaseError());
    auto self = shared_from_this();
    LiveRepository::exec(db_, LiveRepository::Operation::Session, [self, userId, id, reply, fail](const auto& rows) {
            if (rows.empty()) return fail(missingSession());
            const Record record(rows[0]);
            if (record.owner != userId) return fail(forbidden());
            if (record.status == "STOPPING" || record.status == "ENDED" || record.status == "FAILED")
                return reply(sessionJson(rows[0]));
            LiveRepository::exec(self->db_, LiveRepository::Operation::StopSession, [self, userId, id, record, reply, fail](const auto& changed) {
                    if (changed.affectedRows() != 0 && !record.identity.client.empty())
                        self->srs_->closePublisher(record.identity, [](bool) {});
                    self->getSession(userId, id, reply, fail);
                }, fail, id);
        }, fail, id);
}

void LiveService::list(unsigned page, unsigned pageSize, Reply reply, Fail fail) {
    const std::uint64_t offset = (static_cast<std::uint64_t>(page) - 1) * pageSize;
    LiveRepository::exec(db_, LiveRepository::Operation::Lobby, [page, pageSize, reply](const auto& rows) {
            Json::Value data(Json::objectValue), items(Json::arrayValue);
            for (std::size_t i = 0; i < rows.size() && i < pageSize; ++i) items.append(roomJson(rows[i]));
            data["items"] = std::move(items);
            data["page"] = page;
            data["page_size"] = pageSize;
            data["has_more"] = rows.size() > pageSize;
            reply(std::move(data));
        }, fail, pageSize + 1, offset);
}

void LiveService::room(std::uint64_t roomId, Reply reply, Fail fail) {
    LiveRepository::exec(db_, LiveRepository::Operation::Room, [reply, fail](const auto& rows) {
            if (rows.empty()) return fail({404, "ROOM_NOT_FOUND", "Room not found", {}});
            reply(roomJson(rows[0]));
        }, fail, roomId);
}

void LiveService::playback(std::uint64_t roomId, Reply reply, Fail fail) {
    if (!mediaAvailable()) return fail(mediaError());
    auto self = shared_from_this();
    LiveRepository::exec(db_, LiveRepository::Operation::Playback, [self, reply, fail](const auto& rows) {
            if (rows.empty()) return fail({404, "ROOM_NOT_FOUND", "Room not found", {}});
            if (!self->mediaAvailable()) return fail(mediaError());
            if (rows[0]["status"].isNull() || rows[0]["status"].template as<std::string>() != "LIVE")
                return fail({409, "ROOM_NOT_LIVE", "Room is not live", {}});
            Json::Value data(Json::objectValue);
            data["room_id"] = rows[0]["id"].template as<std::string>();
            data["live_session_id"] = rows[0]["live_session_id"].template as<std::string>();
            data["status"] = "LIVE";
            data["protocol"] = "http-flv";
            data["playback_url"] = self->config_.addresses.playback_public_base + "/" +
                self->config_.addresses.srs_app + "/" + rows[0]["stream_name"].template as<std::string>() + ".flv";
            reply(std::move(data));
        }, fail, roomId);
}

void LiveService::hook(Json::Value body, Reply reply, Fail fail) {
    if (!db_) return fail(databaseError());
    const auto action = hookString(body, "action");
    const auto name = hookString(body, "stream");
    const auto app = hookString(body, "app");
    const auto vhost = hookString(body, "vhost");
    const SrsIdentity identity{hookString(body, "server_id"), hookString(body, "service_id"),
                              hookString(body, "stream_id", false), hookString(body, "client_id")};
    if ((action != "on_publish" && action != "on_unpublish") || app != config_.addresses.srs_app ||
        vhost != config_.addresses.srs_vhost || name.size() != 34 || name.substr(0, 2) != "s_") return fail(forbidden());
    requireHexId(name.substr(2));
    if (!body["param"].isString()) return fail(invalidArgument());
    const auto token = publishToken(body["param"].asString());
    auto self = shared_from_this();
    LiveRepository::exec(db_, LiveRepository::Operation::Stream, [self, action, identity, token, reply, fail](const auto& rows) {
            if (rows.empty()) return action == "on_unpublish" ? reply(Json::Value(Json::objectValue)) : fail(forbidden());
            const Record record(rows[0]);
            if (action == "on_unpublish") {
                // Native unpublish may be delivered after credentials were revoked.
                // Trust the authenticated SRS source and exact publishing identity.
                if (!sameConnection(record.identity, identity) || !record.connected)
                    return reply(Json::Value(Json::objectValue));
                LiveRepository::exec(self->db_, LiveRepository::Operation::Unpublish, [self, record, reply](const auto& changed) {
                        if (changed.affectedRows() != 0) {
                            std::lock_guard<std::mutex> lock(self->mutex_);
                            self->observations_.erase(record.id);
                        }
                        reply(Json::Value(Json::objectValue));
                    }, fail, self->config_.lifecycle.reconnect_seconds, record.id, record.revision, record.generation,
                    identity.server, identity.service, identity.client);
                return;
            }
            if (!self->running_ || record.current != record.id || record.status == "STOPPING" ||
                record.status == "ENDED" || record.status == "FAILED" || (record.prepareExpired && !record.started) ||
                record.reconnectExpired || rows[0]["publish_token_hash"].isNull()) return fail(forbidden());
            const auto expectedHash = rows[0]["publish_token_hash"].template as<std::string>();
            std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
            crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(token.data()), token.size());
            if (expectedHash.size() != digest.size() || sodium_memcmp(expectedHash.data(), digest.data(), digest.size()) != 0)
                return fail(forbidden());
            if (record.connected) {
                if (sameConnection(record.identity, identity)) return reply(Json::Value(Json::objectValue));
                return fail(forbidden());
            }
            LiveRepository::exec(self->db_, LiveRepository::Operation::AdmitPublish, [reply, fail](const auto& changed) {
                    if (changed.affectedRows() != 1) return fail(forbidden());
                    reply(Json::Value(Json::objectValue));
                }, fail, identity.server, identity.service, identity.stream, identity.client,
                record.id, record.revision, record.generation);
        }, fail, name);
}

void LiveService::unavailable() {
    available_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    observations_.clear();
}

void LiveService::reconcile() {
    if (!running_ || busy_.exchange(true)) return;
    if (!db_ || !srs_) { unavailable(); busy_ = false; return; }
    auto self = shared_from_this();
    auto fail = [self](ApiError) { self->unavailable(); self->busy_ = false; };
    LiveRepository::exec(db_, LiveRepository::Operation::Nonterminal, [self](const auto& rows) {
            auto records = std::make_shared<std::vector<Record>>();
            for (const auto& row : rows) records->emplace_back(row);
            self->srs_->snapshot([self, records](SrsSnapshot snapshot) {
                if (!self->running_) { self->busy_ = false; return; }
                const auto observedAt = std::chrono::steady_clock::now();
                const auto freshness = std::chrono::seconds(2 * self->config_.srs.reconcile_seconds +
                                                           self->config_.srs.request_timeout_seconds);
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    if (observedAt - self->last_success_ > freshness) self->observations_.clear();
                }
                auto mediaReady = std::make_shared<std::atomic<bool>>(true);
                const auto complete = [self, observedAt, freshness, mediaReady](bool success) {
                    if (success && self->running_ && std::chrono::steady_clock::now() - observedAt <= freshness) {
                        { std::lock_guard<std::mutex> lock(self->mutex_); self->last_confirmed_ = utcNow();
                          self->last_success_ = std::chrono::steady_clock::now(); }
                        // After restart or an API outage, an active connection
                        // alone cannot re-certify a persisted LIVE session.
                        // Keep the new counter baseline while awaiting progress.
                        self->available_ = mediaReady->load();
                    } else self->unavailable();
                    self->busy_ = false;
                };
                if (records->empty()) return complete(true);
                auto remaining = std::make_shared<std::atomic<std::size_t>>(records->size());
                auto okay = std::make_shared<std::atomic<bool>>(true);
                auto sharedSnapshot = std::make_shared<SrsSnapshot>(std::move(snapshot));
                for (const auto& record : *records) {
                    self->confirm(record, *sharedSnapshot, [remaining, okay, complete](bool success) {
                        if (!success) *okay = false;
                        if (remaining->fetch_sub(1) == 1) complete(okay->load());
                    }, [mediaReady] { *mediaReady = false; });
                }
            }, [self] { self->unavailable(); self->busy_ = false; });
        }, fail);
}

void LiveService::finish(const Record& record, const std::string& status,
                         const std::string& reason, std::function<void(bool)> done) {
    auto self = shared_from_this();
    beginTransaction(db_, [self, record, status, reason, done](auto tx) {
        const auto error = transactionFailure(tx, [done](ApiError) { done(false); });
        LiveRepository::exec(tx, LiveRepository::Operation::LockRoom, [self, tx, record, status, reason, done, error](const auto&) {
                LiveRepository::exec(tx, LiveRepository::Operation::FinishSession, [self, tx, record, done, error](const auto& result) {
                        if (result.affectedRows() == 0) {
                            commit(tx, [done](Json::Value) { done(true); }, {}, error);
                            return;
                        }
                        LiveRepository::exec(tx, LiveRepository::Operation::ClearRoom, [self, tx, record, done, error](const auto&) {
                                commit(tx, [self, record, done](Json::Value) {
                                    { std::lock_guard<std::mutex> lock(self->mutex_); self->observations_.erase(record.id); }
                                    done(true);
                                }, {}, error);
                            }, error, record.room, record.id);
                    }, error, status, reason, record.id, record.revision, record.generation);
            }, error, record.room);
    }, [done](ApiError) { done(false); });
}

void LiveService::recover(const Record& record, bool close, std::function<void(bool)> done) {
    auto self = shared_from_this();
    if (!record.started) {
        LiveRepository::exec(db_, LiveRepository::Operation::ClearPreparingConnection, [done](const auto&) { done(true); }, [done](ApiError) { done(false); }, record.id, record.revision, record.generation);
        return;
    }
    LiveRepository::exec(db_, LiveRepository::Operation::Recover, [self, record, close, done](const auto& changed) {
            if (changed.affectedRows() != 0 && close)
                self->srs_->closePublisher(record.identity, [](bool) {});
            done(true);
        }, [done](ApiError) { done(false); }, config_.lifecycle.reconnect_seconds,
        close ? 1 : 0, record.id, record.revision, record.generation);
}

void LiveService::confirm(const Record& record, const SrsSnapshot& snapshot,
                         std::function<void(bool)> done, std::function<void()> pendingMedia) {
    auto self = shared_from_this();
    const auto match = std::find_if(snapshot.streams.begin(), snapshot.streams.end(),
        [this, &record](const SrsStream& stream) {
            return stream.active && stream.name == record.name && stream.app == config_.addresses.srs_app &&
                stream.vhost == config_.addresses.srs_vhost && sameConnection(stream.identity, record.identity);
        });
    auto process = [self, record, done, pendingMedia](std::optional<SrsStream> stream, bool connectionPresent) {
        if (!self->running_) return done(false);
        if (stream && (stream->name != record.name || stream->app != self->config_.addresses.srs_app ||
            stream->vhost != self->config_.addresses.srs_vhost || !sameConnection(stream->identity, record.identity)))
            return done(false);
        const bool active = stream.has_value();
        const auto publishingIdentity = active ? stream->identity : record.identity;
        if (record.status == "STOPPING") {
            if (!connectionPresent) return self->finish(record, record.reason == "PREPARE_TIMEOUT" || record.reason == "PUBLISH_FAILED" ? "FAILED" : "ENDED",
                                               record.reason.empty() ? "OWNER_STOPPED" : record.reason, done);
            self->srs_->closePublisher(publishingIdentity, [done](bool success) { done(success); });
            return;
        }
        const bool timeout = (record.status == "PREPARING" && record.prepareExpired) ||
                             (record.status == "RECONNECTING" && record.reconnectExpired);
        if (timeout) {
            const std::string reason = record.status == "PREPARING" ? "PREPARE_TIMEOUT" : "RECONNECT_TIMEOUT";
            if (!connectionPresent) return self->finish(record, record.status == "PREPARING" ? "FAILED" : "ENDED", reason, done);
            LiveRepository::exec(self->db_, LiveRepository::Operation::TimeoutStop, [self, publishingIdentity, done](const auto& changed) {
                    if (changed.affectedRows() == 0) return done(true);
                    self->srs_->closePublisher(publishingIdentity, [done](bool success) { done(success); });
                }, [done](ApiError) { done(false); }, reason, record.id, record.revision, record.generation);
            return;
        }
        if (!active) {
            // SRS creates client statistics before invoking on_publish. An
            // admitted client can exist before publish.active becomes true;
            // do not clear its admission or finish it merely for absent media.
            if (connectionPresent) {
                if (record.status == "LIVE") return self->recover(record, true, done);
                return done(true);
            }
            { std::lock_guard<std::mutex> lock(self->mutex_); self->observations_.erase(record.id); }
            return self->recover(record, false, done);
        }
        // A trusted unpublish invalidates this admission immediately. SRS may
        // still expose its old active statistics until the hook returns; only a
        // newly accepted on_publish may admit a publisher again.
        if (!record.connected) {
            if (record.status == "LIVE") return self->recover(record, true, done);
            return done(true);
        }
        const auto now = std::chrono::steady_clock::now();
        bool progressed = false;
        bool stalled = false;
        bool mediaConfirmed = false;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            auto found = self->observations_.find(record.id);
            if (found == self->observations_.end() || !sameIdentity(found->second.identity, stream->identity) ||
                found->second.generation != record.generation || stream->audio_frames < found->second.audio ||
                stream->video_frames < found->second.video) {
                self->observations_[record.id] = {stream->identity, record.generation, stream->audio_frames, stream->video_frames, now, false};
            } else {
                auto& previous = found->second;
                progressed = (stream->has_audio && stream->audio_frames > previous.audio) ||
                             (stream->has_video && stream->video_frames > previous.video);
                previous.audio = stream->audio_frames;
                previous.video = stream->video_frames;
                if (progressed) { previous.progress = now; previous.mediaConfirmed = true; }
                stalled = now - previous.progress >= std::chrono::seconds(self->config_.lifecycle.no_media_seconds);
                mediaConfirmed = previous.mediaConfirmed;
            }
        }
        if (record.status == "LIVE" && stalled) return self->recover(record, true, done);
        if (record.status == "LIVE" && !mediaConfirmed) pendingMedia();
        const bool ready = progressed && (stream->has_audio || stream->has_video);
        LiveRepository::exec(self->db_, LiveRepository::Operation::Progress, [done](const auto&) { done(true); }, [done](ApiError) { done(false); },
            stream->identity.stream, stream->recv_bytes, stream->audio_frames, stream->video_frames,
            ready ? 1 : 0, ready ? 1 : 0, ready ? 1 : 0, ready ? 1 : 0,
            record.id, record.revision, record.generation, stream->identity.server, stream->identity.service, stream->identity.client);
    };
    if (match != snapshot.streams.end()) return process(*match, true);
    auto absence = [self, record, process, done](std::optional<SrsStream> stream) {
        if (stream) return process(std::move(stream), true);
        if (record.identity.client.empty()) return process(std::nullopt, false);
        self->srs_->connectionExists(record.identity,
            [process](bool present) { process(std::nullopt, present); }, [done] { done(false); });
    };
    if (!record.identity.stream.empty() && record.identity.server == snapshot.server && record.identity.service == snapshot.service) {
        srs_->inspectPublisher(record.identity, std::move(absence), [done] { done(false); });
    } else {
        absence(std::nullopt);
    }
}

} // namespace lsl::server

