#include "apps/platform/ApiClient.hpp"

#include <QFile>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lsl::platform {
namespace {
constexpr qint64 maximumResponseBytes = 2 * 1024 * 1024;

[[noreturn]] void invalidConfig() {
    // Never include file contents or a URL containing credentials in an exception.
    throw std::runtime_error("Invalid platform configuration");
}

void validateConfig(const PlatformConfig& config) {
    const auto& url = config.apiBase;
    if (!url.isValid() || url.isRelative() || url.host().isEmpty() ||
        (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")) ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
        config.requestTimeoutMs < 1000 || config.requestTimeoutMs > 60000 ||
        config.lobbyRefreshMs < 1000 || config.lobbyRefreshMs > 60000 ||
        config.roomRefreshMs < 1000 || config.roomRefreshMs > 60000) {
        invalidConfig();
    }
}

int integerOption(const QJsonObject& object, const QString& key, int fallback) {
    const auto value = object.value(key);
    if (value.isUndefined()) return fallback;
    if (!value.isDouble()) invalidConfig();
    const double number = value.toDouble();
    if (number < 1000 || number > 60000 || number != static_cast<int>(number)) invalidConfig();
    return static_cast<int>(number);
}

QString safeMessage(const QString& code, int status) {
    if (code == QStringLiteral("OK")) return {};
    if (code == QStringLiteral("TIMEOUT")) return QStringLiteral("请求超时，请重试。");
    if (code == QStringLiteral("NETWORK_ERROR")) return QStringLiteral("无法连接业务服务，请检查网络和平台配置。");
    if (code == QStringLiteral("INVALID_RESPONSE")) return QStringLiteral("业务服务返回了无法识别的数据。");
    if (code == QStringLiteral("REDIRECT_REJECTED")) return QStringLiteral("业务服务地址发生跳转，请核对平台配置。");
    if (code == QStringLiteral("RESPONSE_TOO_LARGE")) return QStringLiteral("业务服务响应过大。");
    if (code == QStringLiteral("AUTHENTICATION_CHANGED")) return QStringLiteral("登录状态已变化，请刷新后重试。");
    if (code == QStringLiteral("INVALID_CREDENTIALS")) return QStringLiteral("账号或密码错误。");
    if (code == QStringLiteral("USERNAME_EXISTS")) return QStringLiteral("此用户名已被注册。");
    if (code == QStringLiteral("LIVE_SESSION_EXISTS")) return QStringLiteral("房间已有有效直播，请先确认当前会话。");
    if (code == QStringLiteral("LIVE_REQUEST_ALREADY_USED")) return QStringLiteral("本次开播请求已结束，请重新发起开播。");
    if (code == QStringLiteral("ROOM_NOT_LIVE")) return QStringLiteral("该房间当前无法观看，请刷新直播大厅。");
    if (code == QStringLiteral("MEDIA_SERVICE_UNAVAILABLE")) return QStringLiteral("直播状态暂不可确认，请稍后重试。");
    if (code == QStringLiteral("DATABASE_UNAVAILABLE")) return QStringLiteral("数据服务暂不可用，请稍后重试。");
    if (code == QStringLiteral("AUTHENTICATION_BUSY")) return QStringLiteral("登录服务繁忙，请稍后重试。");
    if (code == QStringLiteral("UNAUTHENTICATED") || status == 401) return QStringLiteral("登录已失效，请重新登录。");
    if (code == QStringLiteral("INVALID_ARGUMENT") || status == 400) return QStringLiteral("填写的信息不符合要求，请检查后重试。");
    if (status == 403) return QStringLiteral("当前账号无权执行此操作。");
    if (status == 404) return QStringLiteral("请求的房间或直播会话不存在。");
    if (status == 409) return QStringLiteral("当前状态不允许此操作，请刷新后重试。");
    if (status == 429) return QStringLiteral("操作过于频繁，请稍后重试。");
    if (status >= 500) return QStringLiteral("业务服务暂不可用，请稍后重试。");
    return QStringLiteral("业务请求失败，请稍后重试。");
}

QString knownCode(const QJsonValue& value) {
    static const QSet<QString> codes{
        QStringLiteral("OK"), QStringLiteral("INVALID_ARGUMENT"), QStringLiteral("UNAUTHENTICATED"),
        QStringLiteral("INVALID_CREDENTIALS"), QStringLiteral("FORBIDDEN"), QStringLiteral("NOT_FOUND"),
        QStringLiteral("ROOM_NOT_FOUND"), QStringLiteral("LIVE_SESSION_NOT_FOUND"),
        QStringLiteral("METHOD_NOT_ALLOWED"), QStringLiteral("USERNAME_EXISTS"), QStringLiteral("CONFLICT"),
        QStringLiteral("LIVE_SESSION_EXISTS"), QStringLiteral("LIVE_REQUEST_ALREADY_USED"),
        QStringLiteral("ROOM_NOT_LIVE"), QStringLiteral("PAYLOAD_TOO_LARGE"),
        QStringLiteral("UNSUPPORTED_MEDIA_TYPE"), QStringLiteral("RATE_LIMITED"), QStringLiteral("INTERNAL_ERROR"),
        QStringLiteral("NOT_IMPLEMENTED"), QStringLiteral("DATABASE_UNAVAILABLE"),
        QStringLiteral("AUTHENTICATION_BUSY"), QStringLiteral("MEDIA_SERVICE_UNAVAILABLE"),
        QStringLiteral("SERVICE_UNAVAILABLE"), QStringLiteral("INVALID_REQUEST")};
    const auto code = value.toString();
    return codes.contains(code) ? code : QString{};
}
} // namespace

PlatformConfig loadPlatformConfig(const QString& path) {
    PlatformConfig result;
    QFile file(path);
    if (!file.exists()) return result;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024) invalidConfig();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) invalidConfig();
    const auto object = document.object();
    static const QSet<QString> keys{QStringLiteral("api_base"), QStringLiteral("request_timeout_ms"),
        QStringLiteral("lobby_refresh_ms"), QStringLiteral("room_refresh_ms")};
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.contains(it.key())) invalidConfig();
    }
    if (object.contains(QStringLiteral("api_base"))) {
        const auto value = object.value(QStringLiteral("api_base"));
        if (!value.isString()) invalidConfig();
        result.apiBase = QUrl(value.toString(), QUrl::StrictMode);
    }
    result.requestTimeoutMs = integerOption(object, QStringLiteral("request_timeout_ms"), result.requestTimeoutMs);
    result.lobbyRefreshMs = integerOption(object, QStringLiteral("lobby_refresh_ms"), result.lobbyRefreshMs);
    result.roomRefreshMs = integerOption(object, QStringLiteral("room_refresh_ms"), result.roomRefreshMs);
    validateConfig(result);
    return result;
}

bool ApiResult::ok() const {
    return httpStatus >= 200 && httpStatus < 300 && code == QStringLiteral("OK");
}

struct ApiClient::Impl {
    struct Request {
        QPointer<QObject> owner;
        QPointer<QNetworkReply> reply;
        std::function<void(ApiResult)> callback;
        quint64 authenticationRevision = 0;
        bool authenticated = false;
        bool cancelled = false;
        bool timedOut = false;
        bool tooLarge = false;
        QElapsedTimer clock;
        int attempts = 0;
        std::function<void()> send;
    };

    explicit Impl(PlatformConfig value) : config(std::move(value)) {}

    void complete(const std::shared_ptr<Request>& request, ApiResult result) {
        pending.erase(std::remove(pending.begin(), pending.end(), request), pending.end());
        if (request->cancelled || !request->owner) return;
        result.message = safeMessage(result.code, result.httpStatus);
        auto callback = std::move(request->callback);
        if (callback) callback(std::move(result));
    }

    PlatformConfig config;
    QNetworkAccessManager network;
    QString token;
    QJsonObject user;
    QHash<QString, PublishRecovery> publishRecovery;
    quint64 authenticationRevision = 0;
    std::vector<std::shared_ptr<Request>> pending;
};

ApiClient::ApiClient(PlatformConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(std::move(config))) {
    validateConfig(impl_->config);
}

ApiClient::~ApiClient() {
    // Abort only after suppressing every callback; replies belong to network.
    const auto pending = impl_->pending;
    for (const auto& request : pending) {
        request->cancelled = true;
        request->callback = {};
        if (request->reply) {
            request->reply->disconnect(this);
            request->reply->abort();
        }
    }
}

const PlatformConfig& ApiClient::config() const { return impl_->config; }

void ApiClient::setAuthentication(QString token, QJsonObject user) {
    // Header injection is rejected even if an incorrectly configured server returns it.
    if (token.isEmpty() || token.size() > 512 ||
        std::any_of(token.begin(), token.end(), [](QChar value) {
            return value.unicode() < 33 || value.unicode() > 126;
        })) {
        throw std::runtime_error("Invalid authentication response");
    }
    impl_->token = std::move(token);
    impl_->user = std::move(user);
    ++impl_->authenticationRevision;
}

void ApiClient::clearAuthentication() {
    impl_->token.clear();
    impl_->user = {};
    ++impl_->authenticationRevision;
}

bool ApiClient::authenticated() const { return !impl_->token.isEmpty(); }
QJsonObject ApiClient::user() const { return impl_->user; }

PublishRecovery ApiClient::publishRecovery() const {
    if (!authenticated()) return {};
    return impl_->publishRecovery.value(impl_->user.value(QStringLiteral("id")).toString());
}

void ApiClient::setPublishRecovery(PublishRecovery record) {
    const auto id = impl_->user.value(QStringLiteral("id")).toString();
    if (!authenticated() || id.isEmpty()) return;
    impl_->publishRecovery.insert(id, std::move(record));
}

void ApiClient::clearPublishRecovery() {
    if (authenticated()) impl_->publishRecovery.remove(impl_->user.value(QStringLiteral("id")).toString());
}

QNetworkReply* ApiClient::request(QObject* owner, QByteArray method, QString path,
                                  QJsonObject body, bool authenticated,
                                  std::function<void(ApiResult)> callback) {
    if (!owner) return nullptr;
    auto state = std::make_shared<Impl::Request>();
    state->owner = owner;
    state->callback = std::move(callback);
    state->authenticated = authenticated;
    state->authenticationRevision = impl_->authenticationRevision;
    state->clock.start();
    impl_->pending.push_back(state);

    const auto localFailure = [this, state](const QString& code) {
        QTimer::singleShot(0, this, [this, state, code] {
            ApiResult result;
            result.code = state->authenticated && state->authenticationRevision != impl_->authenticationRevision
                ? QStringLiteral("AUTHENTICATION_CHANGED") : code;
            impl_->complete(state, std::move(result));
        });
    };
    if (authenticated && !this->authenticated()) {
        localFailure(QStringLiteral("UNAUTHENTICATED"));
        return nullptr;
    }
    const QUrl relative(path.mid(1), QUrl::StrictMode);
    if (!path.startsWith(QLatin1Char('/')) || path.startsWith(QStringLiteral("//")) ||
        !relative.isValid() || !relative.isRelative() || relative.hasFragment() ||
        !relative.userInfo().isEmpty() || !relative.host().isEmpty() ||
        (method != "GET" && method != "POST" && method != "PATCH" && method != "DELETE")) {
        localFailure(QStringLiteral("INVALID_ARGUMENT"));
        return nullptr;
    }
    auto base = impl_->config.apiBase;
    if (!base.path().endsWith(QLatin1Char('/'))) base.setPath(base.path() + QLatin1Char('/'));
    const auto url = base.resolved(relative);
    if (url.scheme() != base.scheme() || url.host() != base.host() || url.port() != base.port()) {
        localFailure(QStringLiteral("INVALID_ARGUMENT"));
        return nullptr;
    }

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("LiveStreamLab"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    if (authenticated) request.setRawHeader("Authorization", "Bearer " + impl_->token.toLatin1());

    QByteArray payload;
    if (!body.isEmpty()) {
        payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    state->send = [this, weak = std::weak_ptr<Impl::Request>(state), request, method, payload] {
        const auto state = weak.lock();
        if (!state) return;
        if (state->cancelled || !state->owner) { impl_->complete(state, {}); return; }
        if (state->authenticated && state->authenticationRevision != impl_->authenticationRevision) {
            ApiResult result; result.code = QStringLiteral("AUTHENTICATION_CHANGED");
            impl_->complete(state, std::move(result)); return;
        }
        const auto remaining = impl_->config.requestTimeoutMs - state->clock.elapsed();
        if (remaining <= 0) {
            ApiResult result; result.code = QStringLiteral("TIMEOUT"); impl_->complete(state, std::move(result)); return;
        }
        ++state->attempts;
        auto* reply = method == "GET" ? impl_->network.get(request)
                                      : impl_->network.sendCustomRequest(request, method, payload);
        state->reply = reply;
        reply->setReadBufferSize(maximumResponseBytes + 1);
        auto* timeout = new QTimer(reply);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, reply, [state] {
            state->timedOut = true;
            if (state->reply) state->reply->abort();
        });
        timeout->start(static_cast<int>(remaining));
        connect(state->owner.data(), &QObject::destroyed, reply, [state] {
            state->cancelled = true;
            state->callback = {};
            if (state->reply) state->reply->abort();
        });
        connect(reply, &QNetworkReply::downloadProgress, reply, [state](qint64 received, qint64 total) {
            if (received <= maximumResponseBytes && total <= maximumResponseBytes) return;
            state->tooLarge = true;
            if (state->reply) state->reply->abort();
        });
        connect(reply, &QIODevice::readyRead, reply, [state] {
            if (!state->reply || state->reply->bytesAvailable() <= maximumResponseBytes) return;
            state->tooLarge = true;
            state->reply->abort();
        });
        connect(reply, &QNetworkReply::finished, this, [this, state, reply, timeout, method] {
            timeout->stop();
            ApiResult result;
            result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const auto bytes = reply->read(maximumResponseBytes + 1);
            if (state->timedOut) result.code = QStringLiteral("TIMEOUT");
            else if (state->tooLarge || bytes.size() > maximumResponseBytes) result.code = QStringLiteral("RESPONSE_TOO_LARGE");
            else if (result.httpStatus >= 300 && result.httpStatus < 400) result.code = QStringLiteral("REDIRECT_REJECTED");
            else if (result.httpStatus == 0 ||
                     (result.httpStatus >= 200 && result.httpStatus < 300 && reply->error() != QNetworkReply::NoError)) {
                result.code = QStringLiteral("NETWORK_ERROR");
            } else {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(bytes, &parseError);
                if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                    const auto object = document.object();
                    result.code = knownCode(object.value(QStringLiteral("code")));
                    result.data = object.value(QStringLiteral("data")).toObject();
                    result.meta = object.value(QStringLiteral("meta")).toObject();
                    if (result.httpStatus >= 200 && result.httpStatus < 300 &&
                        (result.code != QStringLiteral("OK") || !object.value(QStringLiteral("data")).isObject())) {
                        result.code = QStringLiteral("INVALID_RESPONSE");
                    } else if (result.code.isEmpty() ||
                               (result.httpStatus >= 400 && result.code == QStringLiteral("OK"))) {
                        result.code = QStringLiteral("HTTP_ERROR");
                    }
                } else {
                    result.code = result.httpStatus >= 200 && result.httpStatus < 300
                        ? QStringLiteral("INVALID_RESPONSE") : QStringLiteral("HTTP_ERROR");
                }
            }
            const bool authenticationChanged = state->authenticated && state->authenticationRevision != impl_->authenticationRevision;
            const bool transient = result.code == QStringLiteral("NETWORK_ERROR") ||
                result.httpStatus == 502 || result.httpStatus == 503 || result.httpStatus == 504;
            // One GET retry, including its backoff, shares the original total timeout.
            // Mutations and 401/429/protocol errors always return directly to the owner.
            if (method == "GET" && state->attempts < 2 && transient && !authenticationChanged &&
                !state->cancelled && state->owner && !state->timedOut && !state->tooLarge &&
                impl_->config.requestTimeoutMs - state->clock.elapsed() > 250) {
                reply->deleteLater(); state->reply.clear();
                QTimer::singleShot(250, this, [state] { state->send(); }); return;
            }
            if (authenticationChanged) {
                result = {};
                result.code = QStringLiteral("AUTHENTICATION_CHANGED");
            } else if (!state->cancelled && state->owner && result.httpStatus == 401 && state->authenticated) {
                clearAuthentication();
            }
            reply->deleteLater();
            impl_->complete(state, std::move(result));
        });
    };
    state->send();
    return state->reply.data();
}

void ApiClient::cancel(QObject* owner) {
    // abort() can synchronously emit finished(), so traverse a stable snapshot.
    const auto pending = impl_->pending;
    for (const auto& state : pending) {
        if (state->owner.data() != owner) continue;
        state->cancelled = true;
        state->callback = {};
        if (state->reply) state->reply->abort();
    }
}

} // namespace lsl::platform
