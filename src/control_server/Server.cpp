#include "Server.hpp"
#include "config/ServerConfig.hpp"
#include "controllers/ApiResponse.hpp"
#include "controllers/HealthController.hpp"
#include "controllers/AccountController.hpp"
#include "controllers/LiveController.hpp"
#include "filters/AuthFilter.hpp"
#include "services/AccountService.hpp"
#include "services/LiveService.hpp"
#include "srs/SrsClient.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/config.h>
#include <sodium.h>

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string_view>

#if !USE_MYSQL
#error "Drogon must be built with MySQL support using MariaDB Connector/C"
#endif

namespace lsl::server {
namespace {

// Drogon's MySQL errors can contain an interpolated SQL statement; its HTTP
// diagnostics can contain full URLs. Retain only a source location from those
// records, so credentials/hashes never reach ordinary process logs.
void configureDependencyLogging() {
    trantor::Logger::setOutputFunction([](const char* text, std::uint64_t size) {
        static std::mutex mutex;
        const std::string_view record(text, static_cast<std::size_t>(size));
        const auto marker = record.rfind(" - ");
        std::string location;
        if (marker != std::string_view::npos) {
            auto tail = std::string(record.substr(marker + 3));
            if (!tail.empty() && tail.back() == '\n') tail.pop_back();
            static const std::regex pattern("[A-Za-z0-9_./-]{1,160}:[0-9]{1,8}");
            if (std::regex_match(tail, pattern)) location = std::move(tail);
        }
        std::lock_guard<std::mutex> lock(mutex);
        std::clog << "[control_server] dependency diagnostic";
        if (!location.empty()) std::clog << " at " << location;
        std::clog << '\n';
    }, [] { std::clog.flush(); });
}

std::string quoteConnectionValue(const std::string& value) {
    if (value.find('\n') != std::string::npos || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("Invalid database connection value");
    }
    std::string result = "'";
    for (const char c : value) {
        if (c == '\'' || c == '\\') result += '\\';
        result += c;
    }
    return result + "'";
}

drogon::orm::DbClientPtr databaseClient(const ServerConfig& config) {
    if (!config.database.enabled) return {};
    const char* password = std::getenv(config.database.password_env.c_str());
    if (password == nullptr || *password == '\0') {
        throw std::runtime_error("Database password environment variable is empty");
    }
    const auto& d = config.database;
    // Drogon 1.9.11's quoted-value parser is greedy within a line. Separate
    // fields with newlines so a quoted value cannot consume the next field.
    auto connection = "host=" + quoteConnectionValue(d.host) + "\nport=" + std::to_string(d.port) +
        "\ndbname=" + quoteConnectionValue(d.name) + "\nuser=" + quoteConnectionValue(d.user) +
        "\npassword=" + quoteConnectionValue(password) + "\nclient_encoding='utf8mb4'";
    // One Drogon asynchronous pool, injected into all repositories. Construct it
    // before registering handlers rather than looking up an unstarted app pool.
    auto db = drogon::orm::DbClient::newMysqlClient(connection, d.connections);
    sodium_memzero(connection.data(), connection.size());
    db->setTimeout(d.timeout_seconds);
    return db;
}

} // namespace

void runServer(const ServerConfig& config) {
    if (sodium_init() < 0) throw std::runtime_error("Cannot initialize the credential library");
    configureDependencyLogging();
    auto& app = drogon::app();
    app.setLogLevel(trantor::Logger::kWarn);
    app.setLogLocalTime(false);
    app.disableSession();
    app.enableServerHeader(false);
    // This process serves API routes only; never expose JSON/config/source files.
    app.setFileTypes({});
    app.setImplicitPageEnable(false);
    app.setThreadNum(config.http.threads);
    app.setClientMaxBodySize(config.http.max_body_bytes);
    app.setClientMaxMemoryBodySize(config.http.max_body_bytes);
    app.setIdleConnectionTimeout(30);
    app.setCustomErrorHandler([](drogon::HttpStatusCode status, const drogon::HttpRequestPtr&) {
        return httpError(status);
    });
    app.setExceptionHandler([](const std::exception&, const drogon::HttpRequestPtr&,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        std::clog << "[control_server] unhandled request error\n";
        callback(httpError(drogon::k500InternalServerError));
    });
    auto db = databaseClient(config);
    auto accounts = std::make_shared<AccountService>(db, config);
    auto srs = std::make_shared<SrsClient>(config);
    auto live = std::make_shared<LiveService>(db, config, srs);
    app.registerFilter(std::make_shared<AuthFilter>(accounts));
    registerAccountControllers(app, accounts, [live] { return live->mediaMeta(); });
    registerLiveControllers(app, live);
    registerHealthController(app);
    app.registerBeginningAdvice([live] { live->start(); });
    app.addListener(config.http.address, config.http.port);
    std::clog << "[control_server] starting control API; HTTP port=" << config.http.port << '\n';
    // Drogon's default SIGTERM handler requests an orderly event-loop shutdown.
    app.run();
    live->stop();
    accounts->stop();
    if (db) db->closeAll();
}

} // namespace lsl::server
