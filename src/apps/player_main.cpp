#include "common/config/CommandLine.hpp"

int main(int argc, char* argv[]) {
    return lsl::config::runConfigApp(lsl::config::AppKind::Player, argc, argv);
}
