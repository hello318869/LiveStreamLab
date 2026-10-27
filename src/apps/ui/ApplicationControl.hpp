#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::ui {

// The application owns one Qt event loop for the home screen and both media modes.
int runApplicationControl(std::optional<config::AppKind> initialMode,
                          const std::vector<std::string>& arguments);

} // namespace lsl::ui
