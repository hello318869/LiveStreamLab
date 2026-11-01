#include "LiveController.hpp"
#include "ApiResponse.hpp"
#include "control_server/common/RateLimiter.hpp"
#include "control_server/filters/AuthFilter.hpp"
#include "control_server/services/LiveService.hpp"

#include <drogon/HttpAppFramework.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <limits>
#include <map>

namespace lsl::server {
namespace {
using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
struct Responses { Reply ok; Fail fail; };

Responses responses(HttpCallback callback, const std::shared_ptr<LiveService>& service, int status = 200) {
    auto completed = std::make_shared<std::atomic<bool>>(false);
    return {
        [callback, service, completed, status](Json::Value data) {
            if (!completed->exchange(true)) callback(apiSuccess(std::move(data), status, service->mediaMeta()));
        },
        [callback, service, completed](ApiError error) {
            if (!completed->exchange(true)) callback(apiFailure(std::move(error), service->mediaMeta()));
        }
    };
}

template <class Action>
void execute(const Responses& response, Action action) {
    try { action(); }
    catch (const ApiError& error) { response.fail(error); }
    catch (...) { response.fail(internalError()); }
}

void noBody(const drogon::HttpRequestPtr& request, bool allowQuery = false) {
    if (!request->body().empty() || (!allowQuery && !request->query().empty())) throw invalidArgument();
}

Principal principal(const drogon::HttpRequestPtr& request) {
    const auto identity = request->attributes()->get<Principal>("lsl.principal");
    if (identity.user_id == 0) throw ApiError{401, "UNAUTHENTICATED", "Authentication required", {}};
    return identity;
}

std::map<std::string, std::string> listQuery(const std::string& query) {
    std::map<std::string, std::string> parameters;
    std::size_t start = 0;
    while (start < query.size()) {
        const auto end = query.find('&', start);
        const auto pair = query.substr(start, end == std::string::npos ? end : end - start);
        const auto equal = pair.find('=');
        if (equal == std::string::npos) throw invalidArgument();
        const auto key = pair.substr(0, equal);
        if (key != "page" && key != "page_size" && key != "status") throw invalidArgument();
        if (!parameters.emplace(key, pair.substr(equal + 1)).second) throw invalidArgument();
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == query.size()) throw invalidArgument();
    }
    return parameters;
}

unsigned pageNumber(const std::map<std::string, std::string>& parameters, const char* key,
                    unsigned fallback, unsigned maximum) {
    const auto found = parameters.find(key);
    if (found == parameters.end()) return fallback;
    const auto value = requireDecimalId(found->second);
    if (value > maximum) throw invalidArgument();
    return static_cast<unsigned>(value);
}

Json::Value nativeBody(const drogon::HttpRequestPtr& request) {
    auto contentType = request->getHeader("content-type");
    contentType = contentType.substr(0, contentType.find(';'));
    while (!contentType.empty() && contentType.back() == ' ') contentType.pop_back();
    std::transform(contentType.begin(), contentType.end(), contentType.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (contentType != "application/json") throw invalidArgument();
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["allowComments"] = false;
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 32;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    const auto body = request->body();
    Json::Value value;
    std::string errors;
    if (!reader->parse(body.data(), body.data() + body.size(), &value, &errors) || !value.isObject())
        throw invalidArgument();
    return value;
}

} // namespace

void registerLiveControllers(drogon::HttpAppFramework& app, std::shared_ptr<LiveService> service) {
    auto limiter = std::make_shared<RateLimiter>();
    const auto authentication = AuthFilter::classTypeName();
    app.registerHandler("/api/v1/me/room/live-sessions",
        [service, limiter](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback), service, 201);
            execute(response, [&] {
                if (!request->query().empty()) throw invalidArgument();
                const auto user = principal(request);
                if (!limiter->allow("publish-user:" + std::to_string(user.user_id), 10, 60) ||
                    !limiter->allow("publish-ip:" + peerIp(request), 60, 60))
                    throw ApiError{429, "RATE_LIMITED", "Too many requests", {}};
                const auto body = strictBody(request, {"client_request_id"}, {"client_request_id"});
                if (!body["client_request_id"].isString()) throw invalidArgument();
                service->create(user.user_id, requireUuid(body["client_request_id"].asString()), response.ok, response.fail);
            });
        }, {drogon::Post, authentication});

    app.registerHandler("/api/v1/live-sessions/{1}",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback, std::string id) {
            const auto response = responses(std::move(callback), service);
            execute(response, [&] {
                noBody(request);
                service->getSession(principal(request).user_id, requireHexId(id), response.ok, response.fail);
            });
        }, {drogon::Get, authentication});

    app.registerHandler("/api/v1/live-sessions/{1}/stop",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback, std::string id) {
            const auto response = responses(std::move(callback), service);
            execute(response, [&] {
                noBody(request);
                service->stopSession(principal(request).user_id, requireHexId(id), response.ok, response.fail);
            });
        }, {drogon::Post, authentication});

    app.registerHandler("/api/v1/rooms",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            const auto response = responses(std::move(callback), service);
            execute(response, [&] {
                noBody(request, true);
                const auto parameters = listQuery(request->query());
                if (parameters.count("status") && parameters.at("status") != "live") throw invalidArgument();
                service->list(pageNumber(parameters, "page", 1, std::numeric_limits<unsigned>::max()),
                              pageNumber(parameters, "page_size", 20, 100), response.ok, response.fail);
            });
        }, {drogon::Get});

    app.registerHandler("/api/v1/rooms/{1}",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback, std::string id) {
            const auto response = responses(std::move(callback), service);
            execute(response, [&] {
                noBody(request);
                service->room(requireDecimalId(id), response.ok, response.fail);
            });
        }, {drogon::Get});

    app.registerHandler("/api/v1/rooms/{1}/playback",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback, std::string id) {
            const auto response = responses(std::move(callback), service);
            execute(response, [&] {
                noBody(request);
                service->playback(requireDecimalId(id), response.ok, response.fail);
            });
        }, {drogon::Get});

    app.registerHandler("/internal/srs/hooks",
        [service](const drogon::HttpRequestPtr& request, HttpCallback&& callback) {
            auto completed = std::make_shared<std::atomic<bool>>(false);
            auto respond = [callback = std::move(callback), completed](int code) {
                if (completed->exchange(true)) return;
                Json::Value data(Json::objectValue);
                data["code"] = code;
                auto response = drogon::HttpResponse::newHttpJsonResponse(data);
                response->setStatusCode(drogon::k200OK);
                response->addHeader("Cache-Control", "no-store");
                callback(response);
            };
            try {
                const auto& query = request->query();
                if (query.size() != 47 || query.substr(0, 4) != "key=" ||
                    !service->trustedHook(peerIp(request), query.substr(4))) return respond(1);
                // Fields originate in the fixed SRS version. Additional native
                // metadata is ignored, never used for trust or SQL parameters.
                service->hook(nativeBody(request), [respond](Json::Value) { respond(0); }, [respond](ApiError) { respond(1); });
            } catch (...) { respond(1); }
        }, {drogon::Post});
}

} // namespace lsl::server
