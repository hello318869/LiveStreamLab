#pragma once

#include "common/config/CommandLine.hpp"

class QWidget;

namespace lsl::platform { class ApiClient; }

namespace lsl::ui {

// Qt 主线程拥有窗口；设备、编码器和网络由 PublisherSession 的工作线程拥有。
// 调用方管理窗口和 QApplication；logger 必须比窗口存活更久。
QWidget* createPublisherControl(const config::PublisherConfig& config, logging::Logger& logger);

// 平台入口在同一窗口中完成登录、个人房间设置与开播协调。
QWidget* createPlatformPublisherControl(const config::PublisherConfig& config, logging::Logger& logger,
                                       platform::ApiClient& api);

} // namespace lsl::ui
