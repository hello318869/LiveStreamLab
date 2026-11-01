#include "apps/ui/ApplicationControl.hpp"
#include "apps/platform/ApiClient.hpp"
#include "apps/ui/LobbyControl.hpp"
#include "common/logger/Redaction.hpp"

#include "player/ui/PlayerControl.hpp"
#include "publisher/ui/PublisherControl.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSessionManager>
#include <QVBoxLayout>

#include <memory>
#include <functional>

namespace lsl::ui {
namespace {
QString utf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

class CoordinatedApplication final : public QApplication {
public:
    using QApplication::QApplication;
    std::function<bool()> permitQuit;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Quit && permitQuit && !permitQuit()) {
            event->ignore(); return true;
        }
        return QApplication::event(event);
    }
};

class ApplicationHome final : public QWidget {
public:
    ApplicationHome(std::optional<config::AppKind> initialMode, std::vector<std::string> arguments)
        : initialMode_(initialMode), arguments_(std::move(arguments)) {
        setWindowTitle(QStringLiteral("LiveStreamLab · 直播与观看"));
        setObjectName(QStringLiteral("applicationHome"));
        resize(900, 580); setMinimumSize(760, 500);
        setStyleSheet(QStringLiteral(R"QSS(
QWidget#applicationHome { background: #0b1115; color: #c5d1d5; }
QLabel { color: #c5d1d5; background: transparent; }
QLabel#brand { color: #85cbb1; font-size: 13px; font-weight: 600; }
QLabel#title { color: #edf4f1; font-size: 30px; font-weight: 600; }
QLabel#subtitle { color: #8da4ad; font-size: 13px; }
QFrame#card { background: #142129; border: 1px solid #2b424d; border-radius: 12px; }
QLabel#cardTitle { color: #e0efe9; font-size: 22px; font-weight: 600; }
QLabel#description { color: #9eb2ba; font-size: 12px; }
QPushButton { background: #386d59; color: #edf4f1; border: 1px solid #548d74;
    border-radius: 7px; padding: 12px; font-size: 14px; font-weight: 600; }
QPushButton:hover { background: #467e68; }
QPushButton:pressed { background: #2b5546; }
)QSS"));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(48, 40, 48, 32); layout->setSpacing(14);
        auto* brand = new QLabel(QStringLiteral("LOCAL LIVE STUDIO  /  LiveStreamLab"));
        brand->setObjectName(QStringLiteral("brand")); layout->addWidget(brand);
        layout->addSpacing(12);
        auto* title = new QLabel(QStringLiteral("今天，开播还是观看？"));
        title->setObjectName(QStringLiteral("title")); layout->addWidget(title);
        auto* subtitle = new QLabel(QStringLiteral("选择一种方式，进入你的直播工作空间。"));
        subtitle->setObjectName(QStringLiteral("subtitle")); layout->addWidget(subtitle);
        layout->addSpacing(18);
        auto* cards = new QHBoxLayout; cards->setSpacing(20);
        cards->addWidget(makeCard(QStringLiteral("我要开播"),
            QStringLiteral("分享桌面或摄像头画面\n选择麦克风或系统声音，开始直播"),
            QStringLiteral("进入开播控制台  →"), config::AppKind::Publisher));
        cards->addWidget(makeCard(QStringLiteral("我要观看"),
            QStringLiteral("浏览直播大厅，选择正在开播的主播\n支持暂停、音量调节与全屏观看"),
            QStringLiteral("进入直播大厅  →"), config::AppKind::Player));
        layout->addLayout(cards, 1); layout->addSpacing(10);
        auto* hint = new QLabel(QStringLiteral("随时返回首页切换功能，当前会话会先停止并释放资源。"));
        hint->setObjectName(QStringLiteral("subtitle")); layout->addWidget(hint);
    }

    ~ApplicationHome() override {
        // A media session keeps a reference to its logger until all workers have joined.
        if (active_) {
            disconnect(active_, nullptr, this, nullptr);
            delete active_.data();
        }
    }

    void openMode(config::AppKind mode, bool platformMode = false) {
        if (active_ || quitting_) return;
        if (platformMode && !platformApi_) {
            const auto localConfig = QCoreApplication::applicationDirPath() + QStringLiteral("/config/platform.local.json");
            platformApi_ = std::make_unique<platform::ApiClient>(platform::loadPlatformConfig(
                QFile::exists(localConfig) ? localConfig : QCoreApplication::applicationDirPath() + QStringLiteral("/config/platform.json")));
        }
        auto options = config::parseCommandLine(mode,
            !platformMode && initialMode_ == mode ? arguments_ : std::vector<std::string>{});
        if (!options.configFile) {
            const auto directory = QCoreApplication::applicationDirPath().toUtf8();
            options.configFile = std::filesystem::u8path(directory.constData()) / "config" /
                (mode == config::AppKind::Publisher ? "publisher-stream.json" : "player.json");
        }
        std::optional<config::PublisherConfig> publisher;
        std::optional<config::PlayerConfig> player;
        config::LoggingConfig logConfig;
        if (mode == config::AppKind::Publisher) {
            publisher = config::loadPublisherConfig(options.configFile, options.overrides);
            logConfig = publisher->logging;
        } else {
            player = config::loadPlayerConfig(options.configFile, options.overrides);
            logConfig = player->logging;
        }
        auto logger = std::make_unique<logging::Logger>(logging::Options{
            logConfig.level, logConfig.console, std::filesystem::u8path(logConfig.file)});
        if (!logger->log(logging::Level::Info, "config", "Configuration OK")) {
            throw error::AppError("logger", error::Io, "cannot write startup log");
        }
        QWidget* control = nullptr;
        if (platformMode) {
            control = publisher ? createPlatformPublisherControl(*publisher, *logger, *platformApi_)
                                : createLobbyControl(*platformApi_, *player, *logger);
        } else {
            control = publisher ? createPublisherControl(*publisher, *logger) : createPlayerControl(*player, *logger);
        }
        roleLogger_ = std::move(logger);
        active_ = control;
        control->setAttribute(Qt::WA_DeleteOnClose);
        connect(control, &QObject::destroyed, this, [this] {
            active_.clear(); roleLogger_.reset();
            if (quitting_) QCoreApplication::quit();
            else { show(); raise(); activateWindow(); }
        });
        hide(); control->show();
        if (player && !platformMode && player->video.fullscreen) control->showFullScreen();
    }

    bool prepareQuit() {
        quitting_ = true;
        if (active_) { active_->close(); return false; }
        return true;
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (!prepareQuit()) { event->ignore(); return; }
        event->accept(); QCoreApplication::quit();
    }

private:
    QWidget* makeCard(const QString& title, const QString& description, const QString& buttonText,
                      config::AppKind mode) {
        auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
        auto* layout = new QVBoxLayout(card); layout->setContentsMargins(24, 24, 24, 22); layout->setSpacing(16);
        auto* heading = new QLabel(title); heading->setObjectName(QStringLiteral("cardTitle")); layout->addWidget(heading);
        auto* detail = new QLabel(description); detail->setObjectName(QStringLiteral("description"));
        detail->setWordWrap(true); layout->addWidget(detail); layout->addStretch();
        auto* button = new QPushButton(buttonText); button->setCursor(Qt::PointingHandCursor);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, mode] {
            try { openMode(mode, true); }
            catch (const std::exception& error) {
                QMessageBox::critical(this, QStringLiteral("无法打开工作空间"), utf8(logging::redactSecrets(error.what())));
            }
        });
        return card;
    }

    std::optional<config::AppKind> initialMode_;
    std::vector<std::string> arguments_;
    std::unique_ptr<logging::Logger> roleLogger_;
    std::unique_ptr<platform::ApiClient> platformApi_;
    QPointer<QWidget> active_;
    bool quitting_ = false;
};

void applyTheme() {
    QApplication::setStyle(QStringLiteral("Fusion"));
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setPointSizeF(9.5); QApplication::setFont(font);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(11, 17, 21)); palette.setColor(QPalette::WindowText, QColor(197, 209, 213));
    palette.setColor(QPalette::Base, QColor(11, 21, 28)); palette.setColor(QPalette::AlternateBase, QColor(20, 33, 41));
    palette.setColor(QPalette::Text, QColor(197, 209, 213)); palette.setColor(QPalette::Button, QColor(20, 33, 41));
    palette.setColor(QPalette::ButtonText, QColor(197, 209, 213)); palette.setColor(QPalette::Highlight, QColor(43, 85, 73));
    palette.setColor(QPalette::HighlightedText, QColor(224, 239, 233)); QApplication::setPalette(palette);
}
} // namespace

int runApplicationControl(std::optional<config::AppKind> initialMode,
                          const std::vector<std::string>& arguments) {
    int argc = 1; char name[] = "livestreamlab"; char* argv[]{name, nullptr};
    CoordinatedApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("LiveStreamLab"));
    QApplication::setQuitOnLastWindowClosed(false);
    applyTheme();
    ApplicationHome home(initialMode, arguments);
    app.permitQuit = [&home] { return home.prepareQuit(); };
#ifndef QT_NO_SESSIONMANAGER
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &home, [&home](QSessionManager& manager) {
        // OS shutdown may still terminate the process before HTTP completes.
        // Request normal cleanup without a nested loop or cancelling OS shutdown.
        manager.setRestartHint(QSessionManager::RestartNever);
        home.prepareQuit();
    }, Qt::DirectConnection);
#endif
    if (initialMode) home.openMode(*initialMode);
    else home.show();
    const int result = app.exec(); app.permitQuit = {}; return result;
}
} // namespace lsl::ui
