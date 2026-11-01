#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkReply;

namespace lsl::platform {

struct PlatformConfig {
    QUrl apiBase = QUrl(QStringLiteral("http://127.0.0.1:8090"));
    int requestTimeoutMs = 8000;
    int lobbyRefreshMs = 5000;
    int roomRefreshMs = 5000;
};

PlatformConfig loadPlatformConfig(const QString& path);

// Recovery intent survives closing a workspace, scoped to the signed-in user.
// No publish URL or credential is retained here; process death uses server timeouts.
struct PublishRecovery {
    QString roomId;
    QString clientRequestId;
    QString liveSessionId;
    bool stopRequested = false;
};

struct ApiResult {
    int httpStatus = 0;
    QString code;
    QString message;
    QJsonObject data;
    QJsonObject meta;
    bool ok() const;
};

// All access stays on the Qt application thread. Tokens live only in this object.
class ApiClient final : public QObject {
public:
    explicit ApiClient(PlatformConfig config, QObject* parent = nullptr);
    ~ApiClient() override;

    const PlatformConfig& config() const;
    void setAuthentication(QString token, QJsonObject user);
    void clearAuthentication();
    bool authenticated() const;
    QJsonObject user() const;
    PublishRecovery publishRecovery() const;
    void setPublishRecovery(PublishRecovery record);
    void clearPublishRecovery();

    // owner destruction and cancel(owner) suppress delivery, including aborted replies.
    // path is an API-relative absolute path, for example /api/v1/rooms?status=live.
    // Old authenticated replies deliver AUTHENTICATION_CHANGED with empty data,
    // allowing owners to finish pending UI work without accepting another login's data.
    // GET may retry once after a transient failure, within the same total timeout.
    // The returned reply represents only the first attempt; cancel via owner.
    QNetworkReply* request(QObject* owner, QByteArray method, QString path,
                           QJsonObject body, bool authenticated,
                           std::function<void(ApiResult)> callback);
    void cancel(QObject* owner);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::platform
