#pragma once

#include "common/config/CommandLine.hpp"

namespace lsl::pipeline {
int runPublisher(const config::PublisherConfig& config,
                 const config::CommandLineOptions& options, logging::Logger& logger);
} // namespace lsl::pipeline
