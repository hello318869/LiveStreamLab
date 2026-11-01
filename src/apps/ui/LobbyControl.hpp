#pragma once

#include "common/config/CommandLine.hpp"

class QWidget;
namespace lsl::platform { class ApiClient; }

namespace lsl::ui {
// 调用方持有 API/日志；大厅在当前播放器完成停止后才关闭。
QWidget* createLobbyControl(platform::ApiClient& api, const config::PlayerConfig& config,
                            logging::Logger& logger);
} // namespace lsl::ui
