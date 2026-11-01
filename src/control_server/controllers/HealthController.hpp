#pragma once

namespace drogon { class HttpAppFramework; }

namespace lsl::server {
void registerHealthController(drogon::HttpAppFramework& app);
} // namespace lsl::server
