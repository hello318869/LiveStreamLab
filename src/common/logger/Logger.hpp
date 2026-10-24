#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>

namespace lsl::logging {

enum class Level { Debug, Info, Warn, Error };
std::string_view levelName(Level level) noexcept;
std::optional<Level> parseLevel(std::string_view text) noexcept;

struct Options {
    Level level = Level::Info;
    bool console = true; // 写 stderr，stdout 留给 JSON/命令结果。
    std::filesystem::path file; // 空路径禁用文件，非空路径追加写入。
};

class Logger {
public:
    explicit Logger(Options options = {});
    bool hasOutput() const noexcept { return options_.console || file_.is_open(); }
    // 文件由 ofstream 自动关闭；返回 false 表示输出失败。
    bool log(Level level, std::string_view module, std::string_view message, int code = 0) noexcept;
private:
    Options options_;
    std::ofstream file_;
    std::mutex mutex_;
};

} // namespace lsl::logging
