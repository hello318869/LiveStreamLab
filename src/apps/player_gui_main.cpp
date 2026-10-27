#include "common/config/CommandLine.hpp"
#include "player/ui/PlayerControl.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Player, argc, argv, {}, lsl::ui::runPlayerControl);
}
