#include "player/ui/PlayerControl.hpp"
#include "player/pipeline/PlayerSession.hpp"
#include "common/stats/PerformanceStats.hpp"

#include <SDL3/SDL.h>

#include <QApplication>
#include <QByteArray>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <utility>

namespace lsl::ui {
namespace {
using State = player::PlayerSessionState;

QString utf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
std::string bytes(const QString& value) {
    const auto encoded = value.toUtf8();
    return {encoded.constData(), static_cast<std::size_t>(encoded.size())};
}
bool active(State state) noexcept {
    return state == State::Connecting || state == State::Buffering || state == State::Playing ||
           state == State::Paused || state == State::Reconnecting || state == State::Stopping;
}
QString stateText(State state) {
    switch (state) {
    case State::Idle: return QStringLiteral("未连接");
    case State::Connecting: return QStringLiteral("正在连接");
    case State::Buffering: return QStringLiteral("正在缓冲");
    case State::Playing: return QStringLiteral("播放中");
    case State::Paused: return QStringLiteral("已暂停");
    case State::Reconnecting: return QStringLiteral("正在重连");
    case State::Stopping: return QStringLiteral("正在停止");
    case State::Stopped: return QStringLiteral("已停止");
    case State::Failed: return QStringLiteral("播放失败");
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
QString latencyText(const stats::PlayerPerformance& performance) {
    return performance.latencyStatus == stats::LatencyStatus::Estimated
        ? QStringLiteral("约 %1 ms").arg(performance.endToEndMs, 0, 'f', 0) : QStringLiteral("不可测");
}
QString latencyReason(stats::LatencyStatus state) {
    switch (state) {
    case stats::LatencyStatus::Estimated:
        return QStringLiteral("采集输出到本窗口绘制完成的估计；依赖收发系统时钟同步，不含曝光或物理显示时延。");
    case stats::LatencyStatus::ClockError:
        return QStringLiteral("时钟异常或时间戳过期；请检查采集端与播放端的系统时钟是否同步。");
    case stats::LatencyStatus::Inactive:
        return QStringLiteral("暂停、停止或尚未显示有效帧，当前无有效端到端样本。");
    case stats::LatencyStatus::NoTimestamp:
        return QStringLiteral("流未携带采集时间戳，无法估计采集到显示的端到端时延。");
    }
    return QStringLiteral("尚无有效端到端样本。");
}
QLabel* label(const QString& text, const char* name) {
    auto* result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    result->setObjectName(QString::fromLatin1(name));
    return result;
}

enum class Glyph { Studio, Connect, Stop, Pause, Play, Volume, Fullscreen };
QIcon icon(Glyph glyph, QColor color = QColor(141, 164, 172)) {
    QPixmap image(48, 48);
    image.setDevicePixelRatio(2); image.fill(Qt::transparent);
    QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (glyph) {
    case Glyph::Studio:
        painter.setPen(Qt::NoPen); painter.setBrush(QColor(100, 201, 177));
        painter.drawRoundedRect(QRectF(1, 1, 22, 22), 6, 6);
        painter.setPen(QPen(QColor(12, 31, 28), 2.1, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(7, 7), QPointF(7, 16)); painter.drawLine(QPointF(7, 16), QPointF(15, 16));
        painter.drawLine(QPointF(12, 7), QPointF(17, 7)); painter.drawLine(QPointF(17, 7), QPointF(17, 12));
        break;
    case Glyph::Connect:
    case Glyph::Play:
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(8, 5), QPointF(19, 12), QPointF(8, 19)}); break;
    case Glyph::Stop:
        painter.setBrush(color); painter.drawRoundedRect(QRectF(7, 7, 10, 10), 1, 1); break;
    case Glyph::Pause:
        painter.setPen(Qt::NoPen); painter.setBrush(color);
        painter.drawRoundedRect(QRectF(7, 5, 3, 14), 1, 1); painter.drawRoundedRect(QRectF(14, 5, 3, 14), 1, 1); break;
    case Glyph::Volume:
        painter.drawPolygon(QPolygonF{QPointF(3, 9), QPointF(7, 9), QPointF(12, 5), QPointF(12, 19), QPointF(7, 15), QPointF(3, 15)});
        painter.drawArc(QRectF(10, 7, 8, 10), -65 * 16, 130 * 16);
        painter.drawArc(QRectF(8, 3, 15, 18), -55 * 16, 110 * 16); break;
    case Glyph::Fullscreen:
        for (const auto& corner : {QPointF(4, 4), QPointF(20, 4), QPointF(4, 20), QPointF(20, 20)}) {
            const qreal dx = corner.x() < 12 ? 5 : -5;
            const qreal dy = corner.y() < 12 ? 5 : -5;
            painter.drawLine(corner, corner + QPointF(dx, 0)); painter.drawLine(corner, corner + QPointF(0, dy));
        }
        break;
    }
    return QIcon(image);
}

class VideoSurface final : public QWidget {
public:
    VideoSurface() {
        setMinimumSize(400, 225); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::StrongFocus);
    }
    QSize sizeHint() const override { return {960, 540}; }
    void setFrame(QImage frame, std::uint64_t sequence, std::int64_t capturedUnixUs) {
        frame_ = std::move(frame); sequence_ = sequence; capturedUnixUs_ = capturedUnixUs; update();
    }
    void clearFrame() { frame_ = {}; sequence_ = 0; capturedUnixUs_ = 0; lastPresentedSequence_ = 0; update(); }
    void setPlayback(State state, bool audioOnly) {
        if (state_ == state && audioOnly_ == audioOnly) return;
        state_ = state; audioOnly_ = audioOnly; update();
    }
    std::function<void()> toggleFullscreen;
    std::function<void(std::uint64_t, std::int64_t)> presented;
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) setFocus(Qt::MouseFocusReason);
        QWidget::mousePressEvent(event);
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && toggleFullscreen) {
            toggleFullscreen(); event->accept();
        } else QWidget::mouseDoubleClickEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip; clip.addRoundedRect(outer, 10, 10); painter.setClipPath(clip);
        painter.fillRect(rect(), QColor(5, 11, 15));
        QSizeF viewport(16, 9); viewport.scale(outer.size(), Qt::KeepAspectRatio);
        const QRectF target(QPointF((width() - viewport.width()) / 2, (height() - viewport.height()) / 2), viewport);
        painter.fillRect(target, QColor(7, 14, 18));
        if (!frame_.isNull()) {
            QSizeF fitted = frame_.size(); fitted.scale(target.size(), Qt::KeepAspectRatio);
            painter.drawImage(QRectF(QPointF(target.center().x() - fitted.width() / 2,
                target.center().y() - fitted.height() / 2), fitted), frame_);
        } else {
            painter.setPen(QPen(QColor(24, 38, 44), 1));
            for (int x = 0; x < width(); x += 40) painter.drawLine(x, 0, x, height());
            for (int y = 0; y < height(); y += 40) painter.drawLine(0, y, width(), y);
            const QPointF center(width() / 2.0, height() / 2.0 - 28);
            painter.setPen(QPen(QColor(77, 112, 116), 1.8));
            painter.drawRoundedRect(QRectF(center.x() - 27, center.y() - 20, 54, 36), 5, 5);
            painter.drawLine(center + QPointF(-10, 22), center + QPointF(10, 22));
            painter.drawLine(center + QPointF(0, 16), center + QPointF(0, 22));
            painter.setPen(Qt::NoPen); painter.setBrush(QColor(84, 145, 134));
            painter.drawPolygon(QPolygonF{center + QPointF(-5, -11), center + QPointF(10, -2), center + QPointF(-5, 7)});
            painter.setPen(QColor(175, 195, 200));
            const auto title = audioOnly_ ? QStringLiteral("音频流")
                : (active(state_) ? stateText(state_) : QStringLiteral("等待播放直播"));
            painter.drawText(QRectF(16, height() / 2.0 + 12, width() - 32, 27), Qt::AlignCenter, title);
            painter.setPen(QColor(95, 123, 135));
            auto small = painter.font(); small.setPointSizeF(9); painter.setFont(small);
            painter.drawText(QRectF(16, height() / 2.0 + 41, width() - 32, 24), Qt::AlignCenter,
                audioOnly_ ? QStringLiteral("当前直播流没有视频画面") : QStringLiteral("输入直播流地址，连接后显示实际解码画面"));
        }
        if (!frame_.isNull() && (state_ == State::Paused || state_ == State::Reconnecting || state_ == State::Buffering)) {
            painter.setPen(QPen(QColor(51, 77, 80), 1)); painter.setBrush(QColor(10, 24, 29, 225));
            painter.drawRoundedRect(QRectF(14, 14, 100, 28), 6, 6);
            painter.setPen(QColor(177, 213, 207));
            painter.drawText(QRectF(14, 14, 100, 28), Qt::AlignCenter, stateText(state_));
        }
        if (!frame_.isNull()) {
            painter.setPen(QColor(166, 188, 195));
            painter.drawText(QRectF(14, height() - 29, width() - 28, 18), Qt::AlignRight | Qt::AlignVCenter,
                QStringLiteral("%1 × %2").arg(frame_.width()).arg(frame_.height()));
        }
        painter.setClipping(false); painter.setPen(QPen(QColor(36, 53, 62), 1));
        painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(outer, 10, 10);
        painter.end();
        // 以实际 Qt 绘制完成作为估计终点；暂停/缩放重绘不能重复上报同一帧。
        if (!frame_.isNull() && sequence_ != 0 && sequence_ != lastPresentedSequence_) {
            lastPresentedSequence_ = sequence_;
            if (presented) presented(sequence_, capturedUnixUs_);
        }
    }
private:
    QImage frame_;
    State state_ = State::Idle;
    bool audioOnly_ = false;
    std::uint64_t sequence_ = 0, lastPresentedSequence_ = 0;
    std::int64_t capturedUnixUs_ = 0;
};

const char* stylesheet = R"QSS(
QWidget { color: #c5d1d5; font-size: 12px; }
QWidget#playerWindow { background: #0b1115; }
QLabel#brand { color: #deebeb; font-size: 17px; font-weight: 600; }
QLabel#subtle { color: #6e8792; font-size: 10px; }
QLabel#fieldLabel { color: #92abb5; font-size: 11px; }
QLabel#statsValue { color: #bcd2d6; font-size: 11px; }
QLabel#status { color: #87bcae; background: #142620; border: 1px solid #2b4a40; border-radius: 6px; padding: 5px 12px; font-size: 11px; }
QLabel#status[tone="warning"] { color: #d1b487; background: #28231b; border-color: #514630; }
QLabel#status[tone="danger"] { color: #db9f98; background: #2d1e1f; border-color: #5d3d3b; }
QFrame#panel { background: #101a20; border: 1px solid #24333b; border-radius: 8px; }
QPushButton { background: #142129; color: #adc1c9; border: 1px solid #30434d; border-radius: 6px; padding: 7px 12px; }
QPushButton:hover { background: #1c2d36; color: #dbe9e9; border-color: #48636b; }
QPushButton:pressed { background: #102126; }
QPushButton:disabled { color: #4d6670; background: #101b21; border-color: #26363e; }
QPushButton#connect { background: #64c5ad; color: #102c25; border-color: #7cd3bc; font-weight: 600; }
QPushButton#connect:hover { background: #7bd2bd; }
QPushButton#connect:disabled { background: #213b34; border-color: #2f5047; color: #65887c; }
QPushButton#stop { background: #271e21; color: #d0a19a; border-color: #573c3d; }
QPushButton#stop:disabled { color: #655754; background: #171a1e; border-color: #343139; }
QPushButton#mute:checked { color: #8cd5bf; background: #173129; border-color: #3a6958; }
QPushButton#link { padding: 2px 4px; background: transparent; border: none; color: #7ba798; font-size: 10px; }
QLineEdit { background: #0b151c; color: #c9dbe1; border: 1px solid #304650; border-radius: 6px; padding: 7px 10px; min-height: 17px; selection-background-color: #315a4d; }
QLineEdit:focus { border-color: #619e8e; }
QLineEdit:disabled { color: #6e8691; background: #0e181e; border-color: #293b44; }
QSlider::groove:horizontal { height: 4px; background: #293b44; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #62b9a2; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; background: #93d3c2; border: 1px solid #b0e1d1; }
QSlider::groove:horizontal:disabled { background: #233038; }
QPlainTextEdit { background: #0c151b; color: #88a3af; border: none; font-family: Consolas, 'Microsoft YaHei UI'; font-size: 10px; selection-background-color: #285448; }
QScrollBar:vertical { width: 6px; background: transparent; }
QScrollBar::handle:vertical { min-height: 28px; background: #2b424d; border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QLabel#error { color: #e3b2ab; background: #2a1d1f; border: 1px solid #563b3b; border-radius: 6px; padding: 7px 10px; font-size: 11px; }
)QSS";

class PlayerWindow final : public QWidget {
public:
    PlayerWindow(config::PlayerConfig config, logging::Logger& logger)
        : initial_(std::move(config)), logger_(logger), session_(logger),
          muted_(initial_.audioPlayback.muted), paused_(initial_.audioPlayback.paused), volume_(initial_.audioPlayback.volume) {
        logger_.enableMemoryLog(512);
        setObjectName(QStringLiteral("playerWindow")); setWindowTitle(QStringLiteral("Local Live Studio · 观众播放"));
        setWindowIcon(icon(Glyph::Studio)); resize(1100, 760); setMinimumSize(880, 620);
        setStyleSheet(QString::fromLatin1(stylesheet));
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(18, 14, 18, 12); layout->setSpacing(9);
        auto* header = new QHBoxLayout; header->setSpacing(10);
        auto* brandMark = new QLabel; brandMark->setPixmap(icon(Glyph::Studio).pixmap(30, 30)); header->addWidget(brandMark);
        header->addWidget(label(QStringLiteral("LOCAL LIVE"), "brand"));
        header->addWidget(label(QStringLiteral("观众播放器"), "fieldLabel")); header->addStretch();
        status_ = label(QStringLiteral("●  未连接"), "status"); header->addWidget(status_); layout->addLayout(header);
        auto* addressPanel = new QFrame; addressPanel->setObjectName(QStringLiteral("panel"));
        auto* addressRow = new QHBoxLayout(addressPanel); addressRow->setContentsMargins(12, 9, 12, 9); addressRow->setSpacing(9);
        addressRow->addWidget(label(QStringLiteral("直播流地址"), "fieldLabel"));
        url_ = new QLineEdit(utf8(initial_.inputUrl)); url_->setPlaceholderText(QStringLiteral("RTMP(S) 或 HTTP(S)-FLV 地址"));
        addressRow->addWidget(url_, 1);
        connect_ = new QPushButton(icon(Glyph::Connect, QColor(19, 53, 44)), QStringLiteral("连接直播"));
        connect_->setObjectName(QStringLiteral("connect")); connect_->setCursor(Qt::PointingHandCursor);
        stop_ = new QPushButton(icon(Glyph::Stop, QColor(193, 140, 132)), QStringLiteral("停止"));
        stop_->setObjectName(QStringLiteral("stop")); stop_->setCursor(Qt::PointingHandCursor);
        addressRow->addWidget(connect_); addressRow->addWidget(stop_); layout->addWidget(addressPanel);
        error_ = label({}, "error"); error_->setWordWrap(true); error_->hide(); layout->addWidget(error_);
        video_ = new VideoSurface; video_->toggleFullscreen = [this] { toggleFullscreen(); };
        video_->presented = [this](std::uint64_t sequence, std::int64_t capturedUnixUs) {
            session_.notePresented(sequence, capturedUnixUs);
        };
        layout->addWidget(video_, 1);
        auto* controls = new QHBoxLayout; controls->setContentsMargins(1, 0, 1, 0); controls->setSpacing(10);
        pause_ = new QPushButton(icon(Glyph::Pause), QStringLiteral("暂停")); controls->addWidget(pause_);
        mute_ = new QPushButton(icon(Glyph::Volume), QStringLiteral("静音")); mute_->setObjectName(QStringLiteral("mute"));
        mute_->setCheckable(true); mute_->setChecked(muted_); controls->addWidget(mute_);
        controls->addWidget(label(QStringLiteral("音量"), "fieldLabel"));
        volumeSlider_ = new QSlider(Qt::Horizontal); volumeSlider_->setRange(0, 100); volumeSlider_->setValue(volume_);
        volumeSlider_->setMinimumWidth(100); volumeSlider_->setMaximumWidth(180); controls->addWidget(volumeSlider_);
        const auto audioTip = initial_.audioPlayback.enabled ? QStringLiteral("仅控制当前播放器的音频输出")
            : QStringLiteral("启动配置已禁用音频输出；静音和音量调整不可用");
        mute_->setToolTip(audioTip); volumeSlider_->setToolTip(audioTip);
        volumeLabel_ = label(QStringLiteral("%1%").arg(volume_), "fieldLabel"); volumeLabel_->setMinimumWidth(33);
        controls->addWidget(volumeLabel_); controls->addStretch();
        playbackTime_ = label(QStringLiteral("00:00:00"), "fieldLabel");
        playbackTime_->setToolTip(QStringLiteral("首次实际输出后的会话经过时间；暂停时冻结，重连期间继续计时，非媒体 PTS"));
        controls->addWidget(playbackTime_);
        fullscreen_ = new QPushButton(icon(Glyph::Fullscreen), QStringLiteral("全屏"));
        fullscreen_->setToolTip(QStringLiteral("F / Alt+Enter / 双击画面；Esc 退出全屏")); controls->addWidget(fullscreen_);
        layout->addLayout(controls);
        auto* statsPanel = new QFrame; statsPanel->setObjectName(QStringLiteral("panel"));
        auto* statsLayout = new QVBoxLayout(statsPanel); statsLayout->setContentsMargins(10, 5, 10, 5); statsLayout->setSpacing(5);
        auto* statsHeader = new QHBoxLayout; statsHeader->setSpacing(14);
        toggleStats_ = new QPushButton(QStringLiteral("性能统计  ▸")); toggleStats_->setObjectName(QStringLiteral("link"));
        toggleStats_->setCheckable(true); statsHeader->addWidget(toggleStats_);
        statsSummary_ = label(QStringLiteral("解码 — FPS  ·  接收 — kbps  ·  同步 — ms  ·  端到端 不可测"), "subtle");
        statsSummary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); statsHeader->addWidget(statsSummary_, 1);
        statsLayout->addLayout(statsHeader);
        statsDetails_ = new QWidget; auto* detailsLayout = new QVBoxLayout(statsDetails_);
        detailsLayout->setContentsMargins(3, 2, 3, 3); detailsLayout->setSpacing(6);
        auto* statsGrid = new QGridLayout; statsGrid->setHorizontalSpacing(14); statsGrid->setVerticalSpacing(5);
        const auto addStat = [statsGrid](int row, int column, const QString& title, QLabel*& value) {
            statsGrid->addWidget(label(title, "fieldLabel"), row, column);
            value = label(QStringLiteral("—"), "statsValue"); value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            statsGrid->addWidget(value, row, column + 1);
        };
        addStat(0, 0, QStringLiteral("视频包队列"), videoPacketQueue_);
        addStat(0, 2, QStringLiteral("音频包队列"), audioPacketQueue_);
        addStat(1, 0, QStringLiteral("解码视频队列"), decodedVideoQueue_);
        addStat(1, 2, QStringLiteral("PCM 缓冲"), pcmQueue_);
        addStat(2, 0, QStringLiteral("累计视频丢帧"), droppedFrames_);
        addStat(2, 2, QStringLiteral("累计压缩包丢弃"), droppedPackets_);
        addStat(3, 0, QStringLiteral("音画同步误差"), syncError_);
        addStat(3, 2, QStringLiteral("端到端估计"), endToEnd_);
        statsGrid->setColumnStretch(1, 1); statsGrid->setColumnStretch(3, 1); detailsLayout->addLayout(statsGrid);
        latencyReason_ = label(QStringLiteral("端到端不可测：等待首个统计样本"), "subtle"); latencyReason_->setWordWrap(true);
        detailsLayout->addWidget(latencyReason_); statsSample_ = label(QStringLiteral("等待首个统计样本"), "subtle");
        detailsLayout->addWidget(statsSample_); statsDetails_->hide(); statsLayout->addWidget(statsDetails_); layout->addWidget(statsPanel);
        auto* logs = new QFrame; logs->setObjectName(QStringLiteral("panel")); auto* logLayout = new QVBoxLayout(logs);
        logLayout->setContentsMargins(10, 5, 10, 5); logLayout->setSpacing(4);
        auto* logHeader = new QHBoxLayout;
        toggleLogs_ = new QPushButton(QStringLiteral("运行日志  ▸")); toggleLogs_->setObjectName(QStringLiteral("link"));
        toggleLogs_->setCheckable(true); logHeader->addWidget(toggleLogs_);
        logHint_ = label(QStringLiteral("最近 1000 行"), "subtle"); logHint_->hide(); logHeader->addWidget(logHint_); logHeader->addStretch();
        clearLogs_ = new QPushButton(QStringLiteral("清空")); clearLogs_->setObjectName(QStringLiteral("link")); clearLogs_->hide(); logHeader->addWidget(clearLogs_);
        logLayout->addLayout(logHeader); log_ = new QPlainTextEdit; log_->setReadOnly(true); log_->setMaximumBlockCount(1000);
        log_->setLineWrapMode(QPlainTextEdit::NoWrap); log_->setFixedHeight(90); log_->hide(); logLayout->addWidget(log_); layout->addWidget(logs);
        auto* footer = new QHBoxLayout; media_ = label(QStringLiteral("支持 RTMP(S) / HTTP(S)-FLV"), "subtle"); footer->addWidget(media_);
        footer->addStretch(); footer->addWidget(label(QStringLiteral("音量与静音仅作用于当前播放器"), "subtle")); layout->addLayout(footer);
        connectControls(); updateSoundControls();
        logClock_.start(); timer_ = new QTimer(this); timer_->setTimerType(Qt::PreciseTimer); timer_->setInterval(16);
        connect(timer_, &QTimer::timeout, this, [this] { pollSafely(); }); timer_->start();
        logger_.log(logging::Level::Info, "player_ui", "Qt Widgets player ready; input opens only on Connect"); poll();
    }
    ~PlayerWindow() override { timer_->stop(); session_.requestStop(); }
    void showConfigured() { show(); if (initial_.video.fullscreen) toggleFullscreen(); }
protected:
    void closeEvent(QCloseEvent* event) override {
        closing_ = true; session_.requestStop();
        if (active(session_.snapshot().state)) {
            event->ignore(); error_->setText(QStringLiteral("正在停止播放并释放资源，完成后自动关闭窗口。")); error_->show();
            connect_->setEnabled(false); stop_->setEnabled(false);
        } else event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        // QLineEdit 自行接收 F 等文本字符；只有未被输入控件处理的按键才来到这里。
        if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier && !textInputFocused()) {
            if (!event->isAutoRepeat()) toggleFullscreen(); event->accept(); return;
        }
        QWidget::keyPressEvent(event);
    }
private:
    bool textInputFocused() const {
        auto* focus = QApplication::focusWidget();
        return qobject_cast<QLineEdit*>(focus) || qobject_cast<QPlainTextEdit*>(focus);
    }
    void connectControls() {
        connect(connect_, &QPushButton::clicked, this, [this] { connectStream(); });
        connect(url_, &QLineEdit::returnPressed, this, [this] { if (connect_->isEnabled()) connectStream(); });
        connect(stop_, &QPushButton::clicked, this, [this] { session_.requestStop(); pollSafely(); });
        connect(pause_, &QPushButton::clicked, this, [this] {
            paused_ = !session_.snapshot().paused; session_.setPaused(paused_); updateSoundControls();
        });
        connect(mute_, &QPushButton::toggled, this, [this](bool value) {
            muted_ = value; session_.setMuted(value); updateSoundControls();
        });
        connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
            volume_ = value; session_.setVolume(value); volumeLabel_->setText(QStringLiteral("%1%").arg(value));
        });
        connect(fullscreen_, &QPushButton::clicked, this, [this] { toggleFullscreen(); });
        connect(toggleLogs_, &QPushButton::toggled, this, [this](bool expanded) {
            log_->setVisible(expanded); logHint_->setVisible(expanded); clearLogs_->setVisible(expanded);
            toggleLogs_->setText(expanded ? QStringLiteral("运行日志  ▾") : QStringLiteral("运行日志  ▸"));
        });
        connect(toggleStats_, &QPushButton::toggled, this, [this](bool expanded) {
            statsDetails_->setVisible(expanded);
            toggleStats_->setText(expanded ? QStringLiteral("性能统计  ▾") : QStringLiteral("性能统计  ▸"));
        });
        connect(clearLogs_, &QPushButton::clicked, log_, &QPlainTextEdit::clear);
        for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            auto* shortcut = new QShortcut(QKeySequence(QKeyCombination(Qt::AltModifier, key)), this);
            shortcut->setAutoRepeat(false); connect(shortcut, &QShortcut::activated, this, [this] { toggleFullscreen(); });
        }
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); escape->setAutoRepeat(false);
        connect(escape, &QShortcut::activated, this, [this] { if (isFullScreen()) toggleFullscreen(); });
    }
    void toggleFullscreen() {
        if (!isFullScreen()) {
            previousGeometry_ = saveGeometry(); wasMaximized_ = isMaximized(); showFullScreen();
            fullscreen_->setText(QStringLiteral("退出全屏"));
        } else {
            showNormal(); if (!previousGeometry_.isEmpty()) restoreGeometry(previousGeometry_);
            if (wasMaximized_) showMaximized(); fullscreen_->setText(QStringLiteral("全屏"));
        }
    }
    void updateSoundControls() {
        const QSignalBlocker muteBlock(mute_), volumeBlock(volumeSlider_);
        mute_->setChecked(muted_); mute_->setText(muted_ ? QStringLiteral("取消静音") : QStringLiteral("静音"));
        if (!volumeSlider_->isSliderDown()) volumeSlider_->setValue(volume_);
        volumeLabel_->setText(QStringLiteral("%1%").arg(volume_));
        const auto pauseText = paused_ ? QStringLiteral("继续") : QStringLiteral("暂停");
        if (pause_->text() != pauseText) {
            pause_->setText(pauseText); pause_->setIcon(icon(paused_ ? Glyph::Play : Glyph::Pause));
        }
    }
    config::PlayerConfig formConfig() const {
        auto config = initial_; config.inputUrl = bytes(url_->text().trimmed());
        config.audioPlayback.volume = volume_; config.audioPlayback.muted = muted_; config.audioPlayback.paused = paused_;
        return config;
    }
    void connectStream() {
        if (closing_ || active(session_.snapshot().state)) return;
        try {
            uiError_.clear(); auto config = formConfig(); config::validate(config);
            url_->setText(utf8(config.inputUrl)); video_->clearFrame();
            lastFrameRevision_ = std::numeric_limits<std::uint64_t>::max();
            session_.start(config); poll();
        } catch (const std::exception& problem) { showError(problem); }
    }
    void showError(const std::exception& problem) {
        uiError_ = QString::fromUtf8(problem.what()); error_->setText(uiError_); error_->show();
        logger_.log(logging::Level::Error, "player_ui", bytes(uiError_));
    }
    void pollSafely() {
        try { poll(); }
        catch (const std::exception& problem) { session_.requestStop(); showError(problem); }
    }
    void updateStats(const player::PlayerSessionSnapshot& snapshot) {
        const auto& performance = snapshot.performance; const bool sampled = performance.sampled;
        const auto fps = sampled ? QString::number(performance.decodeFps, 'f', 1) : QStringLiteral("—");
        const auto bitrate = sampled ? QString::number(performance.receiveBitrate / 1000.0, 'f', 1) : QStringLiteral("—");
        const auto sync = performance.syncValid ? QStringLiteral("%1%2 ms").arg(performance.syncErrorMs >= 0 ? QStringLiteral("+") : QString())
            .arg(performance.syncErrorMs, 0, 'f', 1) : QStringLiteral("— ms");
        const auto latency = latencyText(performance); const auto reason = latencyReason(performance.latencyStatus);
        statsSummary_->setText(QStringLiteral("解码 %1 FPS  ·  接收 %2 kbps  ·  同步 %3  ·  端到端 %4").arg(fps, bitrate, sync, latency));
        statsSummary_->setToolTip(QStringLiteral("接收码率为音视频编码 payload，不含协议开销。\n端到端：") + reason
            + (sampled ? QStringLiteral("\n本次会话累计：解码 %1 帧，接收 payload %2 字节")
                .arg(static_cast<qulonglong>(performance.decodedVideoFrames)).arg(static_cast<qulonglong>(performance.receivedBytes)) : QString()));
        statsSample_->setText(!sampled ? QStringLiteral("等待首个统计样本")
            : (active(snapshot.state) ? QStringLiteral("采样窗口统计 · 累计值属于本次会话") : QStringLiteral("会话结束 · 已保留累计值")));
        videoPacketQueue_->setText(queueText(performance.videoPackets, QStringLiteral("包"), sampled));
        audioPacketQueue_->setText(queueText(performance.audioPackets, QStringLiteral("包"), sampled));
        decodedVideoQueue_->setText(queueText(performance.videoFrames, QStringLiteral("帧"), sampled));
        const auto setPeak = [sampled](QLabel* value, const stats::QueueDepth& queue, const QString& unit) {
            value->setToolTip(sampled ? QStringLiteral("本次会话队列峰值：%1 %2")
                .arg(static_cast<qulonglong>(queue.highWaterMark)).arg(unit) : QStringLiteral("等待首个统计样本"));
        };
        setPeak(videoPacketQueue_, performance.videoPackets, QStringLiteral("包"));
        setPeak(audioPacketQueue_, performance.audioPackets, QStringLiteral("包"));
        setPeak(decodedVideoQueue_, performance.videoFrames, QStringLiteral("帧"));
        pcmQueue_->setText(!sampled ? QStringLiteral("—") : (performance.pcm.capacity == 0 ? QStringLiteral("未启用")
            : QStringLiteral("%1 ms").arg(performance.pcmMilliseconds, 0, 'f', 1)));
        pcmQueue_->setToolTip(sampled ? QStringLiteral("每声道样本帧：%1 / %2；本次会话峰值：%3 样本帧")
            .arg(static_cast<qulonglong>(performance.pcm.size)).arg(static_cast<qulonglong>(performance.pcm.capacity))
            .arg(static_cast<qulonglong>(performance.pcm.highWaterMark)) : QStringLiteral("等待首个统计样本"));
        droppedFrames_->setText(sampled ? QStringLiteral("%1 帧").arg(static_cast<qulonglong>(performance.droppedVideoFrames)) : QStringLiteral("—"));
        droppedPackets_->setText(sampled ? QStringLiteral("%1 包").arg(static_cast<qulonglong>(performance.droppedPackets)) : QStringLiteral("—"));
        syncError_->setText(sync);
        syncError_->setToolTip(QStringLiteral("视频 PTS − 音频估计播放位置；正值表示视频超前，负值表示视频落后。仅有效音频主时钟下显示。"));
        endToEnd_->setText(latency); endToEnd_->setToolTip(reason);
        latencyReason_->setText(QStringLiteral("端到端：") + reason);
    }
    void poll() {
        // SDL 音频设备事件/主线程回调需要事件泵；Qt 独占窗口与输入，不创建 SDL 视频窗口。
        if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
        }
        const auto snapshot = session_.snapshot(); const bool running = active(snapshot.state);
        const bool stopping = snapshot.state == State::Stopping;
        connect_->setEnabled(!running && !closing_); url_->setEnabled(!running && !closing_);
        stop_->setEnabled(running && !stopping && !closing_);
        pause_->setEnabled(running && !stopping && !closing_);
        mute_->setEnabled(initial_.audioPlayback.enabled && !stopping && !closing_);
        volumeSlider_->setEnabled(initial_.audioPlayback.enabled && !stopping && !closing_);
        if (running) { muted_ = snapshot.muted; paused_ = snapshot.paused; volume_ = snapshot.volume; updateSoundControls(); }
        const auto state = stateText(snapshot.state); status_->setText(QStringLiteral("●  ") + state);
        const QString desiredTone = snapshot.state == State::Failed ? QStringLiteral("danger")
            : ((snapshot.state == State::Reconnecting || snapshot.state == State::Buffering) ? QStringLiteral("warning") : QStringLiteral("normal"));
        if (status_->property("tone").toString() != desiredTone) {
            status_->setProperty("tone", desiredTone); status_->style()->unpolish(status_); status_->style()->polish(status_);
        }
        video_->setPlayback(snapshot.state, snapshot.hasAudio && !snapshot.hasVideo);
        updateStats(snapshot);
        playbackTime_->setText(durationText(snapshot.playbackSeconds));
        if (snapshot.frameRevision != lastFrameRevision_) {
            if (const auto frame = session_.frame(); frame && frame->width > 0 && frame->height > 0 &&
                frame->stride >= frame->width * 3 && frame->pixels.size() >= static_cast<std::size_t>(frame->stride) * frame->height) {
                video_->setFrame(QImage(frame->pixels.data(), frame->width, frame->height, frame->stride, QImage::Format_RGB888).copy(),
                    frame->sequence, frame->capturedUnixUs);
            } else video_->clearFrame();
            lastFrameRevision_ = snapshot.frameRevision;
        }
        if (snapshot.hasVideo || snapshot.hasAudio) {
            const auto tracks = snapshot.hasVideo && snapshot.hasAudio ? QStringLiteral("视频 + 音频")
                : (snapshot.hasVideo ? QStringLiteral("视频流") : QStringLiteral("音频流"));
            media_->setText(tracks + (initial_.audioPlayback.enabled ? QString() : QStringLiteral("  ·  音频输出已禁用")));
        } else media_->setText(QStringLiteral("支持 RTMP(S) / HTTP(S)-FLV")
            + (initial_.audioPlayback.enabled ? QString() : QStringLiteral("  ·  音频输出已禁用")));
        if (closing_ || !uiError_.isEmpty() || !snapshot.error.empty()) {
            error_->setText(closing_ ? QStringLiteral("正在停止播放并释放资源，完成后自动关闭窗口。")
                : (!uiError_.isEmpty() ? uiError_ : utf8(snapshot.error))); error_->show();
        } else error_->hide();
        if (logClock_.elapsed() >= 100) {
            for (const auto& line : logger_.linesSince(logCursor_)) log_->appendPlainText(utf8(line).trimmed());
            logClock_.restart();
        }
        if (closing_ && !running) close();
    }

    config::PlayerConfig initial_;
    logging::Logger& logger_;
    player::PlayerSession session_;
    bool muted_ = false, paused_ = false;
    int volume_ = 100;
    bool closing_ = false, wasMaximized_ = false;
    std::uint64_t lastFrameRevision_ = std::numeric_limits<std::uint64_t>::max(), logCursor_ = 0;
    QString uiError_;
    QByteArray previousGeometry_;
    QElapsedTimer logClock_;
    QTimer* timer_ = nullptr;
    VideoSurface* video_ = nullptr;
    QLineEdit* url_ = nullptr;
    QPushButton *connect_ = nullptr, *stop_ = nullptr, *pause_ = nullptr, *mute_ = nullptr,
        *fullscreen_ = nullptr, *toggleLogs_ = nullptr, *clearLogs_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QPushButton* toggleStats_ = nullptr;
    QWidget* statsDetails_ = nullptr;
    QLabel *statsSummary_ = nullptr, *statsSample_ = nullptr, *videoPacketQueue_ = nullptr, *audioPacketQueue_ = nullptr,
        *decodedVideoQueue_ = nullptr, *pcmQueue_ = nullptr, *droppedFrames_ = nullptr, *droppedPackets_ = nullptr,
        *syncError_ = nullptr, *endToEnd_ = nullptr, *latencyReason_ = nullptr;
    QLabel *status_ = nullptr, *error_ = nullptr, *volumeLabel_ = nullptr, *playbackTime_ = nullptr,
        *media_ = nullptr, *logHint_ = nullptr;
};
} // namespace

int runPlayerControl(const config::PlayerConfig& config,
                     const config::CommandLineOptions& options, logging::Logger& logger) {
    static_cast<void>(options);
    int argc = 1; char name[] = "player_gui"; char* argv[]{name, nullptr};
    QApplication app(argc, argv); QApplication::setStyle(QStringLiteral("Fusion"));
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setPointSizeF(9.5); QApplication::setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(11, 17, 21)); palette.setColor(QPalette::WindowText, QColor(197, 209, 213));
    palette.setColor(QPalette::Base, QColor(11, 21, 28)); palette.setColor(QPalette::Text, QColor(197, 209, 213));
    palette.setColor(QPalette::Button, QColor(20, 33, 41)); palette.setColor(QPalette::ButtonText, QColor(197, 209, 213));
    palette.setColor(QPalette::Highlight, QColor(43, 85, 73)); palette.setColor(QPalette::HighlightedText, QColor(224, 239, 233));
    QApplication::setPalette(palette);
    PlayerWindow window(config, logger); window.showConfigured(); return app.exec();
}
} // namespace lsl::ui
