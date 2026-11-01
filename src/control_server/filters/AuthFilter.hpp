#pragma once

#include <drogon/HttpFilter.h>
#include <memory>

namespace lsl::server {

class AccountService;

class AuthFilter final : public drogon::HttpFilter<AuthFilter, false> {
public:
    explicit AuthFilter(std::shared_ptr<AccountService> accounts);
    void doFilter(const drogon::HttpRequestPtr& request,
                  drogon::FilterCallback&& reject,
                  drogon::FilterChainCallback&& next) override;

private:
    std::shared_ptr<AccountService> accounts_;
};

} // namespace lsl::server
