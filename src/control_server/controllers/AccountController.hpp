#pragma once

#include <json/json.h>
#include <functional>
#include <memory>

namespace drogon { class HttpAppFramework; }

namespace lsl::server {
class AccountService;
void registerAccountControllers(drogon::HttpAppFramework& app,
                                std::shared_ptr<AccountService> accounts,
                                std::function<Json::Value()> mediaMeta);
} // namespace lsl::server
