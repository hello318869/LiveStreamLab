#pragma once

#include "common/config/CommandLine.hpp"

#include <functional>

class QWidget;
namespace lsl::platform { class ApiClient; }

namespace lsl::ui {
// 调用方持有 API 和日志；大厅在当前播放或开播窗口完成关闭后才关闭。
QWidget* createLobbyControl(platform::ApiClient& api, const config::PlayerConfig& config,
                            logging::Logger& logger, std::function<QWidget*()> createPublisher);
} // namespace lsl::ui
