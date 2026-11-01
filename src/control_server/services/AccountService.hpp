#pragma once

#include "control_server/common/Support.hpp"
#include "control_server/config/ServerConfig.hpp"

#include <drogon/orm/DbClient.h>

#include <functional>
#include <memory>
#include <string>

namespace lsl::server {

class AccountService {
public:
    AccountService(drogon::orm::DbClientPtr database, const ServerConfig& config);
    ~AccountService();
    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;

    // Stop accepting password work and join workers before the DB pool closes.
    void stop();

    void registerAccount(Json::Value input, Reply reply, Fail fail);
    void login(Json::Value input, Reply reply, Fail fail);
    void authenticate(std::string bearerToken, std::function<void(Principal)> reply, Fail fail);
    void logout(const Principal& principal, Reply reply, Fail fail);
    void me(const Principal& principal, Reply reply, Fail fail);
    void room(const Principal& principal, Reply reply, Fail fail);
    void patchRoom(const Principal& principal, Json::Value input, Reply reply, Fail fail);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lsl::server
