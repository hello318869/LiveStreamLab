#pragma once

#include "control_server/common/Support.hpp"

#include <drogon/orm/DbClient.h>
#include <functional>
#include <string>

namespace lsl::server {

class AccountRepository {
public:
    using Credentials = std::function<void(Json::Value user, std::string hash, bool exists)>;

    explicit AccountRepository(drogon::orm::DbClientPtr database);
    bool enabled() const;
    void createAccount(std::string username, std::string nickname, std::string encodedHash, Reply reply, Fail fail);
    void credentials(std::string username, Credentials reply, Fail fail);
    void createAuth(std::uint64_t userId, std::string sessionId, std::string digest,
                    unsigned ttl, Reply reply, Fail fail);
    void authenticate(std::string digest, std::function<void(Principal)> reply, Fail fail);
    void revoke(const Principal& principal, Reply reply, Fail fail);
    void me(const Principal& principal, Reply reply, Fail fail);
    void room(const Principal& principal, Reply reply, Fail fail);
    void patchRoom(const Principal& principal, bool titlePresent, std::string title,
                   bool categoryPresent, std::string category, Reply reply, Fail fail);

private:
    drogon::orm::DbClientPtr database_;
};

} // namespace lsl::server
