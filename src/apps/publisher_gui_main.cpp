#include "common/config/CommandLine.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"
#include "publisher/ui/PublisherControl.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Publisher, argc, argv,
        [](const lsl::config::PublisherConfig& config,
           const lsl::config::CommandLineOptions& options, lsl::logging::Logger& logger) {
            if (options.listDevices || !options.deviceOptions.empty()) {
                return lsl::pipeline::runPublisher(config, options, logger);
            }
            return lsl::ui::runPublisherControl(config, options, logger);
        });
}
