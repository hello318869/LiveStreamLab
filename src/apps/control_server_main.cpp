#include "control_server/Server.hpp"
#include "control_server/config/ServerConfig.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: control_server --config <server.json>\n";
        return 0;
    }
    if (argc != 3 || std::string(argv[1]) != "--config" || std::string(argv[2]).empty()) {
        std::cerr << "Usage: control_server --config <server.json>\n";
        return 2;
    }
    lsl::server::ServerConfig config;
    try {
        config = lsl::server::ServerConfig::load(argv[2]);
    } catch (const std::exception& error) {
        // ServerConfig emits only fixed field names, never input values.
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    }
    try {
        lsl::server::runServer(config);
    } catch (const std::exception&) {
        std::cerr << "Control server startup failed\n";
        return 1;
    }
    return 0;
}
