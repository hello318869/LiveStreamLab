#pragma once

#include "common/config/CommandLine.hpp"

class QWidget;

namespace lsl::ui {

// Qt 主线程拥有窗口；设备、编码器和网络由 PublisherSession 的工作线程拥有。
// 调用方管理窗口和 QApplication；logger 必须比窗口存活更久。
QWidget* createPublisherControl(const config::PublisherConfig& config, logging::Logger& logger);

} // namespace lsl::ui
