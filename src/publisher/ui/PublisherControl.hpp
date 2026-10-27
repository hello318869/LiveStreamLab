#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::ui {

// Qt 主线程拥有窗口；设备、编码器和网络由 PublisherSession 的工作线程拥有。
int runPublisherControl(const config::PublisherConfig& config,
                        const config::CommandLineOptions& options, logging::Logger& logger);

} // namespace lsl::ui
