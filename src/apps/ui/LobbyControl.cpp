#include "apps/ui/LobbyControl.hpp"
#include "apps/platform/ApiClient.hpp"
#include "common/logger/Redaction.hpp"
#include "player/ui/PlayerControl.hpp"

#include <QCloseEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace lsl::ui {
namespace {
QString categoryName(const QString& value) {
    if (value == QStringLiteral("gaming")) return QStringLiteral("游戏");
    if (value == QStringLiteral("music")) return QStringLiteral("音乐");
    if (value == QStringLiteral("education")) return QStringLiteral("教育");
    if (value == QStringLiteral("technology")) return QStringLiteral("科技");
    return QStringLiteral("综合");
}
QLabel* textLabel(const QString& text, const QString& name = {}) {
    auto* label = new QLabel(text); label->setTextFormat(Qt::PlainText);
    label->setObjectName(name); label->setWordWrap(true); return label;
}
bool validRoomId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[1-9][0-9]{0,19}$"));
    return pattern.match(value).hasMatch();
}
bool validSessionId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"));
    return pattern.match(value).hasMatch();
}

class LobbyWindow final : public QWidget {
public:
    LobbyWindow(platform::ApiClient& api, config::PlayerConfig config, logging::Logger& logger,
                std::function<QWidget*()> createPublisher)
        : api_(api), config_(std::move(config)), logger_(logger), createPublisher_(std::move(createPublisher)) {
        setObjectName(QStringLiteral("lobbyWindow")); setWindowTitle(QStringLiteral("LiveStreamLab · 直播大厅"));
        resize(1060, 740); setMinimumSize(820, 560);
        setStyleSheet(QStringLiteral(R"QSS(
QWidget { color: #c5d1d5; font-size: 13px; }
QWidget#lobbyWindow { background: #0b1115; }
QLabel#title { color: #edf4f1; font-size: 28px; font-weight: 600; }
QLabel#subtle { color: #8da4ad; font-size: 12px; }
QLabel#status { color: #b9d3cb; padding: 9px; background: #142129; border-radius: 6px; }
QLabel#publisherError { color: #efb8af; padding: 9px; background: #302023; border-radius: 6px; }
QFrame#roomCard { background: #142129; border: 1px solid #2b424d; border-radius: 9px; }
QLabel#cover { background: #1b3635; color: #a6d5c9; min-height: 100px; font-size: 23px; border-radius: 6px; }
QLabel#roomTitle { color: #e0efe9; font-size: 16px; font-weight: 600; }
QPushButton { background: #386d59; color: #edf4f1; border: 1px solid #548d74; border-radius: 6px; padding: 8px 15px; }
QPushButton:hover { background: #467e68; }
QPushButton:disabled { background: #20372e; color: #70837b; border-color: #344c42; }
QPushButton#startPublisher:enabled { background: #8ac7ae; color: #0e2921; border-color: #a2ddc5; font-weight: 600; }
QPushButton#startPublisher:hover:enabled { background: #a2ddc5; }
QScrollArea { border: none; background: transparent; }
QWidget#roomContainer { background: #0b1115; }
)QSS"));
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(28, 24, 28, 22); layout->setSpacing(14);
        auto* header = new QHBoxLayout; header->addWidget(textLabel(QStringLiteral("直播大厅"), QStringLiteral("title")));
        header->addStretch(); publish_ = new QPushButton(QStringLiteral("我要开播"));
        publish_->setObjectName(QStringLiteral("startPublisher")); publish_->setCursor(Qt::PointingHandCursor);
        header->addWidget(publish_); layout->addLayout(header);
        connect(publish_, &QPushButton::clicked, this, [this] { openPublisher(); });
        publisherError_ = textLabel({}, QStringLiteral("publisherError"));
        publisherError_->hide(); layout->addWidget(publisherError_);
        auto* toolbar = new QHBoxLayout;
        status_ = textLabel(QStringLiteral("正在加载直播…"), QStringLiteral("status")); toolbar->addWidget(status_, 1);
        refresh_ = new QPushButton(QStringLiteral("刷新 / 重试")); toolbar->addWidget(refresh_); layout->addLayout(toolbar);
        connect(refresh_, &QPushButton::clicked, this, [this] { refreshRooms(); });
        auto* scroll = new QScrollArea; scroll->setWidgetResizable(true);
        container_ = new QWidget; container_->setObjectName(QStringLiteral("roomContainer"));
        grid_ = new QGridLayout(container_); grid_->setContentsMargins(0, 0, 0, 0); grid_->setSpacing(16);
        grid_->setAlignment(Qt::AlignTop); for (int column = 0; column < 3; ++column) grid_->setColumnStretch(column, 1);
        scroll->setWidget(container_); layout->addWidget(scroll, 1);
        auto* pages = new QHBoxLayout; pages->addStretch();
        previous_ = new QPushButton(QStringLiteral("上一页")); pageLabel_ = textLabel(QStringLiteral("第 1 页"));
        next_ = new QPushButton(QStringLiteral("下一页")); pages->addWidget(previous_); pages->addWidget(pageLabel_); pages->addWidget(next_);
        pages->addStretch(); layout->addLayout(pages);
        connect(previous_, &QPushButton::clicked, this, [this] { if (page_ > 1) { --page_; refreshRooms(); } });
        connect(next_, &QPushButton::clicked, this, [this] {
            if (hasMore_ && page_ < std::numeric_limits<int>::max()) { ++page_; refreshRooms(); }
        });
        timer_ = new QTimer(this); timer_->setInterval(api_.config().lobbyRefreshMs);
        connect(timer_, &QTimer::timeout, this, [this] { refreshRooms(); }); updateControls();
    }
    ~LobbyWindow() override {
        api_.cancel(this);
        if (child_) { disconnect(child_, nullptr, this, nullptr); delete child_.data(); }
    }
protected:
    void showEvent(QShowEvent* event) override {
        QWidget::showEvent(event); if (!closing_) { timer_->start(); refreshRooms(); }
    }
    void hideEvent(QHideEvent* event) override {
        pauseRoomRequests();
        updateControls(); QWidget::hideEvent(event);
    }
    void closeEvent(QCloseEvent* event) override {
        if (!closing_) {
            closing_ = true;
            pauseRoomRequests();
            updateControls();
            if (child_) QTimer::singleShot(0, this, [this] { if (child_) child_->close(); });
        }
        if (child_) { event->ignore(); return; }
        event->accept();
    }
private:
    void pauseRoomRequests() {
        timer_->stop(); ++generation_; api_.cancel(this); listPending_ = playbackPending_ = false;
    }
    void updateControls() {
        const bool navigating = playbackPending_ || child_ || closing_;
        const bool busy = listPending_ || navigating;
        publish_->setEnabled(!navigating);
        refresh_->setEnabled(!busy); previous_->setEnabled(!busy && page_ > 1);
        next_->setEnabled(!busy && hasMore_ && page_ < std::numeric_limits<int>::max());
        container_->setEnabled(!busy); pageLabel_->setText(QStringLiteral("第 %1 页").arg(page_));
    }
    void openChild(QWidget* window, bool fullscreen = false) {
        window->setAttribute(Qt::WA_DeleteOnClose);
        child_ = window;
        connect(window, &QObject::destroyed, this, [this] {
            child_.clear();
            if (closing_) QTimer::singleShot(0, this, [this] { close(); });
            else { show(); raise(); activateWindow(); }
        });
        publisherError_->hide();
        hide();
        if (fullscreen) window->showFullScreen();
        else window->show();
    }
    void openPublisher() {
        if (closing_ || child_ || playbackPending_ || !isVisible()) return;
        QString message = QStringLiteral("无法打开开播控制台");
        try {
            if (auto* window = createPublisher_()) {
                openChild(window);
                return;
            }
        } catch (const std::exception& problem) {
            const auto detail = logging::redactSecrets(problem.what());
            if (!detail.empty()) message += QStringLiteral("：") + QString::fromUtf8(detail.c_str());
        }
        publisherError_->setText(message);
        publisherError_->show();
    }
    void clearCards() {
        while (auto* item = grid_->takeAt(0)) { if (item->widget()) delete item->widget(); delete item; }
    }
    void renderRooms(const QJsonArray& rooms, bool available) {
        clearCards(); int index = 0; QSet<QString> seen;
        for (const auto& entry : rooms) {
            const auto room = entry.toObject(); const auto id = room.value(QStringLiteral("room_id")).toString();
            if (!validRoomId(id) || seen.contains(id) ||
                !validSessionId(room.value(QStringLiteral("current_session_id")).toString()) ||
                room.value(QStringLiteral("status")).toString() != QStringLiteral("LIVE")) continue;
            seen.insert(id);
            auto* card = new QFrame; card->setObjectName(QStringLiteral("roomCard"));
            auto* content = new QVBoxLayout(card); content->setContentsMargins(13, 13, 13, 13); content->setSpacing(9);
            auto* cover = textLabel(QStringLiteral("LIVE\nLiveStreamLab"), QStringLiteral("cover"));
            cover->setAlignment(Qt::AlignCenter); content->addWidget(cover);
            const auto title = room.value(QStringLiteral("title")).toString();
            const auto nickname = room.value(QStringLiteral("owner")).toObject().value(QStringLiteral("nickname")).toString();
            content->addWidget(textLabel(title, QStringLiteral("roomTitle")));
            content->addWidget(textLabel(QStringLiteral("主播：") + nickname, QStringLiteral("subtle")));
            content->addWidget(textLabel(categoryName(room.value(QStringLiteral("category")).toString()) + QStringLiteral("  ·  ")
                + (available ? QStringLiteral("正在直播") : QStringLiteral("状态暂不可确认")), QStringLiteral("subtle")));
            auto* enter = new QPushButton(available ? QStringLiteral("进入直播") : QStringLiteral("等待状态恢复"));
            enter->setEnabled(available); content->addWidget(enter);
            connect(enter, &QPushButton::clicked, this, [this, id, title, nickname] { openRoom(id, title, nickname); });
            grid_->addWidget(card, index / 3, index % 3); ++index;
        }
        if (index == 0) {
            auto* empty = textLabel(available ? QStringLiteral("暂无直播，稍后刷新看看") : QStringLiteral("直播状态暂不可确认，请稍后重试"), QStringLiteral("subtle"));
            empty->setAlignment(Qt::AlignCenter); empty->setMinimumHeight(220); grid_->addWidget(empty, 0, 0, 1, 3);
        }
    }
    void refreshRooms() {
        if (closing_ || !isVisible() || child_ || listPending_ || playbackPending_) return;
        listPending_ = true; const auto generation = generation_; const int requestedPage = page_;
        status_->setText(QStringLiteral("正在刷新直播列表…")); updateControls();
        api_.request(this, "GET", QStringLiteral("/api/v1/rooms?status=live&page=%1&page_size=20").arg(page_), {}, false,
            [this, generation, requestedPage](platform::ApiResult result) {
                if (closing_ || generation != generation_ || requestedPage != page_ || !isVisible()) return;
                listPending_ = false;
                if (!result.ok()) {
                    renderRooms(cachedPage_ == requestedPage ? cachedRooms_ : QJsonArray{}, false);
                    status_->setText(QStringLiteral("直播列表加载失败：") + result.message + QStringLiteral("，请点击刷新重试"));
                    updateControls(); return;
                }
                const auto rooms = result.data.value(QStringLiteral("items"));
                if (!rooms.isArray() || !result.data.value(QStringLiteral("has_more")).isBool()) {
                    renderRooms(cachedPage_ == requestedPage ? cachedRooms_ : QJsonArray{}, false);
                    status_->setText(QStringLiteral("直播列表响应无效，请点击刷新重试")); updateControls(); return;
                }
                hasMore_ = result.data.value(QStringLiteral("has_more")).toBool();
                cachedRooms_ = rooms.toArray(); cachedPage_ = requestedPage;
                const bool available = result.meta.value(QStringLiteral("media_service_status")).toString() == QStringLiteral("AVAILABLE");
                renderRooms(rooms.toArray(), available);
                status_->setText(available ? (rooms.toArray().isEmpty() ? QStringLiteral("当前没有正在直播的房间") : QStringLiteral("已更新直播列表"))
                    : QStringLiteral("直播状态暂不可确认，列表为最后确认结果，请稍后刷新")); updateControls();
            });
    }
    void openRoom(const QString& roomId, const QString& title, const QString& nickname) {
        if (closing_ || child_ || listPending_ || playbackPending_) return;
        playbackPending_ = true; const auto generation = generation_;
        status_->setText(QStringLiteral("正在取得播放信息…")); updateControls();
        api_.request(this, "GET", QStringLiteral("/api/v1/rooms/") + roomId + QStringLiteral("/playback"), {}, false,
            [this, generation, roomId, title, nickname](platform::ApiResult result) {
                if (closing_ || generation != generation_ || !isVisible()) return;
                playbackPending_ = false;
                if (!result.ok()) {
                    status_->setText(result.code == QStringLiteral("ROOM_NOT_LIVE")
                        ? QStringLiteral("该房间当前无法观看，可能已下播或正在重连，请刷新大厅")
                        : QStringLiteral("无法进入直播：") + result.message); updateControls(); return;
                }
                const auto sessionId = result.data.value(QStringLiteral("live_session_id")).toString();
                const QUrl url(result.data.value(QStringLiteral("playback_url")).toString());
                if (result.data.value(QStringLiteral("room_id")).toString() != roomId || !validSessionId(sessionId) ||
                    result.data.value(QStringLiteral("status")).toString() != QStringLiteral("LIVE") ||
                    result.data.value(QStringLiteral("protocol")).toString() != QStringLiteral("http-flv") ||
                    result.meta.value(QStringLiteral("media_service_status")).toString() != QStringLiteral("AVAILABLE") ||
                    !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
                    (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
                    status_->setText(QStringLiteral("播放信息无效，请刷新大厅重试")); updateControls(); return;
                }
                try {
                    auto config = config_; const auto encoded = url.toString(QUrl::FullyEncoded).toUtf8();
                    config.inputUrl.assign(encoded.constData(), static_cast<std::size_t>(encoded.size()));
                    auto* window = createPlatformPlayerControl(config, logger_, RoomPlaybackContext{&api_, roomId, sessionId, title, nickname});
                    openChild(window, config.video.fullscreen);
                } catch (const std::exception&) {
                    status_->setText(QStringLiteral("无法打开播放器，请刷新后重试")); updateControls();
                }
            });
    }
    platform::ApiClient& api_;
    config::PlayerConfig config_;
    logging::Logger& logger_;
    std::function<QWidget*()> createPublisher_;
    QPointer<QWidget> child_;
    QTimer* timer_ = nullptr;
    QWidget* container_ = nullptr;
    QGridLayout* grid_ = nullptr;
    QLabel *status_ = nullptr, *pageLabel_ = nullptr, *publisherError_ = nullptr;
    QPushButton *publish_ = nullptr, *refresh_ = nullptr, *previous_ = nullptr, *next_ = nullptr;
    int page_ = 1;
    int cachedPage_ = 0;
    QJsonArray cachedRooms_;
    bool hasMore_ = false, listPending_ = false, playbackPending_ = false, closing_ = false;
    std::uint64_t generation_ = 0;
};
} // namespace

QWidget* createLobbyControl(platform::ApiClient& api, const config::PlayerConfig& config,
                            logging::Logger& logger, std::function<QWidget*()> createPublisher) {
    return new LobbyWindow(api, config, logger, std::move(createPublisher));
}
} // namespace lsl::ui
