#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::capture {
int runPublisher(const config::PublisherConfig& config,
                 const config::CommandLineOptions& options, logging::Logger& logger);
} // namespace lsl::capture
