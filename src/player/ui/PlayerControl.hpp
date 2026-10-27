#pragma once

#include "common/config/CommandLine.hpp"

class QWidget;

namespace lsl::ui {

// Qt 主线程拥有控件与画面；网络、解码和音频由 PlayerSession 管理。
// 调用方管理窗口和 QApplication；logger 必须比窗口存活更久。
QWidget* createPlayerControl(const config::PlayerConfig& config, logging::Logger& logger);

} // namespace lsl::ui
