#include "common/error/Error.hpp"

extern "C" {
#include <libavutil/error.h>
}
#include <cerrno>

namespace lsl::error {

std::string ffmpegErrorString(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

bool isRecoverableFFmpegError(std::string_view module, int code) noexcept {
    if (code == AVERROR(EAGAIN) || code == AVERROR(EINTR)) return true;
    return module == "network" && (code == AVERROR(ETIMEDOUT) ||
        code == AVERROR(ECONNRESET) || code == AVERROR(ECONNREFUSED) ||
        code == AVERROR(EPIPE) || code == AVERROR_HTTP_SERVER_ERROR);
}

FFmpegError::FFmpegError(std::string module, int code, std::string_view operation)
    : AppError(module, code, std::string(operation) + ": " + ffmpegErrorString(code),
               isRecoverableFFmpegError(module, code)) {
    if (code >= 0) throw std::invalid_argument("FFmpegError requires a negative error code");
}

void throwIfFFmpegFailed(int result, std::string_view module, std::string_view operation) {
    if (result < 0) throw FFmpegError(std::string(module), result, operation);
}

} // namespace lsl::error
