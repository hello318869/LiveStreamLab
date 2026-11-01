#include "common/logger/Redaction.hpp"

extern "C" {
#include <libavutil/log.h>
}

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <regex>

namespace lsl::logging {

std::string redactUrlCredentials(std::string_view url) {
    std::string result(url);
    const auto scheme = result.find("://");
    if (scheme != std::string::npos) {
        const auto start = scheme + 3;
        const auto end = result.find_first_of("/?#", start);
        const auto at = result.rfind('@', end == std::string::npos ? result.size() : end);
        if (at != std::string::npos && at >= start) result.replace(start, at - start, "<redacted>");
    }
    const auto query = result.find_first_of("?#");
    if (query != std::string::npos) result.replace(query, result.size() - query, "?<redacted>");
    return result;
}

std::string redactSecrets(std::string_view text) {
    // Full URLs are omitted in free-form third-party messages: the source may print
    // unescaped stream credentials as part of the path rather than a query.
    static const std::regex urls(R"((?:rtmps?|https?)://[^\s<>]+)", std::regex::icase);
    static const std::regex bearer(R"(\bBearer\s+[^\s"',;}\]]+)", std::regex::icase);
    static const std::regex quotedFields(
        R"re((\b(?:access_token|publish_token|token|password|authorization|key)\b["']?\s*[:=]\s*)(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'))re",
        std::regex::icase);
    static const std::regex fields(
        R"((\b(?:access_token|publish_token|token|password|authorization|key)\b["']?\s*[:=]\s*["']?)[^\s"'&,;}\]]+)",
        std::regex::icase);
    auto result = std::regex_replace(std::string(text), urls, "<redacted-url>");
    result = std::regex_replace(result, quotedFields, "$1\"<redacted>\"");
    result = std::regex_replace(result, bearer, "Bearer <redacted>");
    return std::regex_replace(result, fields, "$1<redacted>");
}

namespace {
void safeFFmpegLog(void* context, int level, const char* format, va_list arguments) noexcept {
    if (level > av_log_get_level()) return;
    try {
        thread_local int prefix = 1;
        thread_local std::string pending;
        thread_local bool discarding = false;
        char line[8192]{};
        const int length = av_log_format_line2(context, level, format, arguments, line, sizeof(line), &prefix);
        // Never print a truncated fragment: it could divide a secret from its key.
        if (length < 0 || length >= static_cast<int>(sizeof(line))) {
            pending.clear();
            discarding = true;
            return;
        }
        pending += line;
        static std::mutex output;
        for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
            const auto complete = pending.substr(0, end + 1);
            pending.erase(0, end + 1);
            if (discarding) { discarding = false; continue; }
            const auto safe = redactSecrets(complete);
            std::lock_guard<std::mutex> lock(output);
            std::fwrite(safe.data(), 1, safe.size(), stderr);
        }
        if (pending.size() > 65536) { pending.clear(); discarding = true; }
    } catch (...) {
        // No raw fallback: memory/formatting errors must not disclose credentials.
    }
}
} // namespace

void installFFmpegLogRedaction() {
    av_log_set_callback(safeFFmpegLog);
}

} // namespace lsl::logging
