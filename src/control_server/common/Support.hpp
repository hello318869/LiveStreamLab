#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace lsl::server {

struct ApiError {
    int status;
    std::string code;
    std::string message;
    Json::Value data;
};
using Reply = std::function<void(Json::Value)>;
using Fail = std::function<void(ApiError)>;
struct Principal {
    std::uint64_t user_id = 0;
    std::string auth_session_id;
};

ApiError databaseError();
ApiError internalError();
ApiError invalidArgument();

// Every query is asynchronous and binds its values. Exceptions from result
// conversion/validation also return through the same public error boundary.
template <class Client, class Callback, class... Args>
void query(const std::shared_ptr<Client>& client, const std::string& sql,
           Callback ok, Fail fail, Args&&... args) {
    if (!client) { fail(databaseError()); return; }
    const std::weak_ptr<Client> weak = client;
    client->execSqlAsync(sql,
        [ok = std::move(ok), fail, weak](const drogon::orm::Result& rows) {
            try { ok(rows); }
            catch (const ApiError& error) {
                if (auto tx = std::dynamic_pointer_cast<drogon::orm::Transaction>(weak.lock())) tx->rollback();
                fail(error);
            }
            catch (...) {
                if (auto tx = std::dynamic_pointer_cast<drogon::orm::Transaction>(weak.lock())) tx->rollback();
                fail(internalError());
            }
        },
        [fail](const drogon::orm::DrogonDbException&) { fail(databaseError()); },
        std::forward<Args>(args)...);
}

void beginTransaction(const drogon::orm::DbClientPtr& db,
    std::function<void(std::shared_ptr<drogon::orm::Transaction>)> callback, Fail fail);
// Commit happens when the final transaction reference is released. Do not
// capture that reference in Reply/Fail or in the commit callback itself.
void commit(std::shared_ptr<drogon::orm::Transaction> tx, Reply done, Json::Value data, Fail fail);

Json::Value strictBody(const drogon::HttpRequestPtr& request,
    std::initializer_list<const char*> allowed, std::initializer_list<const char*> required);
std::string requireText(const Json::Value& object, const char* key, std::size_t min, std::size_t max);
std::string requireUsername(const Json::Value& object, const char* key = "username");
std::string requirePassword(const Json::Value& object, const char* key = "password");
std::string requireCategory(const Json::Value& object, const char* key = "category");
std::string requireUuid(const std::string& value);
std::string requireHexId(const std::string& value);
std::uint64_t requireDecimalId(const std::string& value);
std::string randomHex(std::size_t bytes);
std::string randomToken();
std::string tokenHashHex(const std::string& token);
bool constantTimeEqual(const std::string& left, const std::string& right);
std::string canonicalIp(const std::string& value);
std::string peerIp(const drogon::HttpRequestPtr& request);
std::string isoTime(std::string databaseTime);
Json::Value jsonTime(const drogon::orm::Field& value);
Json::Value roomJson(const drogon::orm::Row& row);

} // namespace lsl::server
