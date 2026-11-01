#pragma once

#include <drogon/HttpResponse.h>
#include "../common/Support.hpp"

namespace lsl::server {

drogon::HttpResponsePtr success(Json::Value data);
drogon::HttpResponsePtr httpError(drogon::HttpStatusCode status);
drogon::HttpResponsePtr apiSuccess(Json::Value data, int status = 200, Json::Value meta = {});
drogon::HttpResponsePtr apiFailure(ApiError error, Json::Value meta = {});

} // namespace lsl::server
