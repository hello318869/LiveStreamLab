#include "AccountRepository.hpp"

namespace lsl::server {
namespace {

ApiError unauthenticated() { return {401, "UNAUTHENTICATED", "Authentication required", {}}; }

Json::Value userJson(const drogon::orm::Row& row) {
    Json::Value result(Json::objectValue);
    result["id"] = row["id"].as<std::string>();
    result["username"] = row["username"].as<std::string>();
    result["nickname"] = row["nickname"].as<std::string>();
    result["created_at"] = isoTime(row["created_at"].as<std::string>());
    return result;
}

constexpr const char* personalRoomSql =
    "SELECT r.id,r.owner_id,u.nickname,r.title,r.category,r.current_session_id,"
    "s.status,s.started_at,r.updated_at FROM rooms r JOIN users u ON u.id=r.owner_id "
    "LEFT JOIN live_sessions s ON s.id=r.current_session_id AND s.room_id=r.id WHERE r.owner_id=?";

} // namespace

AccountRepository::AccountRepository(drogon::orm::DbClientPtr database) : database_(std::move(database)) {}
bool AccountRepository::enabled() const { return database_ != nullptr; }

void AccountRepository::createAccount(std::string username, std::string nickname,
                                     std::string encodedHash, Reply reply, Fail fail) {
    const auto database = database_;
    beginTransaction(database, [database, username, nickname, encodedHash, reply, fail](auto tx) {
        std::weak_ptr<drogon::orm::Transaction> weakTx(tx);
        Fail rollback = [database, weakTx, username, fail](ApiError problem) {
            if (auto transaction = weakTx.lock()) transaction->rollback();
            // The pinned driver does not expose MySQL error numbers. Never
            // classify conflicts by parsing/logging SQL or exception contents.
            query(database, "SELECT id FROM users WHERE username=?",
                [fail, problem](const drogon::orm::Result& found) {
                    if (!found.empty()) fail({409, "USERNAME_EXISTS", "Username already exists", {}});
                    else fail(problem);
                }, [fail, problem](ApiError) { fail(problem); }, username);
        };
        query(tx, "INSERT INTO users(username,nickname,password_hash,created_at) VALUES(?,?,?,UTC_TIMESTAMP(6))",
            [tx, nickname, reply, rollback](const drogon::orm::Result& inserted) {
                const auto userId = inserted.insertId();
                const auto title = nickname + "的直播间";
                query(tx, "INSERT INTO rooms(owner_id,title,category,created_at,updated_at) "
                          "VALUES(?,?,'general',UTC_TIMESTAMP(6),UTC_TIMESTAMP(6))",
                    [tx, userId, reply, rollback](const drogon::orm::Result&) {
                        query(tx, "SELECT id,username,nickname,created_at FROM users WHERE id=?",
                            [tx, userId, reply, rollback](const drogon::orm::Result& users) {
                                if (users.empty()) { rollback(databaseError()); return; }
                                const auto user = userJson(users.front());
                                query(tx, personalRoomSql,
                                    [tx, user, reply, rollback](const drogon::orm::Result& rooms) {
                                        if (rooms.empty()) { rollback(databaseError()); return; }
                                        Json::Value result(Json::objectValue);
                                        result["user"] = user;
                                        result["room"] = roomJson(rooms.front());
                                        commit(tx, reply, std::move(result), rollback);
                                    }, rollback, userId);
                            }, rollback, userId);
                    }, rollback, userId, title);
            }, rollback, username, nickname, encodedHash);
    }, fail);
}

void AccountRepository::credentials(std::string username, Credentials reply, Fail fail) {
    query(database_, "SELECT id,username,nickname,password_hash,created_at FROM users WHERE username=?",
        [reply](const drogon::orm::Result& found) {
            if (found.empty()) { reply({}, {}, false); return; }
            reply(userJson(found.front()), found.front()["password_hash"].as<std::string>(), true);
        }, fail, username);
}

void AccountRepository::createAuth(std::uint64_t userId, std::string sessionId, std::string digest,
                                  unsigned ttl, Reply reply, Fail fail) {
    const auto database = database_;
    query(database,
        "INSERT INTO auth_sessions(id,user_id,token_hash,created_at,expires_at) "
        "VALUES(?,?,UNHEX(?),UTC_TIMESTAMP(6),DATE_ADD(UTC_TIMESTAMP(6),INTERVAL ? SECOND))",
        [database, sessionId, reply, fail](const drogon::orm::Result&) {
            query(database, "SELECT expires_at FROM auth_sessions WHERE id=?",
                [reply, fail](const drogon::orm::Result& sessions) {
                    if (sessions.empty()) { fail(databaseError()); return; }
                    Json::Value result(Json::objectValue);
                    result["expires_at"] = isoTime(sessions.front()["expires_at"].as<std::string>());
                    reply(std::move(result));
                }, fail, sessionId);
        }, fail, sessionId, userId, digest, ttl);
}

void AccountRepository::authenticate(std::string digest, std::function<void(Principal)> reply, Fail fail) {
    query(database_,
        "SELECT id,user_id FROM auth_sessions WHERE token_hash=UNHEX(?) "
        "AND revoked_at IS NULL AND expires_at>UTC_TIMESTAMP(6)",
        [reply, fail](const drogon::orm::Result& sessions) {
            if (sessions.empty()) { fail(unauthenticated()); return; }
            reply(Principal{sessions.front()["user_id"].as<std::uint64_t>(), sessions.front()["id"].as<std::string>()});
        }, fail, digest);
}

void AccountRepository::revoke(const Principal& principal, Reply reply, Fail fail) {
    query(database_, "UPDATE auth_sessions SET revoked_at=UTC_TIMESTAMP(6) WHERE id=? AND user_id=? "
                     "AND revoked_at IS NULL AND expires_at>UTC_TIMESTAMP(6)",
        [reply, fail](const drogon::orm::Result& result) {
            if (result.affectedRows() != 1) { fail(unauthenticated()); return; }
            reply(Json::Value(Json::objectValue));
        }, fail, principal.auth_session_id, principal.user_id);
}

void AccountRepository::me(const Principal& principal, Reply reply, Fail fail) {
    query(database_, "SELECT id,username,nickname,created_at FROM users WHERE id=?",
        [reply, fail](const drogon::orm::Result& result) {
            if (result.empty()) { fail(unauthenticated()); return; }
            reply(userJson(result.front()));
        }, fail, principal.user_id);
}

void AccountRepository::room(const Principal& principal, Reply reply, Fail fail) {
    query(database_, personalRoomSql,
        [reply, fail](const drogon::orm::Result& result) {
            if (result.empty()) { fail({404, "ROOM_NOT_FOUND", "Room not found", {}}); return; }
            reply(roomJson(result.front()));
        }, fail, principal.user_id);
}

void AccountRepository::patchRoom(const Principal& principal, bool titlePresent, std::string title,
                                 bool categoryPresent, std::string category, Reply reply, Fail fail) {
    const auto database = database_;
    query(database,
        "UPDATE rooms SET title=IF(?,?,title),category=IF(?,?,category),updated_at=UTC_TIMESTAMP(6) WHERE owner_id=?",
        [database, principal, reply, fail](const drogon::orm::Result&) {
            query(database, personalRoomSql,
                [reply, fail](const drogon::orm::Result& result) {
                    if (result.empty()) { fail({404, "ROOM_NOT_FOUND", "Room not found", {}}); return; }
                    reply(roomJson(result.front()));
                }, fail, principal.user_id);
        }, fail, static_cast<int>(titlePresent), title, static_cast<int>(categoryPresent), category, principal.user_id);
}

} // namespace lsl::server
