#include "publisher/ui/PublisherControl.hpp"

#include "publisher/capture/MediaInput.hpp"
#include "publisher/pipeline/PublisherSession.hpp"
#include "common/stats/PerformanceStats.hpp"
#include "apps/platform/ApiClient.hpp"
#include "common/logger/Redaction.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QStackedWidget>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace lsl::ui {
namespace {
using State = pipeline::PublisherSessionState;
constexpr int audioDeviceRole = Qt::UserRole + 1;

QString utf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
std::string bytes(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}
bool active(State state) noexcept {
    return state == State::Starting || state == State::Previewing || state == State::Streaming ||
           state == State::Reconnecting || state == State::Stopping;
}
QString stateText(State state, bool previewRequested) {
    switch (state) {
    case State::Idle: return QStringLiteral("待机");
    case State::Starting: return previewRequested ? QStringLiteral("正在打开设备") : QStringLiteral("正在连接");
    case State::Previewing: return QStringLiteral("本地预览中");
    case State::Streaming: return QStringLiteral("直播中");
    case State::Reconnecting: return QStringLiteral("正在重连");
    case State::Stopping: return QStringLiteral("正在停止");
    case State::Stopped: return QStringLiteral("已停止");
    case State::Failed: return QStringLiteral("运行失败");
    }
    return QStringLiteral("未知状态");
}
QString durationText(double seconds) {
    const auto total = static_cast<qulonglong>(std::max(0.0, std::floor(seconds)));
    return QStringLiteral("%1:%2:%3").arg(total / 3600, 2, 10, QLatin1Char('0'))
        .arg(total / 60 % 60, 2, 10, QLatin1Char('0')).arg(total % 60, 2, 10, QLatin1Char('0'));
}
QString queueText(const stats::QueueDepth& queue, const QString& unit, bool sampled) {
    if (!sampled) return QStringLiteral("—");
    if (queue.capacity == 0) return QStringLiteral("未启用");
    return QStringLiteral("%1 / %2 %3").arg(static_cast<qulonglong>(queue.size))
        .arg(static_cast<qulonglong>(queue.capacity)).arg(unit);
}
void showQueue(QLabel* value, const stats::QueueDepth& queue, const QString& unit, bool sampled) {
    value->setText(queueText(queue, unit, sampled));
    value->setToolTip(sampled ? QStringLiteral("本次会话峰值：%1 %2").arg(static_cast<qulonglong>(queue.highWaterMark)).arg(unit)
        : QStringLiteral("等待首个统计样本"));
}

enum class Glyph { Studio, Dashboard, Camera, Sliders, Log, Play, Stop, Copy, Refresh };
QIcon icon(Glyph glyph, QColor color = QColor(132, 151, 157)) {
    QPixmap image(48, 48);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::Studio:
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(100, 201, 177));
        painter.drawRoundedRect(QRectF(1, 1, 22, 22), 6, 6);
        painter.setPen(QPen(QColor(12, 31, 28), 2.1, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(7, 7), QPointF(7, 16));
        painter.drawLine(QPointF(7, 16), QPointF(15, 16));
        painter.drawLine(QPointF(12, 7), QPointF(17, 7));
        painter.drawLine(QPointF(17, 7), QPointF(17, 12));
        break;
    case Glyph::Dashboard:
        for (const auto& point : {QPointF(3, 3), QPointF(14, 3), QPointF(3, 14), QPointF(14, 14)})
            painter.drawRoundedRect(QRectF(point, QSizeF(7, 7)), 1.5, 1.5);
        break;
    case Glyph::Camera:
        painter.drawRoundedRect(QRectF(3, 6, 13, 12), 2, 2);
        painter.drawPolyline(QPolygonF{QPointF(16, 10), QPointF(21, 7), QPointF(21, 17), QPointF(16, 14)});
        break;
    case Glyph::Sliders:
        for (int y : {6, 12, 18}) painter.drawLine(QPointF(3, y), QPointF(21, y));
        painter.setBrush(QColor(14, 22, 27));
        painter.drawEllipse(QPointF(8, 6), 2, 2);
        painter.drawEllipse(QPointF(16, 12), 2, 2);
        painter.drawEllipse(QPointF(10, 18), 2, 2);
        break;
    case Glyph::Log:
        painter.drawRoundedRect(QRectF(4, 3, 16, 18), 2, 2);
        for (int y : {8, 12, 16}) painter.drawLine(QPointF(8, y), QPointF(16, y));
        break;
    case Glyph::Play:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(8, 5), QPointF(19, 12), QPointF(8, 19)});
        break;
    case Glyph::Stop:
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(7, 7, 10, 10), 1, 1);
        break;
    case Glyph::Copy:
        painter.drawRoundedRect(QRectF(8, 7, 12, 14), 2, 2);
        painter.drawPolyline(QPolygonF{QPointF(15, 4), QPointF(4, 4), QPointF(4, 16)});
        break;
    case Glyph::Refresh:
        painter.drawArc(QRectF(4, 4, 16, 16), 35 * 16, 290 * 16);
        painter.drawPolyline(QPolygonF{QPointF(16, 3), QPointF(21, 4), QPointF(20, 9)});
        break;
    }
    return QIcon(image);
}
QLabel* label(const QString& text, const char* name) {
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName(QString::fromLatin1(name));
    return result;
}
QFrame* panel(const char* name = "panel") {
    auto* frame = new QFrame;
    frame->setObjectName(QString::fromLatin1(name));
    return frame;
}
void sectionLabel(QVBoxLayout* layout, const QString& text) { layout->addWidget(label(text, "fieldLabel")); }

class PreviewWidget final : public QWidget {
public:
    PreviewWidget() {
        setMinimumSize(360, 190);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override { return {640, 360}; }
    void setFrame(QImage image) { image_ = std::move(image); update(); }
    void clearFrame() { image_ = {}; update(); }
    void setStatus(const QString& value, bool live) {
        if (status_ == value && live_ == live) return;
        status_ = value; live_ = live; update();
    }
    void setOutputSize(const QSize& size) { outputSize_ = size; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(outer, 10, 10);
        painter.setClipPath(clip);
        painter.fillRect(rect(), QColor(6, 12, 16));
        // 16:9 视口；实际源画面等比缩放，非16:9源保留黑边。
        QSizeF viewport(16, 9);
        viewport.scale(outer.size(), Qt::KeepAspectRatio);
        const QRectF video(QPointF((width() - viewport.width()) / 2, (height() - viewport.height()) / 2), viewport);
        painter.fillRect(video, QColor(7, 14, 18));
        if (!image_.isNull()) {
            QSizeF fitted = image_.size();
            fitted.scale(video.size(), Qt::KeepAspectRatio);
            painter.drawImage(QRectF(QPointF(video.center().x() - fitted.width() / 2,
                video.center().y() - fitted.height() / 2), fitted), image_);
        } else {
            painter.setPen(QPen(QColor(26, 39, 45), 1));
            for (int x = 0; x < width(); x += 34) painter.drawLine(x, 0, x, height());
            for (int y = 0; y < height(); y += 34) painter.drawLine(0, y, width(), y);
            const QRectF camera(width() / 2.0 - 24, height() / 2.0 - 36, 40, 30);
            painter.setPen(QPen(QColor(67, 91, 99), 1.8));
            painter.drawRoundedRect(camera, 5, 5);
            painter.drawPolyline(QPolygonF{QPointF(camera.right(), camera.top() + 9),
                QPointF(camera.right() + 13, camera.top() + 3), QPointF(camera.right() + 13, camera.bottom() - 3),
                QPointF(camera.right(), camera.bottom() - 9)});
            painter.setPen(QColor(170, 186, 191));
            painter.drawText(QRectF(10, height() / 2.0 + 10, width() - 20, 28), Qt::AlignCenter, QStringLiteral("画面预览"));
            painter.setPen(QColor(91, 113, 122));
            auto small = painter.font(); small.setPointSizeF(8.5); painter.setFont(small);
            painter.drawText(QRectF(10, height() / 2.0 + 37, width() - 20, 25), Qt::AlignCenter,
                QStringLiteral("开始预览或直播后，显示实际采集画面"));
        }
        const QRectF badge(14, 14, 118, 28);
        painter.setPen(QPen(QColor(53, 74, 78), 1));
        painter.setBrush(QColor(11, 22, 27, 225));
        painter.drawRoundedRect(badge, 7, 7);
        painter.setPen(Qt::NoPen);
        painter.setBrush(live_ ? QColor(105, 210, 183) : QColor(130, 148, 156));
        painter.drawEllipse(QPointF(28, 28), 3, 3);
        painter.setPen(QColor(196, 216, 217));
        painter.drawText(badge.adjusted(25, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft, status_);
        painter.setPen(QColor(161, 181, 187));
        painter.drawText(QRectF(14, height() - 30, width() - 28, 20), Qt::AlignRight | Qt::AlignVCenter,
            QStringLiteral("%1 × %2").arg(outputSize_.width()).arg(outputSize_.height()));
        painter.setClipping(false);
        painter.setPen(QPen(QColor(34, 48, 55), 1)); painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(outer, 10, 10);
    }
private:
    QImage image_;
    QString status_ = QStringLiteral("等待采集");
    bool live_ = false;
    QSize outputSize_{1280, 720};
};

class MetricCard final : public QFrame {
public:
    MetricCard(const QString& title, const QString& hint) {
        setObjectName(QStringLiteral("metricCard"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 5, 12, 5); layout->setSpacing(2);
        layout->addWidget(label(title, "metricTitle"));
        value_ = label(QStringLiteral("—"), "metricValue"); layout->addWidget(value_);
        hint_ = label(hint, "metricHint"); layout->addWidget(hint_);
        setFixedHeight(72);
    }
    void setValue(const QString& value) { value_->setText(value); }
    void setHint(const QString& value) { hint_->setText(value); }
private:
    QLabel* value_ = nullptr;
    QLabel* hint_ = nullptr;
};

const char* stylesheet = R"QSS(
QWidget { color: #c5d1d5; font-size: 12px; }
QWidget#publisherWindow { background: #0b1115; }
QFrame#sidebar { background: #0d151a; border-right: 1px solid #1d2a31; }
QLabel#brandName { color: #e1eeef; font-size: 16px; font-weight: 600; letter-spacing: 1px; }
QLabel#brandSub { color: #66818c; font-size: 10px; letter-spacing: 2px; }
QLabel#navCaption { color: #506875; font-size: 10px; letter-spacing: 2px; }
QLabel#pageTitle { color: #e4edef; font-size: 23px; font-weight: 600; }
QLabel#sectionTitle { color: #d5e1e4; font-size: 13px; font-weight: 600; }
QLabel#subtle, QLabel#metricHint { color: #6e8792; font-size: 10px; }
QLabel#fieldLabel, QLabel#metricTitle { color: #90a7b0; font-size: 11px; }
QLabel#statsValue { color: #bcd2d6; font-size: 11px; }
QLabel#metricValue { color: #e1eded; font-size: 20px; font-weight: 600; }
QFrame#panel, QFrame#metricCard { background: #101a20; border: 1px solid #24333b; border-radius: 10px; }
QFrame#settingsPanel { background: #0e171d; border-left: 1px solid #1e2c34; }
QFrame#settingsCard { background: #111d24; border: 1px solid #273943; border-radius: 9px; }
QFrame#footer { background: #0c1419; border-top: 1px solid #203039; }
QPushButton { background: #142129; border: 1px solid #30424c; border-radius: 6px; padding: 7px 13px; color: #aebfc6; }
QPushButton:hover { background: #1b2c35; border-color: #46626b; color: #e2eeee; }
QPushButton:pressed { background: #0e1d24; }
QPushButton:disabled { color: #4b626c; background: #101b21; border-color: #25343c; }
QPushButton#startButton { background: #64c5ad; color: #102c25; border: 1px solid #7dd6c0; font-weight: 600; padding: 9px 17px; }
QPushButton#startButton:hover { background: #7bd4be; }
QPushButton#startButton:disabled { background: #203b35; border-color: #2b4d45; color: #5f8278; }
QPushButton#stopButton { color: #d3a4a0; border-color: #5b3e3d; background: #261d20; }
QPushButton#stopButton:hover { background: #362427; border-color: #865650; }
QPushButton#stopButton:disabled { color: #655451; background: #17191d; border-color: #343138; }
QPushButton#navButton { text-align: left; padding: 11px 15px; border: 1px solid transparent; border-radius: 7px; background: transparent; color: #829aa6; }
QPushButton#navButton:hover { color: #d1e0e2; background: #15232a; }
QPushButton#navButton[active="true"] { color: #8cddc6; background: #16302c; border-color: #284b42; }
QPushButton#smallButton { padding: 5px 9px; font-size: 10px; }
QPushButton#linkButton { background: transparent; border: none; color: #79bfae; padding: 2px 3px; font-size: 10px; }
QLineEdit, QComboBox, QSpinBox { background: #0b151c; color: #c4d5dd; border: 1px solid #30434e; border-radius: 6px; padding: 7px 9px; selection-background-color: #315c52; min-height: 17px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #5b9c8b; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: #647b86; background: #0c171d; border-color: #253841; }
QLineEdit#addressDisplay { border: none; background: transparent; padding: 2px 0; font-family: Consolas; color: #8bc7b9; font-size: 11px; }
QComboBox::drop-down { width: 24px; border: none; }
QComboBox QAbstractItemView { background: #14242d; color: #bdd0d8; selection-background-color: #285246; outline: 0; border: 1px solid #34505b; }
QSpinBox::up-button, QSpinBox::down-button { border: none; background: transparent; width: 20px; }
QCheckBox { spacing: 8px; color: #96adb5; font-size: 11px; }
QCheckBox::indicator { width: 15px; height: 15px; background: #0b151b; border: 1px solid #39535e; border-radius: 4px; }
QCheckBox::indicator:checked { background: #5eb69f; border-color: #89d2bf; }
QPlainTextEdit { background: #0c151b; border: none; color: #8aa5b1; font-family: Consolas, 'Microsoft YaHei UI'; font-size: 10px; padding: 3px 0; selection-background-color: #285448; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { width: 6px; margin: 3px 0; background: transparent; }
QScrollBar::handle:vertical { background: #2b414b; min-height: 30px; border-radius: 3px; }
QScrollBar::handle:vertical:hover { background: #42606b; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QLabel#errorBanner { background: #2a1d1f; color: #e5b6b0; border: 1px solid #583b3b; border-radius: 6px; padding: 8px 11px; font-size: 11px; }
)QSS";

class PublisherWindow final : public QWidget {
public:
    PublisherWindow(config::PublisherConfig config, logging::Logger& logger, platform::ApiClient* api = nullptr)
        : initial_(std::move(config)), logger_(logger), session_(logger), api_(api) {
        logger_.enableMemoryLog(512);
        setObjectName(QStringLiteral("publisherWindow"));
        setWindowTitle(QStringLiteral("Local Live Studio · 主播控制"));
        setWindowIcon(icon(Glyph::Studio));
        resize(1280, 820); setMinimumSize(1120, 760);
        setStyleSheet(QString::fromLatin1(stylesheet));
        auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
        pages_ = new QStackedWidget; outer->addWidget(pages_);
        auto* workspace = new QWidget; pages_->addWidget(workspace);
        auto* root = new QHBoxLayout(workspace);
        root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);
        root->addWidget(buildSidebar());
        auto* main = new QWidget;
        auto* mainLayout = new QVBoxLayout(main);
        mainLayout->setContentsMargins(0, 0, 0, 0); mainLayout->setSpacing(0);
        auto* body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0); body->setSpacing(0);
        body->addWidget(buildStudio(), 1); body->addWidget(buildSettings());
        mainLayout->addLayout(body, 1); mainLayout->addWidget(buildFooter());
        root->addWidget(main, 1);
        connectControls(); refreshDevices(); updateSourceDetails(); updateOutputSummary();
        logger_.log(logging::Level::Info, "publisher_ui", "Qt Widgets publisher controls ready; capture starts only on Preview or Start");
        timer_ = new QTimer(this);
        timer_->setInterval(100);
        connect(timer_, &QTimer::timeout, this, [this] { pollSafely(); });
        timer_->start(); poll();
        if (api_) {
            loginPage_ = buildLogin(); pages_->addWidget(loginPage_);
            platformTimer_ = new QTimer(this); platformTimer_->setInterval(api_->config().roomRefreshMs);
            connect(platformTimer_, &QTimer::timeout, this, [this] { refreshPlatformState(); });
            platformTimer_->start();
            if (api_->authenticated()) { restoreRecovery(); loadRoom(); }
            else pages_->setCurrentWidget(loginPage_);
        }
    }
    ~PublisherWindow() override {
        if (timer_) timer_->stop();
        if (api_) api_->cancel(this);
        session_.requestStop();
        // session_ 成员先于 QWidget 基类析构，join 后才释放子控件与 QApplication。
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (!closing_) {
            closeClock_.start();
            closing_ = true; pendingLive_.reset(); session_.requestStop();
            if (api_) { cancelRequested_ = true; requestPlatformStop(); }
        }
        if (active(session_.snapshot().state) || (platformBusy() && !remoteCloseGraceExpired())) {
            event->ignore(); start_->setEnabled(false); stop_->setEnabled(false); previewButton_->setEnabled(false);
            banner_->setText(QStringLiteral("正在停止采集并释放资源，完成后自动关闭窗口。")); banner_->show();
        } else event->accept();
    }
private:
    bool platformBusy() const {
        return authBusy_ || roomBusy_ || applying_ || stopBusy_ || recoveryBusy_ || logoutBusy_;
    }
    bool remoteCloseGraceExpired() const {
        return closing_ && closeClock_.isValid() &&
            closeClock_.elapsed() >= (api_ ? std::min(api_->config().requestTimeoutMs, 2000) : 0);
    }
    static bool terminal(const QString& status) {
        return status == QStringLiteral("ENDED") || status == QStringLiteral("FAILED");
    }
    static bool validSessionId(const QString& id) {
        static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
        return pattern.match(id).hasMatch();
    }
    static bool validRoomId(const QString& id) {
        static const QRegularExpression pattern(QStringLiteral("^[1-9][0-9]{0,19}$"));
        return pattern.match(id).hasMatch();
    }
    static bool validRequestId(const QString& id) {
        static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        return pattern.match(id).hasMatch();
    }
    static bool sameRecovery(const platform::PublishRecovery& a, const platform::PublishRecovery& b) {
        return a.roomId == b.roomId && a.clientRequestId == b.clientRequestId &&
            a.liveSessionId == b.liveSessionId && a.stopRequested == b.stopRequested;
    }
    void restoreRecovery() {
        auto record = api_->publishRecovery();
        if (!validRoomId(record.roomId) ||
            (!validRequestId(record.clientRequestId) && !validSessionId(record.liveSessionId))) return;
        // 新窗口/重新登录时没有存活的本地发布会话，恢复原有明确的回收责任。
        record.stopRequested = true; api_->setPublishRecovery(record);
        roomId_ = record.roomId;
        if (validRequestId(record.clientRequestId)) requestId_ = record.clientRequestId;
        uncertainApply_ = record.liveSessionId.isEmpty(); stopPostAttempts_ = 0;
    }
    void rememberAttempt() {
        if (!api_->authenticated() || !validRoomId(roomId_) || !validRequestId(requestId_)) return;
        api_->setPublishRecovery({roomId_, requestId_, ownedSessionId_, false});
    }
    void clearRecovery(const platform::PublishRecovery& record) {
        if (sameRecovery(api_->publishRecovery(), record)) api_->clearPublishRecovery();
    }
    bool canVerifyCancelledRequest() const {
        const auto record = api_->publishRecovery();
        return roomLoaded_ && platformStatus_ == QStringLiteral("OFFLINE") && observedSessionId_.isEmpty() &&
            record.stopRequested && record.liveSessionId.isEmpty() && record.roomId == roomId_ &&
            validRequestId(record.clientRequestId);
    }
    void verifyCancelledRequest() {
        const auto record = api_->publishRecovery();
        if (!canVerifyCancelledRequest()) return;
        // 用户明确核实旧申请；不存在时可能新建一个准备会话，取得响应后立即结束，绝不启动媒体。
        requestId_ = record.clientRequestId; cancelRequested_ = true; uncertainApply_ = true;
        pendingLive_.reset(); pendingPlatformConfig_.reset(); stopPostAttempts_ = 0;
        applying_ = true; setPlatformState(QStringLiteral("正在核实并结束上次申请；如尚未创建，将创建准备记录后立即取消。"));
        api_->request(this, "POST", QStringLiteral("/api/v1/me/room/live-sessions"),
            {{QStringLiteral("client_request_id"), requestId_}}, true,
            [this, requestId = requestId_](platform::ApiResult result) { finishApply(requestId, std::move(result)); });
    }
    void refreshPlatformState() {
        if (closing_ || platformBusy() || needsLogin_ || logoutRequested_ || !api_->authenticated()) return;
        if (!roomLoaded_) { loadRoom(); return; }
        const auto record = api_->publishRecovery();
        if (record.stopRequested || (!record.clientRequestId.isEmpty() && ownedSessionId_.isEmpty())) {
            recoverOwnPreparation(); return;
        }
        queryPlatformSession();
    }
    static bool knownSessionStatus(const QString& status) {
        return status == QStringLiteral("PREPARING") || status == QStringLiteral("LIVE") ||
            status == QStringLiteral("RECONNECTING") || status == QStringLiteral("STOPPING") || terminal(status);
    }
    static bool matchesSession(const QJsonObject& payload, const QString& sessionId, const QString& roomId) {
        return validSessionId(sessionId) && validRoomId(roomId) &&
            payload.value(QStringLiteral("live_session_id")).toString() == sessionId &&
            payload.value(QStringLiteral("room_id")).toString() == roomId &&
            knownSessionStatus(payload.value(QStringLiteral("status")).toString());
    }
    bool validOwnRoom(const QJsonObject& payload, const QString& expectedRoomId = {}) const {
        const QString id = payload.value(QStringLiteral("room_id")).toString();
        const auto currentSession = payload.value(QStringLiteral("current_session_id"));
        const QString session = currentSession.toString();
        const QString status = payload.value(QStringLiteral("status")).toString();
        return validRoomId(id) && (expectedRoomId.isEmpty() || id == expectedRoomId) &&
            payload.value(QStringLiteral("owner")).toObject().value(QStringLiteral("id")).toString() ==
                api_->user().value(QStringLiteral("id")).toString() &&
            ((currentSession.isNull() && status == QStringLiteral("OFFLINE")) ||
             (currentSession.isString() && validSessionId(session) && knownSessionStatus(status)));
    }
    static QString platformStatusText(const QString& status) {
        if (status == QStringLiteral("PREPARING")) return QStringLiteral("等待服务器确认媒体就绪");
        if (status == QStringLiteral("LIVE")) return QStringLiteral("正在直播 · 已由服务器确认");
        if (status == QStringLiteral("RECONNECTING")) return QStringLiteral("直播连接恢复中");
        if (status == QStringLiteral("STOPPING")) return QStringLiteral("正在下播，等待服务器确认");
        if (status == QStringLiteral("ENDED")) return QStringLiteral("直播已结束");
        if (status == QStringLiteral("FAILED")) return QStringLiteral("直播会话失败，可重新开播");
        return QStringLiteral("尚未开播");
    }
    void setPlatformState(const QString& text) {
        if (platformState_) platformState_->setText(text);
    }
    QWidget* buildLogin() {
        auto* page = new QWidget; auto* outer = new QVBoxLayout(page);
        outer->setContentsMargins(260, 50, 260, 50); outer->addStretch();
        outer->addWidget(label(QStringLiteral("登录后开启直播"), "pageTitle"));
        outer->addWidget(label(QStringLiteral("公开直播无需登录即可观看。登录状态仅保存在本次运行中。"), "fieldLabel"));
        sectionLabel(outer, QStringLiteral("用户名 · 3–32 位小写字母、数字或下划线"));
        username_ = new QLineEdit; username_->setMaxLength(32); outer->addWidget(username_);
        sectionLabel(outer, QStringLiteral("昵称 · 注册时填写"));
        nickname_ = new QLineEdit; nickname_->setMaxLength(64); outer->addWidget(nickname_);
        sectionLabel(outer, QStringLiteral("密码 · UTF-8 编码 8–128 字节"));
        password_ = new QLineEdit; password_->setEchoMode(QLineEdit::Password); password_->setMaxLength(128);
        outer->addWidget(password_);
        auto* row = new QHBoxLayout;
        login_ = new QPushButton(QStringLiteral("登录")); register_ = new QPushButton(QStringLiteral("注册账号"));
        row->addWidget(login_); row->addWidget(register_); outer->addLayout(row);
        authState_ = label({}, "fieldLabel"); authState_->setWordWrap(true); outer->addWidget(authState_);
        auto* home = new QPushButton(QStringLiteral("返回直播大厅")); outer->addWidget(home);
        connect(home, &QPushButton::clicked, this, &QWidget::close);
        connect(login_, &QPushButton::clicked, this, [this] { authenticate(false); });
        connect(register_, &QPushButton::clicked, this, [this] { authenticate(true); });
        connect(password_, &QLineEdit::returnPressed, this, [this] { authenticate(false); });
        outer->addStretch(); return page;
    }
    QWidget* buildRoomSettings() {
        auto* card = panel("settingsCard"); auto* layout = new QVBoxLayout(card);
        layout->addWidget(label(QStringLiteral("我的平台直播间"), "sectionTitle"));
        account_ = label({}, "fieldLabel"); account_->setWordWrap(true); layout->addWidget(account_);
        sectionLabel(layout, QStringLiteral("直播标题")); roomTitle_ = new QLineEdit;
        roomTitle_->setMaxLength(240); layout->addWidget(roomTitle_);
        sectionLabel(layout, QStringLiteral("分类")); category_ = new QComboBox;
        const QStringList titles{QStringLiteral("综合"), QStringLiteral("游戏"), QStringLiteral("音乐"), QStringLiteral("教育"), QStringLiteral("科技")};
        const QStringList values{QStringLiteral("general"), QStringLiteral("gaming"), QStringLiteral("music"), QStringLiteral("education"), QStringLiteral("technology")};
        for (int i = 0; i < values.size(); ++i) category_->addItem(titles[i], values[i]);
        layout->addWidget(category_);
        auto* actions = new QHBoxLayout; saveRoom_ = new QPushButton(QStringLiteral("保存房间"));
        refreshRoom_ = new QPushButton(QStringLiteral("刷新状态"));
        actions->addWidget(saveRoom_); actions->addWidget(refreshRoom_); layout->addLayout(actions);
        platformState_ = label(QStringLiteral("正在读取房间"), "fieldLabel"); platformState_->setWordWrap(true);
        layout->addWidget(platformState_);
        logout_ = new QPushButton(QStringLiteral("退出登录")); layout->addWidget(logout_);
        connect(saveRoom_, &QPushButton::clicked, this, [this] { saveRoom({}); });
        connect(refreshRoom_, &QPushButton::clicked, this, [this] {
            if (observedSessionId_.isEmpty()) loadRoom(); else queryPlatformSession();
        });
        connect(logout_, &QPushButton::clicked, this, [this] { beginLogout(); });
        return card;
    }
    void authenticate(bool registration) {
        if (authBusy_ || closing_) return;
        const QString username = username_->text();
        const QString nickname = nickname_->text();
        const QString password = password_->text(); password_->clear();
        const auto passwordSize = password.toUtf8().size();
        static const QRegularExpression namePattern(QStringLiteral("^[a-z0-9_]{3,32}$"));
        if (!namePattern.match(username).hasMatch() || passwordSize < 8 || passwordSize > 128 ||
            (registration && (nickname.trimmed().isEmpty() || nickname.toUcs4().size() > 32))) {
            authState_->setText(QStringLiteral("请检查用户名、密码长度，以及注册昵称。密码已清空，请重新输入。")); return;
        }
        authBusy_ = true; login_->setEnabled(false); register_->setEnabled(false);
        authState_->setText(registration ? QStringLiteral("正在注册…") : QStringLiteral("正在登录…"));
        QJsonObject body{{QStringLiteral("username"), username}, {QStringLiteral("password"), password}};
        if (registration) body.insert(QStringLiteral("nickname"), nickname);
        api_->request(this, "POST", registration ? QStringLiteral("/api/v1/auth/register") : QStringLiteral("/api/v1/auth/login"),
            body, false, [this, registration](platform::ApiResult result) {
                authBusy_ = false; login_->setEnabled(true); register_->setEnabled(true);
                if (closing_) return;
                if (!result.ok()) { authState_->setText(result.message); return; }
                if (registration) { authState_->setText(QStringLiteral("注册成功，请输入密码后登录。")); return; }
                const auto token = result.data.value(QStringLiteral("access_token")).toString();
                const auto user = result.data.value(QStringLiteral("user")).toObject();
                if (token.isEmpty() || !validRoomId(user.value(QStringLiteral("id")).toString())) {
                    authState_->setText(QStringLiteral("登录响应不完整，请重试。")); return;
                }
                try { api_->setAuthentication(token, user); }
                catch (const std::exception&) { authState_->setText(QStringLiteral("登录响应无效，请重试。")); return; }
                nickname_->clear(); uiError_.clear();
                if (!closing_) { restoreRecovery(); pages_->setCurrentIndex(0); loadRoom(); }
            });
    }
    bool authenticationLost(const platform::ApiResult& result) {
        if (result.httpStatus != 401) return false;
        api_->clearAuthentication(); cancelRequested_ = true; pendingLive_.reset();
        session_.requestStop(); needsLogin_ = true;
        uiError_ = QStringLiteral("登录已失效，本地媒体已停止，请重新登录。");
        setPlatformState(uiError_); return true;
    }
    void loadRoom() {
        if (roomBusy_ || closing_ || !api_->authenticated()) return;
        roomBusy_ = true; setPlatformState(QStringLiteral("正在读取房间…"));
        api_->request(this, "GET", QStringLiteral("/api/v1/me/room"), {}, true, [this](platform::ApiResult result) {
            roomBusy_ = false;
            if (authenticationLost(result) || closing_) return;
            if (!result.ok()) { setPlatformState(result.message + QStringLiteral("，可点击刷新状态重试。")); return; }
            if (!validOwnRoom(result.data, roomId_)) {
                setPlatformState(QStringLiteral("房间响应不完整，请刷新重试。")); return;
            }
            roomId_ = result.data.value(QStringLiteral("room_id")).toString();
            if (!validRoomId(roomId_)) { roomId_.clear(); setPlatformState(QStringLiteral("房间响应不完整，请刷新重试。")); return; }
            roomTitle_->setText(result.data.value(QStringLiteral("title")).toString());
            category_->setCurrentIndex(std::max(0, category_->findData(result.data.value(QStringLiteral("category")).toString())));
            account_->setText(api_->user().value(QStringLiteral("nickname")).toString() + QStringLiteral(" · 房间 ") + roomId_);
            observedSessionId_ = result.data.value(QStringLiteral("current_session_id")).toString();
            if (!observedSessionId_.isEmpty() && !validSessionId(observedSessionId_)) {
                observedSessionId_.clear(); roomId_.clear(); setPlatformState(QStringLiteral("房间响应不完整，请刷新重试。")); return;
            }
            platformStatus_ = result.data.value(QStringLiteral("status")).toString();
            roomLoaded_ = true;
            setPlatformState(platformStatusText(platformStatus_));
            if (result.meta.value(QStringLiteral("media_service_status")).toString() != QStringLiteral("AVAILABLE"))
                setPlatformState(QStringLiteral("直播状态暂不可确认，可刷新重试。"));
            refreshPlatformState();
        });
    }
    void saveRoom(std::function<void(bool)> complete) {
        if (roomBusy_ || closing_ || !api_->authenticated() || roomId_.isEmpty()) {
            if (complete) complete(false); return;
        }
        const QString title = roomTitle_->text();
        if (title.trimmed().isEmpty() || title.toUcs4().size() > 120) {
            setPlatformState(QStringLiteral("标题需要 1–120 个字符。")); if (complete) complete(false); return;
        }
        roomBusy_ = true; setPlatformState(QStringLiteral("正在保存房间…"));
        api_->request(this, "PATCH", QStringLiteral("/api/v1/me/room"),
            {{QStringLiteral("title"), title}, {QStringLiteral("category"), category_->currentData().toString()}}, true,
            [this, complete = std::move(complete)](platform::ApiResult result) {
                roomBusy_ = false;
                const bool success = !authenticationLost(result) && result.ok() && validOwnRoom(result.data, roomId_);
                if (!closing_) setPlatformState(success ? QStringLiteral("房间资料已保存。")
                    : (result.ok() ? QStringLiteral("房间响应不完整，请刷新重试。") : result.message));
                if (complete) complete(success);
            });
    }
    void startPlatformLive() {
        if (closing_ || needsLogin_ || platformBusy() || !observedSessionId_.isEmpty() ||
            logoutRequested_ || !api_->authenticated() || !roomLoaded_ || roomId_.isEmpty()) return;
        if (canVerifyCancelledRequest()) { verifyCancelledRequest(); return; }
        if (api_->publishRecovery().stopRequested) return;
        try {
            auto config = formConfig(); config::validate(config);
            pendingPlatformConfig_ = std::move(config);
        } catch (const std::exception& problem) { showUiError(problem); return; }
        cancelRequested_ = false; uiError_.clear(); stopPostAttempts_ = 0; needsStopConfirmation_ = false;
        // 响应丢失后再次点击仍使用同一个 UUID，不创建并行会话。
        if (requestId_.isEmpty()) requestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        saveRoom([this](bool success) {
            if (!success || closing_ || cancelRequested_) {
                pendingPlatformConfig_.reset();
                if (!uncertainApply_ && ownedSessionId_.isEmpty() &&
                    api_->publishRecovery().clientRequestId != requestId_) requestId_.clear();
                return;
            }
            applying_ = true; uncertainApply_ = true; setPlatformState(QStringLiteral("正在申请开播…"));
            rememberAttempt();
            const QString requestId = requestId_;
            api_->request(this, "POST", QStringLiteral("/api/v1/me/room/live-sessions"),
                {{QStringLiteral("client_request_id"), requestId}}, true,
                [this, requestId](platform::ApiResult result) { finishApply(requestId, std::move(result)); });
        });
    }
    void finishApply(const QString& requestId, platform::ApiResult result) {
        applying_ = false;
        if (authenticationLost(result)) { pendingPlatformConfig_.reset(); return; }
        const QString id = result.data.value(QStringLiteral("live_session_id")).toString();
        const QString returnedRequestId = result.data.value(QStringLiteral("client_request_id")).toString();
        if (!result.ok()) {
            pendingPlatformConfig_.reset();
            if (result.httpStatus == 409 && matchesSession(result.data, id, roomId_) && !returnedRequestId.isEmpty()) {
                observedSessionId_ = id; platformStatus_ = result.data.value(QStringLiteral("status")).toString();
                uncertainApply_ = false;
                const bool same = returnedRequestId == requestId;
                if (same) {
                    ownedSessionId_ = id;
                    api_->setPublishRecovery({roomId_, requestId, id, closing_ || cancelRequested_});
                } else {
                    const auto record = api_->publishRecovery();
                    if (record.clientRequestId == requestId) clearRecovery(record);
                }
                setPlatformState(same ? QStringLiteral("本次申请已创建会话，但推流凭证无法再次取得。请明确结束该会话后重新开播。")
                    : QStringLiteral("房间已有其他客户端的会话。可刷新状态，或明确结束该会话。"));
                if (terminal(platformStatus_)) { clearTerminalSession(); }
                else if ((closing_ || cancelRequested_) && same) {
                    ownedSessionId_ = id; requestPlatformStop();
                }
            } else {
                // 明确的参数/限频拒绝不保留一次新开播操作；网络及服务错误保留 UUID。
                if (result.httpStatus >= 400 && result.httpStatus < 500 && result.httpStatus != 409) {
                    const auto record = api_->publishRecovery();
                    if (record.clientRequestId == requestId) clearRecovery(record);
                    uncertainApply_ = false; requestId_.clear();
                }
                setPlatformState(result.message + QStringLiteral("；可重试申请。"));
            }
            if ((closing_ || cancelRequested_) && uncertainApply_) recoverOwnPreparation();
            return;
        }
        // 在任何本地启动或自动撤销前，先确认响应对应本次房间、请求与新建准备会话。
        if (result.httpStatus != 201 || requestId_ != requestId || returnedRequestId != requestId ||
            !matchesSession(result.data, id, roomId_) ||
            result.data.value(QStringLiteral("status")).toString() != QStringLiteral("PREPARING")) {
            pendingPlatformConfig_.reset(); uncertainApply_ = true;
            setPlatformState(QStringLiteral("开播响应不完整，正在核实本次准备会话。"));
            // 不信任不完整响应中的会话 ID，通过个人房间和会话查询核对后再回收。
            auto record = api_->publishRecovery(); record.stopRequested = true; api_->setPublishRecovery(record);
            recoverOwnPreparation(); return;
        }
        uncertainApply_ = false; ownedSessionId_ = id; observedSessionId_ = id;
        platformStatus_ = QStringLiteral("PREPARING");
        rememberAttempt();
        if (closing_ || cancelRequested_) { pendingPlatformConfig_.reset(); requestPlatformStop(); return; }
        const QUrl publishUrl(result.data.value(QStringLiteral("publish_url")).toString());
        if (!publishUrl.isValid() || publishUrl.host().isEmpty() ||
            (publishUrl.scheme() != QStringLiteral("rtmp") && publishUrl.scheme() != QStringLiteral("rtmps")) ||
            !publishUrl.userInfo().isEmpty() || !pendingPlatformConfig_) {
            pendingPlatformConfig_.reset(); setPlatformState(QStringLiteral("推流地址响应无效，正在取消准备会话。"));
            requestPlatformStop(); return;
        }
        auto config = std::move(*pendingPlatformConfig_); pendingPlatformConfig_.reset();
        config.outputUrl = bytes(publishUrl.toString(QUrl::FullyEncoded));
        try {
            config::validate(config); mediaOwned_ = true; cleanupRequested_ = false;
            if (active(session_.snapshot().state)) { pendingLive_ = std::move(config); session_.requestStop(); }
            else { previewRequested_ = false; clearPreview(); if (!session_.start(config)) requestPlatformStop(); }
            setPlatformState(platformStatusText(platformStatus_));
        } catch (const std::exception& problem) { showUiError(problem); requestPlatformStop(); }
    }
    void recoverOwnPreparation() {
        if (recoveryBusy_ || applying_ || stopBusy_ || !api_->authenticated()) return;
        const auto record = api_->publishRecovery();
        if (!validRoomId(record.roomId) || record.roomId != roomId_ ||
            (!validSessionId(record.liveSessionId) && !validRequestId(record.clientRequestId))) return;
        recoveryBusy_ = true;
        if (validSessionId(record.liveSessionId)) { inspectRecoverySession(record, record.liveSessionId); return; }
        api_->request(this, "GET", QStringLiteral("/api/v1/me/room"), {}, true,
            [this, record](platform::ApiResult result) {
                if (authenticationLost(result) || !result.ok() || !sameRecovery(api_->publishRecovery(), record) ||
                    !validOwnRoom(result.data, record.roomId)) { recoveryUnconfirmed(); return; }
                const QString id = result.data.value(QStringLiteral("current_session_id")).toString();
                if (!validSessionId(id)) {
                    // 一次空查询不能证明先前超时的事务以后不会提交。保留 UUID，可安全重试同一次申请。
                    recoveryBusy_ = false;
                    platformStatus_ = QStringLiteral("OFFLINE");
                    setPlatformState(record.stopRequested
                        ? QStringLiteral("上次申请的结束结果仍未确认。可点击“核实并结束上次申请”，仅复用原请求编号核实并取消。")
                        : QStringLiteral("未发现当前直播；可以重试申请，客户端将继续复用未确认的请求编号。")); return;
                }
                inspectRecoverySession(record, id);
            });
    }
    void inspectRecoverySession(platform::PublishRecovery record, const QString& id) {
        api_->request(this, "GET", QStringLiteral("/api/v1/live-sessions/") + id, {}, true,
            [this, record, id](platform::ApiResult result) mutable {
                recoveryBusy_ = false;
                if (result.httpStatus == 404 && result.code == QStringLiteral("LIVE_SESSION_NOT_FOUND") &&
                    sameRecovery(api_->publishRecovery(), record)) {
                    clearRecovery(record);
                    if (observedSessionId_ == id || ownedSessionId_ == id) clearTerminalSession();
                    setPlatformState(QStringLiteral("服务器已无此直播会话，可以重新开播。")); return;
                }
                if (authenticationLost(result) || !result.ok() || !sameRecovery(api_->publishRecovery(), record) ||
                    !matchesSession(result.data, id, record.roomId)) { recoveryUnconfirmed(); return; }
                const QString actualRequest = result.data.value(QStringLiteral("client_request_id")).toString();
                if (!record.clientRequestId.isEmpty() && actualRequest != record.clientRequestId) {
                    // 同一房间只有一个有效会话。另一请求占用房间时，绝不能把恢复意图转给它。
                    if (record.liveSessionId.isEmpty()) {
                        clearRecovery(record);
                        if (requestId_ == record.clientRequestId) { requestId_.clear(); uncertainApply_ = false; }
                    }
                    setPlatformState(QStringLiteral("房间当前会话属于另一次开播，已保留该会话；请刷新状态。")); return;
                }
                const QString status = result.data.value(QStringLiteral("status")).toString();
                if (terminal(status)) {
                    clearRecovery(record);
                    if (observedSessionId_ == id || ownedSessionId_ == id) clearTerminalSession();
                    else if (requestId_ == record.clientRequestId) { requestId_.clear(); uncertainApply_ = false; }
                    setPlatformState(platformStatusText(status)); return;
                }
                record.liveSessionId = id;
                if (!actualRequest.isEmpty()) record.clientRequestId = actualRequest;
                api_->setPublishRecovery(record);
                // 只有查询确认属于本次请求的会话才恢复本进程的回收责任。
                if (observedSessionId_.isEmpty() || observedSessionId_ == id) {
                    observedSessionId_ = id; ownedSessionId_ = id; platformStatus_ = status; uncertainApply_ = false;
                }
                if (!record.stopRequested) {
                    setPlatformState(QStringLiteral("本次申请已建立会话，推流凭证无法再次取得；请明确结束该会话后重新开播。")); return;
                }
                if (status == QStringLiteral("STOPPING")) { setPlatformState(platformStatusText(status)); return; }
                if (!result.data.value(QStringLiteral("allow_publish")).isBool() ||
                    !result.data.value(QStringLiteral("allow_publish")).toBool()) {
                    recoveryUnconfirmed(); return;
                }
                // 每次重发下播前先确认确切的会话仍允许发布；最多三次，之后保留人工重试入口。
                sendPlatformStop(record);
            });
    }
    void recoveryUnconfirmed() {
        recoveryBusy_ = false;
        if (!closing_ && !needsLogin_) setPlatformState(QStringLiteral("未能确认本次准备会话，可刷新状态或使用同一次申请重试。"));
    }
    void requestPlatformStop(bool explicitExisting = false) {
        pendingLive_.reset(); pendingPlatformConfig_.reset(); session_.requestStop();
        if (!api_->authenticated()) return;
        const QString id = !ownedSessionId_.isEmpty() ? ownedSessionId_ : (explicitExisting ? observedSessionId_ : QString{});
        auto record = api_->publishRecovery();
        if (!id.isEmpty()) {
            if (record.liveSessionId != id) {
                record = {roomId_, id == ownedSessionId_ ? requestId_ : QString{}, id, true};
            } else record.stopRequested = true;
            api_->setPublishRecovery(record);
        } else if (record.clientRequestId == requestId_ && !requestId_.isEmpty()) {
            record.stopRequested = true; api_->setPublishRecovery(record);
        }
        if (stopBusy_ || applying_ || recoveryBusy_) return;
        ++sessionEpoch_;
        if (id.isEmpty()) {
            if (record.stopRequested && (uncertainApply_ || validRequestId(record.clientRequestId))) recoverOwnPreparation();
            return;
        }
        if (needsStopConfirmation_) { recoverOwnPreparation(); return; }
        sendPlatformStop(record);
    }
    void sendPlatformStop(const platform::PublishRecovery& record) {
        if (stopBusy_ || !sameRecovery(api_->publishRecovery(), record) || !record.stopRequested) return;
        if (stopPostAttempts_ >= 3) {
            setPlatformState(QStringLiteral("本地已停止，下播确认仍未完成；可手动重试或稍后重新进入控制台。")); return;
        }
        const QString id = record.liveSessionId; const QString roomId = record.roomId;
        if (!validSessionId(id) || !validRoomId(roomId)) return;
        ++stopPostAttempts_; needsStopConfirmation_ = true;
        ++sessionEpoch_; const auto epoch = sessionEpoch_;
        stopBusy_ = true; setPlatformState(QStringLiteral("本地已请求停止，正在申请下播…"));
        api_->request(this, "POST", QStringLiteral("/api/v1/live-sessions/") + id + QStringLiteral("/stop"), {}, true,
            [this, id, roomId, epoch, record](platform::ApiResult result) {
                stopBusy_ = false;
                if (authenticationLost(result)) return;
                if (result.httpStatus == 404 && result.code == QStringLiteral("LIVE_SESSION_NOT_FOUND") &&
                    sameRecovery(api_->publishRecovery(), record)) {
                    clearRecovery(record);
                    if (observedSessionId_ == id || ownedSessionId_ == id) clearTerminalSession();
                    setPlatformState(QStringLiteral("服务器已无此直播会话，本地已停止。")); return;
                }
                if (!result.ok()) { setPlatformState(QStringLiteral("本地已停止，下播请求未确认，可点击停止直播重试。")); return; }
                if (roomId_ != roomId || epoch != sessionEpoch_ || !sameRecovery(api_->publishRecovery(), record)) return;
                if (!matchesSession(result.data, id, roomId) ||
                    (result.data.value(QStringLiteral("status")).toString() != QStringLiteral("STOPPING") &&
                     !terminal(result.data.value(QStringLiteral("status")).toString()))) {
                    setPlatformState(QStringLiteral("本地已停止，下播响应不完整，请刷新状态或重试下播。")); return;
                }
                platformStatus_ = result.data.value(QStringLiteral("status")).toString();
                setPlatformState(platformStatusText(platformStatus_));
                if (terminal(platformStatus_)) {
                    clearRecovery(record);
                    if (observedSessionId_ == id || ownedSessionId_ == id) clearTerminalSession();
                }
            });
    }
    void clearTerminalSession() {
        const auto record = api_->publishRecovery();
        if (validSessionId(record.liveSessionId) &&
            (record.liveSessionId == observedSessionId_ || record.liveSessionId == ownedSessionId_))
            clearRecovery(record);
        session_.requestStop(); pendingLive_.reset(); pendingPlatformConfig_.reset();
        ownedSessionId_.clear(); observedSessionId_.clear(); requestId_.clear();
        mediaOwned_ = false; uncertainApply_ = false; needsStopConfirmation_ = false; stopPostAttempts_ = 0;
    }
    void queryPlatformSession() {
        if (sessionQueryBusy_ || platformBusy() || closing_ || !api_->authenticated() || observedSessionId_.isEmpty()) return;
        const QString id = observedSessionId_; const QString roomId = roomId_;
        const auto epoch = sessionEpoch_; sessionQueryBusy_ = true;
        api_->request(this, "GET", QStringLiteral("/api/v1/live-sessions/") + id, {}, true,
            [this, id, roomId, epoch](platform::ApiResult result) {
                sessionQueryBusy_ = false;
                if (authenticationLost(result) || closing_ || observedSessionId_ != id || roomId_ != roomId || epoch != sessionEpoch_) return;
                if (result.httpStatus == 404 && result.code == QStringLiteral("LIVE_SESSION_NOT_FOUND")) {
                    clearTerminalSession(); setPlatformState(QStringLiteral("服务器已无此直播会话，本地已停止，可以重新开播。")); return;
                }
                if (!result.ok()) { setPlatformState(QStringLiteral("平台状态查询失败，本地媒体状态见预览区，可刷新重试。")); return; }
                if (!matchesSession(result.data, id, roomId)) {
                    setPlatformState(QStringLiteral("会话状态响应不完整，保持本地媒体状态，可刷新重试。")); return;
                }
                platformStatus_ = result.data.value(QStringLiteral("status")).toString();
                setPlatformState(platformStatusText(platformStatus_));
                if (platformStatus_ == QStringLiteral("STOPPING") || terminal(platformStatus_)) {
                    pendingLive_.reset(); session_.requestStop();
                    if (terminal(platformStatus_)) clearTerminalSession();
                } else if (result.meta.value(QStringLiteral("media_service_status")).toString() != QStringLiteral("AVAILABLE")) {
                    setPlatformState(QStringLiteral("直播状态暂不可确认；本地媒体继续运行。"));
                }
            });
    }
    void beginLogout() {
        if (platformBusy() || closing_) return;
        if (!observedSessionId_.isEmpty() || active(session_.snapshot().state)) {
            setPlatformState(QStringLiteral("请先停止本地预览和当前直播，再退出登录。")); return;
        }
        const auto record = api_->publishRecovery();
        if (!record.clientRequestId.isEmpty() || !record.liveSessionId.isEmpty()) {
            logoutRequested_ = true; cancelRequested_ = true;
            requestPlatformStop(); return;
        }
        finishLogout();
    }
    void finishLogout() {
        logoutRequested_ = false;
        logoutBusy_ = true;
        api_->request(this, "POST", QStringLiteral("/api/v1/auth/logout"), {}, true,
            [this](platform::ApiResult result) {
                logoutBusy_ = false;
                if (result.code == QStringLiteral("AUTHENTICATION_CHANGED")) return;
                // 即使服务不可用也允许退出本地登录；明确提示服务端撤销尚未确认。
                api_->clearAuthentication(); needsLogin_ = true;
                authState_->setText(result.ok() || result.httpStatus == 401 ? QStringLiteral("已退出登录。")
                    : QStringLiteral("已退出本地登录，服务端令牌撤销未确认。"));
            });
    }
    QWidget* buildSidebar() {
        auto* side = panel("sidebar"); side->setFixedWidth(188);
        auto* layout = new QVBoxLayout(side);
        layout->setContentsMargins(16, 25, 16, 20); layout->setSpacing(7);
        auto* brandRow = new QHBoxLayout; brandRow->setSpacing(9);
        auto* mark = new QLabel; mark->setPixmap(icon(Glyph::Studio).pixmap(32, 32));
        brandRow->addWidget(mark, 0, Qt::AlignTop);
        auto* brandText = new QVBoxLayout; brandText->setSpacing(4);
        brandText->addWidget(label(QStringLiteral("LOCAL LIVE"), "brandName"));
        brandText->addWidget(label(QStringLiteral("STUDIO"), "brandSub"));
        brandRow->addLayout(brandText); layout->addLayout(brandRow); layout->addSpacing(32);
        layout->addWidget(label(QStringLiteral("工作空间"), "navCaption")); layout->addSpacing(7);
        const QStringList names{QStringLiteral("直播间"), QStringLiteral("采集设备"), QStringLiteral("推流设置"), QStringLiteral("运行日志")};
        const Glyph glyphs[]{Glyph::Dashboard, Glyph::Camera, Glyph::Sliders, Glyph::Log};
        for (int i = 0; i < names.size(); ++i) {
            auto* button = new QPushButton(icon(glyphs[i]), names[i]);
            button->setObjectName(QStringLiteral("navButton")); button->setIconSize({18, 18});
            button->setCursor(Qt::PointingHandCursor); button->setProperty("active", i == 0);
            navigation_.push_back(button); layout->addWidget(button);
        }
        layout->addStretch();
        auto* home = new QPushButton(QStringLiteral("返回直播大厅"));
        home->setObjectName(QStringLiteral("navButton")); home->setCursor(Qt::PointingHandCursor);
        home->setToolTip(QStringLiteral("停止当前采集或直播后返回直播大厅"));
        connect(home, &QPushButton::clicked, this, &QWidget::close); layout->addWidget(home);
        layout->addSpacing(12); sidebarStatus_ = label(QStringLiteral("●  本机控制台 · 待机"), "subtle");
        layout->addWidget(sidebarStatus_); layout->addSpacing(6);
        layout->addWidget(label(QStringLiteral("设备采集  →  RTMP 推流"), "subtle"));
        return side;
    }
    QWidget* buildStudio() {
        auto* studio = new QWidget; auto* layout = new QVBoxLayout(studio);
        layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(8);
        auto* header = new QHBoxLayout; auto* title = new QVBoxLayout; title->setSpacing(4);
        title->addWidget(label(QStringLiteral("直播间"), "pageTitle"));
        title->addWidget(label(QStringLiteral("管理采集画面与直播推流"), "subtle"));
        header->addLayout(title); header->addStretch();
        start_ = new QPushButton(icon(Glyph::Play, QColor(17, 51, 41)), QStringLiteral("开始直播"));
        start_->setObjectName(QStringLiteral("startButton")); start_->setCursor(Qt::PointingHandCursor);
        stop_ = new QPushButton(icon(Glyph::Stop, QColor(192, 137, 132)), QStringLiteral("停止直播"));
        stop_->setObjectName(QStringLiteral("stopButton")); stop_->setCursor(Qt::PointingHandCursor);
        header->addWidget(start_); header->addWidget(stop_); layout->addLayout(header);
        banner_ = label({}, "errorBanner"); banner_->setWordWrap(true); banner_->hide(); layout->addWidget(banner_);
        auto* previewHeader = new QHBoxLayout;
        previewHeader->addWidget(label(QStringLiteral("实时预览"), "sectionTitle")); previewHeader->addStretch();
        previewHeader->addWidget(label(QStringLiteral("16:9  ·  等比显示"), "subtle"));
        previewButton_ = new QPushButton(icon(Glyph::Camera), QStringLiteral("开始预览"));
        previewButton_->setObjectName(QStringLiteral("smallButton")); previewHeader->addWidget(previewButton_);
        layout->addLayout(previewHeader); preview_ = new PreviewWidget; layout->addWidget(preview_, 1);
        auto* metrics = new QHBoxLayout; metrics->setSpacing(10);
        bitrate_ = new MetricCard(QStringLiteral("推流码率"), QStringLiteral("实际音视频 payload"));
        fps_ = new MetricCard(QStringLiteral("视频帧率"), QStringLiteral("实际编码帧率"));
        duration_ = new MetricCard(QStringLiteral("直播时长"), QStringLiteral("首次媒体发送起计时"));
        connection_ = new MetricCard(QStringLiteral("连接状态"), QStringLiteral("RTMP 推流连接"));
        for (auto* metric : {bitrate_, fps_, duration_, connection_}) metrics->addWidget(metric, 1);
        layout->addLayout(metrics);
        auto* addressCard = panel(); auto* addressLayout = new QVBoxLayout(addressCard);
        addressLayout->setContentsMargins(12, 8, 12, 8); addressLayout->setSpacing(3);
        auto* addressTitle = new QHBoxLayout; addressTitle->addWidget(label(QStringLiteral("推流地址"), "sectionTitle"));
        addressTitle->addStretch(); copyAddress_ = new QPushButton(icon(Glyph::Copy), QStringLiteral("复制地址"));
        copyAddress_->setObjectName(QStringLiteral("linkButton")); addressTitle->addWidget(copyAddress_);
        addressLayout->addLayout(addressTitle); addressDisplay_ = new QLineEdit;
        addressDisplay_->setObjectName(QStringLiteral("addressDisplay")); addressDisplay_->setReadOnly(true);
        addressLayout->addWidget(addressDisplay_); layout->addWidget(addressCard);
        if (api_) addressCard->hide();
        auto* logs = panel(); auto* logLayout = new QVBoxLayout(logs);
        logLayout->setContentsMargins(12, 7, 12, 7); logLayout->setSpacing(2);
        auto* logTitle = new QHBoxLayout; logTitle->addWidget(label(QStringLiteral("运行日志"), "sectionTitle"));
        logTitle->addWidget(label(QStringLiteral("最近 1000 行"), "subtle")); logTitle->addStretch();
        clearLogs_ = new QPushButton(QStringLiteral("清空")); clearLogs_->setObjectName(QStringLiteral("linkButton"));
        logTitle->addWidget(clearLogs_); logLayout->addLayout(logTitle);
        log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->setMaximumBlockCount(1000);
        log_->setLineWrapMode(QPlainTextEdit::NoWrap); log_->setFixedHeight(52);
        logs->setFixedHeight(88);
        logLayout->addWidget(log_); layout->addWidget(logs); return studio;
    }
    QWidget* buildSettings() {
        auto* outer = panel("settingsPanel"); outer->setFixedWidth(320);
        auto* outerLayout = new QVBoxLayout(outer); outerLayout->setContentsMargins(0, 0, 0, 0);
        settingsScroll_ = new QScrollArea; settingsScroll_->setWidgetResizable(true);
        settingsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget; settingsContent_ = content; auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(18, 20, 18, 20); layout->setSpacing(15);
        if (api_) layout->addWidget(buildRoomSettings());
        captureSection_ = new QWidget; auto* captureLayout = new QVBoxLayout(captureSection_);
        captureLayout->setContentsMargins(0, 0, 0, 0); captureLayout->setSpacing(10);
        captureLayout->addWidget(label(QStringLiteral("采集设备"), "sectionTitle"));
        captureLayout->addWidget(label(QStringLiteral("开播前选择音视频来源"), "subtle"));
        auto* devicesCard = panel("settingsCard"); auto* devices = new QVBoxLayout(devicesCard);
        devices->setContentsMargins(13, 14, 13, 14); devices->setSpacing(8);
        sectionLabel(devices, QStringLiteral("视频源")); videoSource_ = new QComboBox;
        videoSource_->addItem(QStringLiteral("桌面采集"), QStringLiteral("desktop"));
        videoSource_->addItem(QStringLiteral("摄像头"), QStringLiteral("camera"));
        videoSource_->setCurrentIndex(initial_.capture.videoSource == "camera" ? 1 : 0); devices->addWidget(videoSource_);
        cameraLabel_ = label(QStringLiteral("摄像头设备 / 设备 ID"), "fieldLabel"); devices->addWidget(cameraLabel_);
        camera_ = new QComboBox; camera_->setEditable(true); camera_->setMinimumContentsLength(12);
        camera_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); devices->addWidget(camera_);
        videoDescription_ = label(QStringLiteral("采集默认显示输出，包含鼠标光标"), "subtle");
        videoDescription_->setWordWrap(true); devices->addWidget(videoDescription_); devices->addSpacing(7);
        sectionLabel(devices, QStringLiteral("音频源")); audioSource_ = new QComboBox;
        // 类型和具体设备在同一列表选择，避免可编辑设备框遮挡下拉入口。
        audioSource_->setMinimumContentsLength(12);
        audioSource_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); devices->addWidget(audioSource_);
        audioDescription_ = label({}, "subtle"); audioDescription_->setWordWrap(true); devices->addWidget(audioDescription_);
        refresh_ = new QPushButton(icon(Glyph::Refresh), QStringLiteral("刷新设备列表"));
        refresh_->setObjectName(QStringLiteral("smallButton")); devices->addWidget(refresh_);
        deviceError_ = label({}, "subtle"); deviceError_->setWordWrap(true); deviceError_->hide(); devices->addWidget(deviceError_);
        captureLayout->addWidget(devicesCard); layout->addWidget(captureSection_);
        outputSection_ = new QWidget; auto* outputLayout = new QVBoxLayout(outputSection_);
        outputLayout->setContentsMargins(0, 0, 0, 0); outputLayout->setSpacing(10);
        outputLayout->addWidget(label(QStringLiteral("输出设置"), "sectionTitle"));
        auto* outputCard = panel("settingsCard"); auto* output = new QVBoxLayout(outputCard);
        output->setContentsMargins(13, 14, 13, 14); output->setSpacing(8);
        if (!api_) sectionLabel(output, QStringLiteral("RTMP / RTMPS 地址"));
        url_ = new QLineEdit(api_ ? QString{} : utf8(initial_.outputUrl));
        url_->setPlaceholderText(QStringLiteral("rtmp://127.0.0.1/live/livestream")); output->addWidget(url_);
        if (api_) url_->hide();
        sectionLabel(output, QStringLiteral("输出分辨率")); resolution_ = new QComboBox;
        const QSize initialSize(initial_.video.width, initial_.video.height);
        for (const auto& size : {initialSize, QSize(1920, 1080), QSize(1280, 720), QSize(960, 540), QSize(640, 360)})
            if (resolution_->findData(size) < 0) resolution_->addItem(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()), size);
        output->addWidget(resolution_);
        auto* row = new QHBoxLayout; row->setSpacing(10);
        auto* fpsColumn = new QVBoxLayout; fpsColumn->setSpacing(8); sectionLabel(fpsColumn, QStringLiteral("目标帧率"));
        targetFps_ = new QSpinBox; targetFps_->setRange(1, 120); targetFps_->setSuffix(QStringLiteral(" FPS"));
        targetFps_->setValue(initial_.video.fps); fpsColumn->addWidget(targetFps_);
        auto* bitrateColumn = new QVBoxLayout; bitrateColumn->setSpacing(8); sectionLabel(bitrateColumn, QStringLiteral("视频目标码率"));
        targetBitrate_ = new QSpinBox; targetBitrate_->setRange(64, 100000); targetBitrate_->setSingleStep(100);
        targetBitrate_->setSuffix(QStringLiteral(" kbps")); targetBitrate_->setValue(initial_.video.bitrate / 1000);
        bitrateColumn->addWidget(targetBitrate_); row->addLayout(fpsColumn, 1); row->addLayout(bitrateColumn, 1); output->addLayout(row);
        sectionLabel(output, QStringLiteral("视频编码方式"));
        encodingBackend_ = new QComboBox;
        encodingBackend_->setAccessibleName(QStringLiteral("视频编码方式"));
        encodingBackend_->addItem(QStringLiteral("软件编码（CPU）"), QStringLiteral("software"));
        encodingBackend_->addItem(QStringLiteral("硬件编码（NVIDIA NVENC）"), QStringLiteral("nvenc"));
        encodingBackend_->addItem(QStringLiteral("自动（优先 NVENC）"), QStringLiteral("auto"));
        encodingBackend_->setCurrentIndex(std::max(0, encodingBackend_->findData(utf8(initial_.videoEncoder.backend))));
        encodingBackend_->setToolTip(QStringLiteral("硬件编码需要可用的 NVIDIA NVENC；自动模式在硬件编码初始化失败时改用软件编码。"));
        output->addWidget(encodingBackend_);
        output->addSpacing(2); lowLatency_ = new QCheckBox(QStringLiteral("低延迟编码"));
        lowLatency_->setChecked(initial_.videoEncoder.lowLatency); output->addWidget(lowLatency_);
        auto* encoding = label(QStringLiteral("H.264 视频  ·  AAC 音频\n仅直播推流，不生成本地录像文件"), "subtle");
        encoding->setWordWrap(true); output->addWidget(encoding); outputLayout->addWidget(outputCard);
        layout->addWidget(outputSection_);
        auto* statsSection = new QWidget; auto* statsLayout = new QVBoxLayout(statsSection);
        statsLayout->setContentsMargins(0, 0, 0, 0); statsLayout->setSpacing(9);
        statsLayout->addWidget(label(QStringLiteral("性能统计"), "sectionTitle"));
        auto* statsCard = panel("settingsCard"); auto* stats = new QVBoxLayout(statsCard);
        stats->setContentsMargins(13, 12, 13, 12); stats->setSpacing(7);
        statsSample_ = label(QStringLiteral("等待首个统计样本"), "subtle"); stats->addWidget(statsSample_);
        const auto addStat = [stats](const QString& title, QLabel*& value) {
            auto* row = new QHBoxLayout; row->setSpacing(6);
            row->addWidget(label(title, "fieldLabel")); row->addStretch();
            value = label(QStringLiteral("—"), "statsValue"); row->addWidget(value); stats->addLayout(row);
        };
        addStat(QStringLiteral("视频采集"), captureRate_);
        addStat(QStringLiteral("视频编码"), encodeRate_);
        addStat(QStringLiteral("成功发送 payload"), sendRate_);
        addStat(QStringLiteral("原始视频队列"), rawVideoQueue_);
        addStat(QStringLiteral("原始音频队列"), rawAudioQueue_);
        addStat(QStringLiteral("封装输出队列"), outputQueue_);
        addStat(QStringLiteral("网络包合计"), networkPacketQueue_);
        addStat(QStringLiteral("网络视频队列"), networkVideoQueue_);
        addStat(QStringLiteral("网络音频队列"), networkAudioQueue_);
        addStat(QStringLiteral("累计视频丢帧"), droppedFrames_);
        addStat(QStringLiteral("累计网络丢包"), droppedPackets_);
        addStat(QStringLiteral("中止丢弃输出事件"), abortedEvents_);
        statsLayout->addWidget(statsCard); layout->addWidget(statsSection);
        layout->addStretch(); settingsScroll_->setWidget(content); outerLayout->addWidget(settingsScroll_);
#ifndef _WIN32
        if (auto* model = qobject_cast<QStandardItemModel*>(videoSource_->model())) model->item(0)->setEnabled(false);
        videoSource_->setCurrentIndex(1);
#endif
        return outer;
    }
    QWidget* buildFooter() {
        auto* footer = panel("footer"); footer->setFixedHeight(32); auto* layout = new QHBoxLayout(footer);
        layout->setContentsMargins(22, 0, 20, 0); layout->setSpacing(20);
        footerState_ = label(QStringLiteral("●  待机"), "subtle"); footerSources_ = label({}, "subtle"); footerOutput_ = label({}, "subtle");
        layout->addWidget(footerState_); layout->addWidget(footerSources_); layout->addStretch(); layout->addWidget(footerOutput_); return footer;
    }
    void connectControls() {
        connect(videoSource_, &QComboBox::currentIndexChanged, this, [this] { updateSourceDetails(); });
        connect(audioSource_, &QComboBox::currentIndexChanged, this, [this] { updateSourceDetails(); });
        connect(resolution_, &QComboBox::currentIndexChanged, this, [this] { updateOutputSummary(); });
        connect(targetFps_, &QSpinBox::valueChanged, this, [this] { updateOutputSummary(); });
        connect(url_, &QLineEdit::textChanged, this, [this] { addressDisplay_->setText(url_->text()); addressDisplay_->setCursorPosition(0); });
        connect(refresh_, &QPushButton::clicked, this, [this] { refreshDevices(); });
        connect(start_, &QPushButton::clicked, this, [this] { startLive(); });
        connect(stop_, &QPushButton::clicked, this, [this] {
            if (api_) {
                const auto recorded = api_->publishRecovery();
                if (ownedSessionId_.isEmpty() && !observedSessionId_.isEmpty() &&
                    !(recorded.stopRequested && recorded.liveSessionId == observedSessionId_) &&
                    QMessageBox::question(this, QStringLiteral("结束已有直播"),
                        QStringLiteral("该会话可能由其他客户端使用。确定结束此直播会话？")) != QMessageBox::Yes) return;
                cancelRequested_ = true; pendingLive_.reset(); session_.requestStop();
                stopPostAttempts_ = 0;
                requestPlatformStop(true);
            } else session_.requestStop();
            pollSafely();
        });
        connect(previewButton_, &QPushButton::clicked, this, [this] { togglePreview(); });
        connect(copyAddress_, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(addressDisplay_->text()); copyAddress_->setText(QStringLiteral("已复制"));
            QTimer::singleShot(1600, this, [this] { copyAddress_->setText(QStringLiteral("复制地址")); });
        });
        connect(clearLogs_, &QPushButton::clicked, log_, &QPlainTextEdit::clear);
        for (int i = 0; i < static_cast<int>(navigation_.size()); ++i) {
            connect(navigation_[static_cast<std::size_t>(i)], &QPushButton::clicked, this, [this, i] {
                for (std::size_t n = 0; n < navigation_.size(); ++n) {
                    auto* button = navigation_[n]; button->setProperty("active", static_cast<int>(n) == i);
                    button->style()->unpolish(button); button->style()->polish(button);
                }
                if (i == 1 || i == 2) {
                    auto* section = i == 1 ? captureSection_ : outputSection_;
                    settingsScroll_->verticalScrollBar()->setValue(section->mapTo(settingsContent_, QPoint(0, 0)).y() - 12);
                    if (i == 2) url_->setFocus();
                } else if (i == 3) {
                    log_->setFocus(); log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
                } else settingsScroll_->verticalScrollBar()->setValue(0);
            });
        }
    }
    std::string deviceValue(QComboBox* combo) const {
        if (combo->currentIndex() >= 0 && combo->currentText() == combo->itemText(combo->currentIndex()))
            return bytes(combo->currentData().toString());
        return bytes(combo->currentText());
    }
    void fillDevices(QComboBox* combo, capture::Kind kind, const std::string& selected, QStringList& errors) {
        combo->clear(); combo->addItem(QStringLiteral("默认设备"), QStringLiteral("default"));
        try {
            for (const auto& device : capture::listDevices(kind)) combo->addItem(utf8(device.description), utf8(device.id));
        } catch (const std::exception& problem) {
            const auto message = QStringLiteral("%1枚举失败：%2").arg(kind == capture::Kind::Video
                ? QStringLiteral("摄像头") : QStringLiteral("麦克风"), QString::fromUtf8(problem.what()));
            errors.push_back(message); logger_.log(logging::Level::Warn, "publisher_ui", bytes(message));
        }
        const int index = combo->findData(utf8(selected));
        if (index >= 0) combo->setCurrentIndex(index); else combo->setEditText(utf8(selected));
    }
    void refreshDevices() {
        const auto camera = camera_->count() ? deviceValue(camera_) : initial_.videoDevice;
        const auto audioSource = audioSource_->count() ? audioSource_->currentData().toString() : utf8(initial_.capture.audioSource);
        const auto audioDevice = audioSource == QStringLiteral("microphone")
            ? (audioSource_->count() ? audioSource_->currentData(audioDeviceRole).toString() : utf8(initial_.audioDevice))
            : QStringLiteral("default");
        QStringList errors; fillDevices(camera_, capture::Kind::Video, camera, errors);
        fillAudioSources(audioSource, audioDevice, errors);
        deviceError_->setText(errors.join(QLatin1Char('\n'))); deviceError_->setVisible(!errors.empty());
        updateSourceDetails();
    }
    void fillAudioSources(const QString& selectedSource, const QString& selectedDevice, QStringList& errors) {
        const QSignalBlocker blocker(audioSource_);
        audioSource_->clear();
        const auto addSource = [this](const QString& text, const QString& source, const QString& device) {
            audioSource_->addItem(text, source);
            const int index = audioSource_->count() - 1;
            audioSource_->setItemData(index, device, audioDeviceRole);
            audioSource_->setItemData(index, text + QLatin1Char('\n') + device, Qt::ToolTipRole);
        };
        addSource(QStringLiteral("麦克风：默认设备"), QStringLiteral("microphone"), QStringLiteral("default"));
#ifdef _WIN32
        addSource(QStringLiteral("系统声音：默认播放设备"), QStringLiteral("system"), QStringLiteral("default"));
#endif
        try {
            for (const auto& device : capture::listDevices(capture::Kind::Audio))
                addSource(QStringLiteral("麦克风：%1").arg(utf8(device.description)), QStringLiteral("microphone"), utf8(device.id));
        } catch (const std::exception& problem) {
            const auto message = QStringLiteral("麦克风枚举失败：%1").arg(QString::fromUtf8(problem.what()));
            errors.push_back(message); logger_.log(logging::Level::Warn, "publisher_ui", bytes(message));
        }
        int selectedIndex = -1;
        for (int i = 0; i < audioSource_->count(); ++i) {
            if (audioSource_->itemData(i).toString() == selectedSource &&
                audioSource_->itemData(i, audioDeviceRole).toString() == selectedDevice) {
                selectedIndex = i; break;
            }
        }
        if (selectedIndex < 0 && selectedSource == QStringLiteral("microphone") && !selectedDevice.isEmpty()) {
            // 保留配置指定或刷新前选中的设备，不静默切换到默认麦克风。
            addSource(QStringLiteral("麦克风：配置设备（未枚举到）"), selectedSource, selectedDevice);
            selectedIndex = audioSource_->count() - 1;
        }
        audioSource_->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    }
    void updateSourceDetails() {
        const bool camera = videoSource_->currentData().toString() == QStringLiteral("camera");
        cameraLabel_->setVisible(camera); camera_->setVisible(camera);
        videoDescription_->setText(camera ? QStringLiteral("可选择设备或直接输入名称 / ID")
            : QStringLiteral("采集默认显示输出，包含鼠标光标"));
        const bool microphone = audioSource_->currentData().toString() == QStringLiteral("microphone");
        audioDescription_->setText(microphone ? QStringLiteral("直接在上方选择麦克风设备，可刷新设备列表")
            : QStringLiteral("Windows 默认播放设备回采，不与麦克风混音"));
        audioSource_->setToolTip(audioSource_->currentData(Qt::ToolTipRole).toString());
        footerSources_->setText(videoSource_->currentText() + QStringLiteral("  ·  ")
            + (microphone ? QStringLiteral("麦克风") : QStringLiteral("系统声音")));
        footerSources_->setToolTip(audioSource_->currentText());
    }
    void updateOutputSummary() {
        const QSize size = resolution_->currentData().toSize(); preview_->setOutputSize(size);
        footerOutput_->setText(QStringLiteral("%1 × %2  ·  目标 %3 FPS  ·  H.264 / AAC")
            .arg(size.width()).arg(size.height()).arg(targetFps_->value()));
        addressDisplay_->setText(url_->text()); addressDisplay_->setCursorPosition(0);
    }
    config::PublisherConfig formConfig() const {
        auto config = initial_; config.capture.source = "devices";
        config.capture.videoSource = bytes(videoSource_->currentData().toString());
        config.capture.audioSource = bytes(audioSource_->currentData().toString());
        config.capture.video = true; config.capture.audio = true; config.capture.loop = false; config.capture.realtime = true;
        config.videoDevice = config.capture.videoSource == "camera" ? deviceValue(camera_) : "default";
        config.audioDevice = config.capture.audioSource == "microphone" ? bytes(audioSource_->currentData(audioDeviceRole).toString()) : "default";
        // 平台预览/参数校验使用占位地址，真实地址仅存在于申请成功后的配置副本中。
        config.outputUrl = api_ ? "rtmp://127.0.0.1/live/preparing" : bytes(url_->text());
        config.streaming.enabled = true; config.videoFile.clear(); config.audioFile.clear();
        const QSize size = resolution_->currentData().toSize(); config.video.width = size.width(); config.video.height = size.height();
        config.video.fps = targetFps_->value();
        config.video.bitrate = targetBitrate_->value() == initial_.video.bitrate / 1000
            ? initial_.video.bitrate : targetBitrate_->value() * 1000;
        config.videoEncoder.backend = bytes(encodingBackend_->currentData().toString());
        config.videoEncoder.lowLatency = lowLatency_->isChecked();
        return config;
    }
    void clearPreview() { preview_->clearFrame(); lastSequence_ = 0; }
    void showUiError(const std::exception& problem) {
        uiError_ = api_ ? QStringLiteral("媒体操作失败，请查看已脱敏的运行日志。")
            : utf8(logging::redactSecrets(problem.what()));
        logger_.log(logging::Level::Error, "publisher_ui", bytes(uiError_));
        banner_->setText(uiError_); banner_->show();
    }
    void startLive() {
        if (closing_ || pendingLive_) return;
        if (api_) { startPlatformLive(); return; }
        try {
            uiError_.clear(); auto config = formConfig(); config::validate(config);
            if (session_.snapshot().state == State::Previewing) {
                pendingLive_ = std::move(config); session_.requestStop();
            } else {
                previewRequested_ = false; clearPreview(); session_.start(config);
            }
            poll();
        } catch (const std::exception& problem) { showUiError(problem); }
    }
    void togglePreview() {
        if (closing_ || pendingLive_ || (api_ && (platformBusy() || !observedSessionId_.isEmpty()))) return;
        try {
            uiError_.clear(); const auto snapshot = session_.snapshot();
            if (previewRequested_ && active(snapshot.state)) session_.requestStop();
            else if (!active(snapshot.state)) { clearPreview(); previewRequested_ = session_.startPreview(formConfig()); }
            poll();
        } catch (const std::exception& problem) { showUiError(problem); }
    }
    void updatePreviewAndLogs() {
        if (const auto frame = session_.preview(); frame && frame->sequence != lastSequence_) {
            if (frame->width > 0 && frame->height > 0 && frame->stride >= frame->width * 3 &&
                frame->pixels.size() >= static_cast<std::size_t>(frame->stride) * frame->height) {
                preview_->setFrame(QImage(frame->pixels.data(), frame->width, frame->height, frame->stride, QImage::Format_RGB888).copy());
                lastSequence_ = frame->sequence;
            }
        }
        for (const auto& line : logger_.linesSince(logCursor_)) log_->appendPlainText(utf8(line).trimmed());
    }
    void pollSafely() {
        try { poll(); }
        catch (const std::exception& problem) { session_.requestStop(); showUiError(problem); }
    }
    void updateStats(const pipeline::PublisherSessionSnapshot& snapshot) {
        const auto& performance = snapshot.performance;
        const bool sampled = performance.sampled;
        statsSample_->setText(!sampled ? QStringLiteral("等待首个统计样本")
            : (active(snapshot.state) ? QStringLiteral("采样窗口统计 · 本次会话累计") : QStringLiteral("会话结束 · 已保留累计值")));
        captureRate_->setText(sampled ? QStringLiteral("%1 FPS").arg(performance.captureFps, 0, 'f', 1) : QStringLiteral("—"));
        encodeRate_->setText(sampled ? QStringLiteral("%1 FPS").arg(performance.encodeFps, 0, 'f', 1) : QStringLiteral("—"));
        sendRate_->setText(sampled ? QStringLiteral("%1 kbps").arg(performance.sendBitrate / 1000.0, 0, 'f', 1) : QStringLiteral("—"));
        bitrate_->setValue(sampled ? QStringLiteral("%1 kbps").arg(performance.sendBitrate / 1000.0, 0, 'f', 0) : QStringLiteral("—"));
        fps_->setValue(sampled ? QStringLiteral("%1 FPS").arg(performance.encodeFps, 0, 'f', 1) : QStringLiteral("—"));
        captureRate_->setToolTip(sampled ? QStringLiteral("本次会话实际采集：%1 帧").arg(static_cast<qulonglong>(performance.capturedVideoFrames))
            : QStringLiteral("等待首个统计样本"));
        encodeRate_->setToolTip(sampled ? QStringLiteral("本次会话实际送入编码器：%1 帧").arg(static_cast<qulonglong>(performance.encodedVideoFrames))
            : QStringLiteral("等待首个统计样本"));
        sendRate_->setToolTip(sampled ? QStringLiteral("成功发送累计 payload：%1 字节；不含封装与网络协议开销")
            .arg(static_cast<qulonglong>(performance.sentBytes)) : QStringLiteral("等待首个统计样本"));
        showQueue(rawVideoQueue_, performance.rawVideo, QStringLiteral("帧"), sampled);
        showQueue(rawAudioQueue_, performance.rawAudio, QStringLiteral("块"), sampled);
        showQueue(outputQueue_, performance.outputEvents, QStringLiteral("项"), sampled);
        showQueue(networkPacketQueue_, performance.networkPackets, QStringLiteral("包"), sampled);
        networkVideoQueue_->setText(sampled ? QStringLiteral("%1 包").arg(static_cast<qulonglong>(performance.networkVideoPackets)) : QStringLiteral("—"));
        networkAudioQueue_->setText(sampled ? QStringLiteral("%1 包").arg(static_cast<qulonglong>(performance.networkAudioPackets)) : QStringLiteral("—"));
        droppedFrames_->setText(sampled ? QStringLiteral("%1 帧").arg(static_cast<qulonglong>(performance.droppedVideoFrames)) : QStringLiteral("—"));
        droppedFrames_->setToolTip(sampled ? QStringLiteral("原始视频队列/时效丢弃 %1 帧，预处理丢弃 %2 帧；不含压缩包")
            .arg(static_cast<qulonglong>(performance.rawVideoDrops)).arg(static_cast<qulonglong>(performance.preprocessVideoDrops))
            : QStringLiteral("等待首个统计样本"));
        droppedPackets_->setText(sampled ? QStringLiteral("%1 包").arg(static_cast<qulonglong>(performance.droppedPackets)) : QStringLiteral("—"));
        abortedEvents_->setText(sampled ? QStringLiteral("%1 项").arg(static_cast<qulonglong>(performance.abortedOutputEvents)) : QStringLiteral("—"));
        abortedEvents_->setToolTip(QStringLiteral("封装输出队列中止时丢弃的混合事件，包含参数、编码包或结束事件；不等同于视频帧或压缩包"));
    }
    void poll() {
        if (api_ && !closing_ && !needsLogin_ && !api_->authenticated() && !roomId_.isEmpty()) {
            cancelRequested_ = true; pendingLive_.reset(); pendingPlatformConfig_.reset();
            session_.requestStop(); needsLogin_ = true;
            uiError_ = QStringLiteral("登录已失效，本地媒体已停止，请重新登录。");
        }
        auto snapshot = session_.snapshot();
        if (pendingLive_ && snapshot.state == State::Stopped) {
            auto config = std::move(*pendingLive_); pendingLive_.reset(); previewRequested_ = false;
            if (!closing_ && !cancelRequested_) {
                clearPreview();
                const bool started = session_.start(config);
                if (api_ && !started) requestPlatformStop();
                snapshot = session_.snapshot();
            }
        } else if (pendingLive_ && !active(snapshot.state)) {
            // 预览关闭失败时保留错误，不自动启动另一轮设备采集。
            pendingLive_.reset();
        }
        if (!active(snapshot.state)) previewRequested_ = false;
        const bool running = active(snapshot.state), stopping = snapshot.state == State::Stopping;
        captureSection_->setEnabled(!running && !closing_ && !pendingLive_);
        outputSection_->setEnabled(!running && !closing_ && !pendingLive_);
        start_->setEnabled(!closing_ && !pendingLive_ && (!running || snapshot.state == State::Previewing));
        stop_->setEnabled(!closing_ && running && !previewRequested_ && !stopping && !pendingLive_);
        previewButton_->setEnabled(!closing_ && !pendingLive_ && (!running || (previewRequested_ && !stopping)));
        if (api_) {
            const bool busy = platformBusy();
            const bool ready = api_->authenticated() && roomLoaded_ && !roomId_.isEmpty();
            const bool verifyCancelled = canVerifyCancelledRequest();
            start_->setText(verifyCancelled ? QStringLiteral("核实并结束上次申请") : QStringLiteral("开始直播"));
            start_->setEnabled(ready && !closing_ && !busy && !logoutRequested_ && !pendingLive_ &&
                (!api_->publishRecovery().stopRequested || verifyCancelled) && observedSessionId_.isEmpty()
                && (!running || snapshot.state == State::Previewing));
            stop_->setEnabled(!closing_ && !stopBusy_ &&
                (applying_ || pendingPlatformConfig_ || uncertainApply_ || !observedSessionId_.isEmpty() ||
                 (running && !previewRequested_)));
            stop_->setText(ownedSessionId_.isEmpty() && !observedSessionId_.isEmpty()
                ? QStringLiteral("结束已有会话") : QStringLiteral("停止直播"));
            previewButton_->setEnabled(ready && !closing_ && !busy && observedSessionId_.isEmpty()
                && !pendingLive_ && (!running || (previewRequested_ && !stopping)));
            captureSection_->setEnabled(!running && !closing_ && !busy && !pendingLive_);
            outputSection_->setEnabled(!running && !closing_ && !busy && !pendingLive_);
            roomTitle_->setEnabled(!closing_ && !busy); category_->setEnabled(!closing_ && !busy);
            saveRoom_->setEnabled(ready && !closing_ && !busy);
            refreshRoom_->setEnabled(api_->authenticated() && !closing_ && !busy && !sessionQueryBusy_);
            logout_->setEnabled(!closing_ && !busy && !logoutRequested_);
            if (mediaOwned_ && !running && !pendingLive_ && !cleanupRequested_ && !ownedSessionId_.isEmpty()) {
                cleanupRequested_ = true; requestPlatformStop();
            }
            if (needsLogin_ && !running && !busy && !closing_) {
                ++sessionEpoch_; api_->cancel(this); sessionQueryBusy_ = false; needsLogin_ = false;
                ownedSessionId_.clear(); observedSessionId_.clear(); roomId_.clear(); requestId_.clear();
                uncertainApply_ = false; mediaOwned_ = false; cancelRequested_ = false; roomLoaded_ = false;
                logoutRequested_ = false;
                pendingPlatformConfig_.reset(); pendingLive_.reset(); password_->clear();
                pages_->setCurrentWidget(loginPage_);
            }
            if (logoutRequested_ && !running && !platformBusy() && !closing_) finishLogout();
        }
        previewButton_->setText(previewRequested_ && running ? QStringLiteral("停止预览") : QStringLiteral("开始预览"));
        const auto state = stateText(snapshot.state, previewRequested_); preview_->setStatus(state, snapshot.state == State::Streaming);
        updateStats(snapshot); duration_->setValue(durationText(snapshot.liveSeconds));
        connection_->setValue(previewRequested_ ? QStringLiteral("未推流") : state);
        connection_->setHint(previewRequested_ ? QStringLiteral("仅本地视频采集") : QStringLiteral("RTMP 推流连接"));
        footerState_->setText(QStringLiteral("●  ") + state); sidebarStatus_->setText(QStringLiteral("●  本机控制台 · ") + state);
        if (!snapshot.error.empty() || !uiError_.isEmpty() || closing_) {
            banner_->setText(closing_ ? QStringLiteral("正在停止采集并释放资源，完成后自动关闭窗口。")
                : (!uiError_.isEmpty() ? uiError_ : utf8(logging::redactSecrets(snapshot.error)))); banner_->show();
        } else banner_->hide();
        updatePreviewAndLogs();
        if (closing_ && !running && (!platformBusy() || remoteCloseGraceExpired())) close();
    }

    config::PublisherConfig initial_;
    logging::Logger& logger_;
    pipeline::PublisherSession session_;
    platform::ApiClient* api_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* loginPage_ = nullptr;
    QTimer* platformTimer_ = nullptr;
    QLineEdit *username_ = nullptr, *nickname_ = nullptr, *password_ = nullptr, *roomTitle_ = nullptr;
    QComboBox* category_ = nullptr;
    QLabel *authState_ = nullptr, *platformState_ = nullptr, *account_ = nullptr;
    QPushButton *login_ = nullptr, *register_ = nullptr, *saveRoom_ = nullptr, *refreshRoom_ = nullptr, *logout_ = nullptr;
    QString roomId_, observedSessionId_, ownedSessionId_, requestId_, platformStatus_;
    std::uint64_t sessionEpoch_ = 0;
    std::optional<config::PublisherConfig> pendingPlatformConfig_;
    bool authBusy_ = false, roomBusy_ = false, applying_ = false, stopBusy_ = false, recoveryBusy_ = false,
        logoutBusy_ = false, sessionQueryBusy_ = false, needsLogin_ = false, uncertainApply_ = false,
        mediaOwned_ = false, cancelRequested_ = false, cleanupRequested_ = false;
    bool roomLoaded_ = false, needsStopConfirmation_ = false, logoutRequested_ = false;
    int stopPostAttempts_ = 0;
    QElapsedTimer closeClock_;
    std::optional<config::PublisherConfig> pendingLive_;
    bool previewRequested_ = false, closing_ = false;
    std::uint64_t lastSequence_ = 0, logCursor_ = 0;
    QString uiError_;
    QTimer* timer_ = nullptr;
    std::vector<QPushButton*> navigation_;
    QScrollArea* settingsScroll_ = nullptr;
    QWidget *settingsContent_ = nullptr, *captureSection_ = nullptr, *outputSection_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    MetricCard *bitrate_ = nullptr, *fps_ = nullptr, *duration_ = nullptr, *connection_ = nullptr;
    QPushButton *start_ = nullptr, *stop_ = nullptr, *previewButton_ = nullptr, *refresh_ = nullptr,
        *copyAddress_ = nullptr, *clearLogs_ = nullptr;
    QComboBox *videoSource_ = nullptr, *audioSource_ = nullptr, *camera_ = nullptr, *resolution_ = nullptr;
    QComboBox* encodingBackend_ = nullptr;
    QSpinBox *targetFps_ = nullptr, *targetBitrate_ = nullptr;
    QCheckBox* lowLatency_ = nullptr;
    QLineEdit *url_ = nullptr, *addressDisplay_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QLabel *cameraLabel_ = nullptr, *videoDescription_ = nullptr, *audioDescription_ = nullptr,
        *deviceError_ = nullptr, *banner_ = nullptr, *footerState_ = nullptr, *footerSources_ = nullptr,
        *footerOutput_ = nullptr, *sidebarStatus_ = nullptr;
    QLabel *statsSample_ = nullptr, *captureRate_ = nullptr, *encodeRate_ = nullptr, *sendRate_ = nullptr,
        *rawVideoQueue_ = nullptr, *rawAudioQueue_ = nullptr, *outputQueue_ = nullptr,
        *networkPacketQueue_ = nullptr, *networkVideoQueue_ = nullptr, *networkAudioQueue_ = nullptr,
        *droppedFrames_ = nullptr, *droppedPackets_ = nullptr, *abortedEvents_ = nullptr;
};
} // namespace

QWidget* createPublisherControl(const config::PublisherConfig& config, logging::Logger& logger) {
    return new PublisherWindow(config, logger);
}
QWidget* createPlatformPublisherControl(const config::PublisherConfig& config, logging::Logger& logger,
                                       platform::ApiClient& api) {
    return new PublisherWindow(config, logger, &api);
}
} // namespace lsl::ui
