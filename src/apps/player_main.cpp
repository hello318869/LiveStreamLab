#include "common/config/CommandLine.hpp"
#include "player/pipeline/PlayerPipeline.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Player, argc, argv, {}, lsl::player::runPlayer);
}
