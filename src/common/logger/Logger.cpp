#include "common/logger/Logger.hpp"
#include "common/error/Error.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace lsl::logging {

std::string_view levelName(Level level) noexcept {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::optional<Level> parseLevel(std::string_view text) noexcept {
    for (auto level : {Level::Debug, Level::Info, Level::Warn, Level::Error}) {
        if (text == levelName(level)) return level;
    }
    return std::nullopt;
}

Logger::Logger(Options options) : options_(std::move(options)) {
    if (!parseLevel(levelName(options_.level))) {
        throw error::AppError("logger", error::Io, "invalid logger level");
    }
    if (!options_.file.empty()) {
        std::error_code ec;
        const auto parent = options_.file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (!ec) file_.open(options_.file, std::ios::binary | std::ios::app);
        if (ec || !file_) throw error::AppError("logger", error::Io, "cannot open log file: " + options_.file.u8string());
    }
}

bool Logger::log(Level level, std::string_view module, std::string_view message, int code) noexcept {
    if (level < options_.level || !hasOutput()) return true;
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        std::ostringstream out;
        out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << " level=" << levelName(level)
            << " module=" << module << " thread=" << std::this_thread::get_id()
            << " code=" << code << " message=";
        // 防止消息中的换行把一条日志拆成多行。
        for (char ch : message) {
            if (ch == '\n') out << "\\n";
            else if (ch == '\r') out << "\\r";
            else out << ch;
        }
        out << '\n';
        const auto line = out.str();
        if (options_.console) std::cerr << line << std::flush;
        if (file_.is_open()) file_ << line << std::flush;
        return (!options_.console || std::cerr.good()) && (!file_.is_open() || file_.good());
    } catch (...) {
        return false;
    }
}

} // namespace lsl::logging
