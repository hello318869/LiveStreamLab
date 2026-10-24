#pragma once

#include "publisher/capture/MediaInput.hpp"

namespace lsl::capture::detail {
void registerDevices();
const AVInputFormat* deviceFormat(Kind kind);
std::string deviceUrl(Kind kind, const std::string& name);
} // namespace lsl::capture::detail
