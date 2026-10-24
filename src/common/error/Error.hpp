#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace lsl::error {

enum Code { InvalidConfig = 1001, Io = 1003, NotImplemented = 1004, Unexpected = 1005 };

// 模块名直接使用字符串；具体模块实现后再按需增加异常类型。
class AppError : public std::runtime_error {
public:
    AppError(std::string module, int code, std::string message, bool recoverable = false)
        : std::runtime_error(std::move(message)), module(std::move(module)),
          code(code), recoverable(recoverable) {}
    const std::string module;
    const int code;
    const bool recoverable;
};

class ConfigError final : public AppError {
public:
    explicit ConfigError(std::string message)
        : AppError("config", InvalidConfig, std::move(message)) {}
};

std::string ffmpegErrorString(int code);
bool isRecoverableFFmpegError(std::string_view module, int code) noexcept;

class FFmpegError final : public AppError {
public:
    FFmpegError(std::string module, int code, std::string_view operation);
};

// send/receive 的 EAGAIN、EOF 等正常控制流程应先在调用点处理。
void throwIfFFmpegFailed(int result, std::string_view module, std::string_view operation);

} // namespace lsl::error
