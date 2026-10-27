#pragma once

#include <filesystem>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

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
    // GUI 以游标轮询有界日志副本；默认禁用，不在媒体线程调用 UI 回调。
    void enableMemoryLog(std::size_t maxLines = 512);
    std::vector<std::string> linesSince(std::uint64_t& cursor) const;
private:
    Options options_;
    std::ofstream file_;
    mutable std::mutex mutex_;
    std::size_t memoryLimit_ = 0;
    std::uint64_t nextLine_ = 1;
    std::deque<std::pair<std::uint64_t, std::string>> recent_;
};

} // namespace lsl::logging
