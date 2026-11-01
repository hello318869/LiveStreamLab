#pragma once

#include <string>
#include <string_view>

namespace lsl::logging {

// Diagnostic output never needs URL credentials or query values.
std::string redactUrlCredentials(std::string_view url);
std::string redactSecrets(std::string_view text);
// Install before any FFmpeg work, including CLI device/config queries.
void installFFmpegLogRedaction();

} // namespace lsl::logging
