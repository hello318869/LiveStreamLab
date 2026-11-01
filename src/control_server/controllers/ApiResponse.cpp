#include "ApiResponse.hpp"

namespace lsl::server {
namespace {

drogon::HttpResponsePtr response(drogon::HttpStatusCode status, const char* code,
                                const char* message, Json::Value data) {
    Json::Value body(Json::objectValue);
    body["code"] = code;
    body["message"] = message;
    body["data"] = std::move(data);
    auto result = drogon::HttpResponse::newHttpJsonResponse(body);
    result->setStatusCode(status);
    result->addHeader("Cache-Control", "no-store");
    result->addHeader("X-Content-Type-Options", "nosniff");
    return result;
}

} // namespace

drogon::HttpResponsePtr apiSuccess(Json::Value data, int status, Json::Value meta) {
    auto result = response(static_cast<drogon::HttpStatusCode>(status), "OK", "", std::move(data));
    if (!meta.isNull()) {
        auto body = *result->getJsonObject();
        body["meta"] = std::move(meta);
        result = drogon::HttpResponse::newHttpJsonResponse(body);
        result->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
        result->addHeader("Cache-Control", "no-store");
        result->addHeader("X-Content-Type-Options", "nosniff");
    }
    return result;
}

drogon::HttpResponsePtr apiFailure(ApiError error, Json::Value meta) {
    auto result = response(static_cast<drogon::HttpStatusCode>(error.status), error.code.c_str(), error.message.c_str(), std::move(error.data));
    if (!meta.isNull()) {
        auto body = *result->getJsonObject();
        body["meta"] = std::move(meta);
        auto replacement = drogon::HttpResponse::newHttpJsonResponse(body);
        replacement->setStatusCode(result->statusCode());
        replacement->addHeader("Cache-Control", "no-store");
        replacement->addHeader("X-Content-Type-Options", "nosniff");
        result = std::move(replacement);
    }
    if (error.status == 429) result->addHeader("Retry-After", "60");
    return result;
}

drogon::HttpResponsePtr success(Json::Value data) {
    return response(drogon::k200OK, "OK", "", std::move(data));
}

drogon::HttpResponsePtr httpError(drogon::HttpStatusCode status) {
    // Never reflect exception text, request bodies, URLs or authentication data.
    switch (static_cast<int>(status)) {
    case 400: return response(status, "INVALID_ARGUMENT", "Invalid request", {});
    case 401: return response(status, "UNAUTHENTICATED", "Authentication required", {});
    case 403: return response(status, "FORBIDDEN", "Access denied", {});
    case 404: return response(status, "NOT_FOUND", "Resource not found", {});
    case 405: return response(status, "METHOD_NOT_ALLOWED", "Method not allowed", {});
    case 409: return response(status, "CONFLICT", "Resource conflict", {});
    case 413: return response(status, "PAYLOAD_TOO_LARGE", "Request body too large", {});
    case 415: return response(status, "UNSUPPORTED_MEDIA_TYPE", "JSON content required", {});
    case 429: return response(status, "RATE_LIMITED", "Too many requests", {});
    case 501: return response(status, "NOT_IMPLEMENTED", "Operation not implemented", {});
    case 503: return response(status, "SERVICE_UNAVAILABLE", "Service unavailable", {});
    default:
        if (static_cast<int>(status) >= 400 && static_cast<int>(status) < 500) {
            return response(status, "INVALID_REQUEST", "Request rejected", {});
        }
        return response(status, "INTERNAL_ERROR", "Internal server error", {});
    }
}

} // namespace lsl::server
