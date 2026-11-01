#pragma once

#include <memory>
namespace drogon { class HttpAppFramework; }
namespace lsl::server {
class LiveService;
void registerLiveControllers(drogon::HttpAppFramework& app,
                             std::shared_ptr<LiveService> service);
}
