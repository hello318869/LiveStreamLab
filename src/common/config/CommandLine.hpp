#pragma once

#include "common/config/Config.hpp"
#include <functional>

namespace lsl::config {

enum class AppKind { Publisher, Player };

struct CommandLineOptions {
    std::optional<std::filesystem::path> configFile;
    Overrides overrides;
    bool help = false;
    bool checkConfig = false;
    bool printConfig = false;
    bool listDevices = false;
    std::string deviceOptions; // video 或 audio，仅主播端使用
};

// args 不包含程序名。未知选项、位置参数和缺少值会抛出 ConfigError。
CommandLineOptions parseCommandLine(AppKind app, const std::vector<std::string>& args);
std::string commandLineHelp(AppKind app);

// Windows 从 Unicode 命令行转换为 UTF-8，避免中文设备名或文件路径被 ANSI argv 损坏。
std::vector<std::string> commandLineArguments(int argc, char* argv[]);

using PublisherAction = std::function<int(const PublisherConfig&, const CommandLineOptions&, logging::Logger&)>;
using PlayerAction = std::function<int(const PlayerConfig&, const CommandLineOptions&, logging::Logger&)>;
// 共用配置、日志初始化与顶层异常处理；两端通过各自 action 接入媒体模块。
// args 不包含程序名和统一入口已经处理的 --mode / --cli。
int runConfigApp(AppKind app, const std::vector<std::string>& args, const PublisherAction& action = {},
                 const PlayerAction& playerAction = {});
int runConfigApp(AppKind app, int argc, char* argv[], const PublisherAction& action = {},
                 const PlayerAction& playerAction = {});

} // namespace lsl::config
