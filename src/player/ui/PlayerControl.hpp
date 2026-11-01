#pragma once

#include "common/config/CommandLine.hpp"
#include <QString>

class QWidget;
namespace lsl::platform { class ApiClient; }

namespace lsl::ui {

// Qt 主线程拥有控件与画面；网络、解码和音频由 PlayerSession 管理。
// 调用方管理窗口和 QApplication；logger 必须比窗口存活更久。
QWidget* createPlayerControl(const config::PlayerConfig& config, logging::Logger& logger);

struct RoomPlaybackContext {
    platform::ApiClient* api = nullptr;
    QString roomId;
    QString liveSessionId;
    QString title;
    QString nickname;
};

// 已由大厅取得新鲜播放信息；主线程构造，并在事件循环中自动开始播放。
QWidget* createPlatformPlayerControl(const config::PlayerConfig& config, logging::Logger& logger,
                                     RoomPlaybackContext context);

} // namespace lsl::ui
