#include "common/config/CommandLine.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Publisher, argc, argv, lsl::pipeline::runPublisher);
}
