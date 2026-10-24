#include "common/config/CommandLine.hpp"
#include "publisher/capture/CaptureApp.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Publisher, argc, argv, lsl::capture::runPublisher);
}
