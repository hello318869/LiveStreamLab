#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::player {

int runPlayer(const config::PlayerConfig& config, const config::CommandLineOptions& options,
              logging::Logger& logger);

} // namespace lsl::player
