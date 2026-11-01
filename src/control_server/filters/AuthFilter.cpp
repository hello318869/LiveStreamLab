#include "AuthFilter.hpp"
#include "control_server/common/Support.hpp"
#include "control_server/controllers/ApiResponse.hpp"
#include "control_server/services/AccountService.hpp"

#include <atomic>

namespace lsl::server {

AuthFilter::AuthFilter(std::shared_ptr<AccountService> accounts) : accounts_(std::move(accounts)) {}

void AuthFilter::doFilter(const drogon::HttpRequestPtr& request,
                          drogon::FilterCallback&& reject,
                          drogon::FilterChainCallback&& next) {
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto fail = [reject = std::move(reject), completed](ApiError problem) {
        if (!completed->exchange(true)) reject(apiFailure(std::move(problem)));
    };
    try {
        const auto& parameters = request->getParameters();
        const auto& authorization = request->getHeader("authorization");
        constexpr const char* prefix = "Bearer ";
        if (parameters.find("access_token") != parameters.end() ||
            parameters.find("token") != parameters.end() ||
            authorization.compare(0, 7, prefix) != 0) {
            fail({401, "UNAUTHENTICATED", "Authentication required", {}});
            return;
        }
        accounts_->authenticate(authorization.substr(7),
            [request, completed, next = std::move(next)](Principal principal) {
                if (completed->exchange(true)) return;
                request->attributes()->insert("lsl.principal", principal);
                next();
            }, fail);
    } catch (const ApiError& problem) { fail(problem); }
      catch (...) { fail(internalError()); }
}

} // namespace lsl::server
