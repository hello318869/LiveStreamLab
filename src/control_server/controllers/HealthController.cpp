#include "HealthController.hpp"
#include "ApiResponse.hpp"

#include <drogon/HttpAppFramework.h>

namespace lsl::server {

void registerHealthController(drogon::HttpAppFramework& app) {
    app.registerHandler("/health", [](const drogon::HttpRequestPtr&,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        Json::Value data(Json::objectValue);
        data["service"] = "control_server";
        data["stage"] = "N03";
        data["status"] = "ok";
        // Liveness only: this does not claim MySQL/SRS or media readiness.
        callback(success(std::move(data)));
    }, {drogon::Get});
}

} // namespace lsl::server
