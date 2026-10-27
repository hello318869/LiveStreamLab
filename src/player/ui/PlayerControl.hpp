#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::ui {

// Qt 主线程拥有控件与画面；网络、解码和音频由 PlayerSession 管理。
int runPlayerControl(const config::PlayerConfig& config,
                     const config::CommandLineOptions& options, logging::Logger& logger);

} // namespace lsl::ui
