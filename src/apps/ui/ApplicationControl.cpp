#include "apps/ui/ApplicationControl.hpp"
#include "apps/platform/ApiClient.hpp"
#include "apps/ui/LobbyControl.hpp"
#include "common/logger/Redaction.hpp"

#include "player/ui/PlayerControl.hpp"
#include "publisher/ui/PublisherControl.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFile>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QSessionManager>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

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

std::filesystem::path appConfigPath(const char* file) {
    const auto directory = QCoreApplication::applicationDirPath().toUtf8();
    return std::filesystem::u8path(directory.constData()) / "config" / file;
}

std::unique_ptr<logging::Logger> createLogger(const config::LoggingConfig& config) {
    auto logger = std::make_unique<logging::Logger>(logging::Options{
        config.level, config.console, std::filesystem::u8path(config.file)});
    if (!logger->log(logging::Level::Info, "config", "Configuration OK")) {
        throw error::AppError("logger", error::Io, "cannot write startup log");
    }
    return logger;
}

class ApplicationWindows final : public QObject {
public:
    ~ApplicationWindows() override {
        // Destroy the windows and join their media workers before freeing the API and loggers.
        if (mainWindow_) {
            disconnect(mainWindow_, nullptr, this, nullptr);
            delete mainWindow_.data();
        }
    }

    void openLobby() {
        const auto directory = QCoreApplication::applicationDirPath() + QStringLiteral("/config/");
        const auto localConfig = directory + QStringLiteral("platform.local.json");
        platformApi_ = std::make_unique<platform::ApiClient>(platform::loadPlatformConfig(
            QFile::exists(localConfig) ? localConfig : directory + QStringLiteral("platform.json")));
        const auto player = config::loadPlayerConfig(appConfigPath("player.json"));
        auto logger = createLogger(player.logging);
        auto* lobby = createLobbyControl(*platformApi_, player, *logger, [this] { return createPublisher(); });
        showMainWindow(lobby, std::move(logger), false);
    }

    void openDirect(config::AppKind mode, const std::vector<std::string>& arguments) {
        auto options = config::parseCommandLine(mode, arguments);
        if (mode == config::AppKind::Publisher) {
            if (!options.configFile) options.configFile = appConfigPath("publisher-stream.json");
            const auto publisher = config::loadPublisherConfig(options.configFile, options.overrides);
            auto logger = createLogger(publisher.logging);
            auto* control = createPublisherControl(publisher, *logger);
            showMainWindow(control, std::move(logger), true);
        } else {
            if (!options.configFile) options.configFile = appConfigPath("player.json");
            const auto player = config::loadPlayerConfig(options.configFile, options.overrides);
            auto logger = createLogger(player.logging);
            auto* control = createPlayerControl(player, *logger);
            showMainWindow(control, std::move(logger), true, player.video.fullscreen);
        }
    }

    bool prepareQuit() {
        quitting_ = true;
        if (!mainWindow_) return true;
        mainWindow_->close();
        return false;
    }

private:
    QWidget* createPublisher() {
        const auto publisher = config::loadPublisherConfig(appConfigPath("publisher-stream.json"));
        auto logger = createLogger(publisher.logging);
        auto* control = createPlatformPublisherControl(publisher, *logger, *platformApi_);
        publisherLogger_ = std::move(logger);
        connect(control, &QObject::destroyed, this, [this] { publisherLogger_.reset(); });
        return control;
    }

    void showMainWindow(QWidget* window, std::unique_ptr<logging::Logger> logger,
                        bool returnToLobby, bool fullscreen = false) {
        mainLogger_ = std::move(logger);
        mainWindow_ = window;
        window->setAttribute(Qt::WA_DeleteOnClose);
        connect(window, &QObject::destroyed, this, [this, returnToLobby] {
            mainWindow_.clear();
            mainLogger_.reset();
            if (quitting_ || !returnToLobby) {
                QCoreApplication::quit();
                return;
            }
            try { openLobby(); }
            catch (const std::exception& error) {
                QMessageBox::critical(nullptr, QStringLiteral("无法打开直播大厅"),
                    utf8(logging::redactSecrets(error.what())));
                QCoreApplication::exit(1);
            }
        });
        if (fullscreen) window->showFullScreen();
        else window->show();
    }

    std::unique_ptr<platform::ApiClient> platformApi_;
    std::unique_ptr<logging::Logger> mainLogger_, publisherLogger_;
    QPointer<QWidget> mainWindow_;
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
    ApplicationWindows windows;
    app.permitQuit = [&windows] { return windows.prepareQuit(); };
#ifndef QT_NO_SESSIONMANAGER
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &windows, [&windows](QSessionManager& manager) {
        // OS shutdown may still terminate the process before HTTP completes.
        // Request normal cleanup without a nested loop or cancelling OS shutdown.
        manager.setRestartHint(QSessionManager::RestartNever);
        windows.prepareQuit();
    }, Qt::DirectConnection);
#endif
    if (initialMode) windows.openDirect(*initialMode, arguments);
    else windows.openLobby();
    const int result = app.exec(); app.permitQuit = {}; return result;
}
} // namespace lsl::ui
