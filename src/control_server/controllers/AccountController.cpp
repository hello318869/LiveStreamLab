#include "AccountController.hpp"
#include "ApiResponse.hpp"
#include "control_server/common/RateLimiter.hpp"
#include "control_server/common/Support.hpp"
#include "control_server/filters/AuthFilter.hpp"
#include "control_server/services/AccountService.hpp"

#include <drogon/HttpAppFramework.h>
#include <atomic>

namespace lsl::server {
namespace {

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

struct Responses {
    Reply ok;
    Fail fail;
};

Responses responses(HttpCallback callback, int status = 200,
                    std::function<Json::Value()> meta = {}) {
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto readMeta = [meta] {
        if (!meta) return Json::Value();
        try { return meta(); }
        catch (...) {
            Json::Value unavailable(Json::objectValue);
            unavailable["media_service_status"] = "UNAVAILABLE";
            unavailable["last_confirmed_at"] = Json::Value();
            return unavailable;
        }
    };
    return {
        [callback, completed, status, readMeta](Json::Value data) {
            if (!completed->exchange(true)) callback(apiSuccess(std::move(data), status, readMeta()));
        },
        [callback, completed, readMeta](ApiError problem) {
            if (!completed->exchange(true)) callback(apiFailure(std::move(problem), readMeta()));
        }
    };
}

void noQuery(const drogon::HttpRequestPtr& request) {
    if (!request->getParameters().empty()) throw invalidArgument();
}

void noBody(const drogon::HttpRequestPtr& request) {
    noQuery(request);
    if (!request->body().empty()) throw invalidArgument();
}

Principal principal(const drogon::HttpRequestPtr& request) {
    const auto value = request->attributes()->get<Principal>("lsl.principal");
    if (value.user_id == 0 || value.auth_session_id.empty()) {
        throw ApiError{401, "UNAUTHENTICATED", "Authentication required", {}};
    }
    return value;
}

template <class Action>
void execute(const Responses& response, Action action) {
    try { action(); }
    catch (const ApiError& problem) { response.fail(problem); }
    catch (...) { response.fail(internalError()); }
}

} // namespace

void registerAccountControllers(drogon::HttpAppFramework& app,
                                std::shared_ptr<AccountService> accounts,
                                std::function<Json::Value()> mediaMeta) {
    const auto limiter = std::make_shared<RateLimiter>();
    app.registerHandler("/api/v1/auth/register",
        [accounts, limiter](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback), 201);
            execute(response, [&] {
                if (!limiter->allow("register:" + peerIp(request), 5, 60)) {
                    throw ApiError{429, "RATE_LIMITED", "Too many requests", {}};
                }
                noQuery(request);
                accounts->registerAccount(strictBody(request, {"username", "nickname", "password"},
                    {"username", "nickname", "password"}), response.ok, response.fail);
            });
        }, {drogon::Post});

    app.registerHandler("/api/v1/auth/login",
        [accounts, limiter](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback));
            execute(response, [&] {
                if (!limiter->allow("login:" + peerIp(request), 10, 60)) {
                    throw ApiError{429, "RATE_LIMITED", "Too many requests", {}};
                }
                noQuery(request);
                accounts->login(strictBody(request, {"username", "password"}, {"username", "password"}),
                                response.ok, response.fail);
            });
        }, {drogon::Post});

    const auto authentication = AuthFilter::classTypeName();
    app.registerHandler("/api/v1/auth/logout",
        [accounts](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback));
            execute(response, [&] {
                noBody(request);
                accounts->logout(principal(request), response.ok, response.fail);
            });
        }, {drogon::Post, authentication});

    app.registerHandler("/api/v1/me",
        [accounts](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback));
            execute(response, [&] {
                noBody(request);
                accounts->me(principal(request), response.ok, response.fail);
            });
        }, {drogon::Get, authentication});

    app.registerHandler("/api/v1/me/room",
        [accounts, mediaMeta](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback), 200, mediaMeta);
            execute(response, [&] {
                noBody(request);
                accounts->room(principal(request), response.ok, response.fail);
            });
        }, {drogon::Get, authentication});

    app.registerHandler("/api/v1/me/room",
        [accounts, mediaMeta](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback), 200, mediaMeta);
            execute(response, [&] {
                noQuery(request);
                accounts->patchRoom(principal(request), strictBody(request, {"title", "category"}, {}),
                                    response.ok, response.fail);
            });
        }, {drogon::Patch, authentication});
}

} // namespace lsl::server
