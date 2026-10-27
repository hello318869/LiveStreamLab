#include "common/config/CommandLine.hpp"
#include "player/pipeline/PlayerPipeline.hpp"
#include "publisher/pipeline/PublisherPipeline.hpp"

#ifdef LSL_WITH_GUI
#include "apps/ui/ApplicationControl.hpp"
#endif

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {
struct LaunchOptions {
    std::optional<lsl::config::AppKind> mode;
    bool cli = false;
    std::vector<std::string> arguments;
};

LaunchOptions parseLaunchOptions(const std::vector<std::string>& args) {
    LaunchOptions result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--cli") {
            result.cli = true;
        } else if (args[i] == "--mode" || args[i].rfind("--mode=", 0) == 0) {
            if (result.mode) throw lsl::config::ConfigError("--mode: specify one mode only");
            std::string value;
            if (args[i] == "--mode") {
                if (++i == args.size()) throw lsl::config::ConfigError("--mode: expected publisher or player");
                value = args[i];
            } else value = args[i].substr(7);
            if (value == "publisher") result.mode = lsl::config::AppKind::Publisher;
            else if (value == "player") result.mode = lsl::config::AppKind::Player;
            else throw lsl::config::ConfigError("--mode: expected publisher or player");
        } else {
            result.arguments.push_back(args[i]);
            // Preserve option values verbatim, even if they look like launcher switches.
            if (args[i].find('=') == std::string::npos && args[i] != "--help" && args[i] != "-h" &&
                args[i] != "--check-config" && args[i] != "--print-config" && args[i] != "--list-devices" &&
                i + 1 < args.size()) {
                result.arguments.push_back(args[++i]);
            }
        }
    }
    return result;
}

void printHelp() {
    std::cout << "LiveStreamLab - publish and watch live streams in one application\n"
                 "Usage: livestreamlab [--mode publisher|player] [--cli] [options]\n\n"
#ifdef LSL_WITH_GUI
                 "Launch without arguments to choose publishing or viewing.\n"
                 "--mode publisher / player opens that control window directly.\n"
                 "--cli runs the selected media pipeline in the console.\n"
#else
                 "This build has no GUI; --mode publisher or --mode player is required.\n"
                 "The selected media pipeline runs in the console.\n"
#endif
                 "--mode publisher --help / --mode player --help lists mode options.\n"
                 "--check-config, --print-config and device queries do not open a window.\n";
}

#ifdef LSL_WITH_GUI
bool queryOnly(const lsl::config::CommandLineOptions& options) {
    return options.help || options.checkConfig || options.printConfig || options.listDevices ||
           !options.deviceOptions.empty();
}
#endif
} // namespace

int main(int argc, char* argv[]) {
    try {
        const auto launch = parseLaunchOptions(lsl::config::commandLineArguments(argc, argv));
        if (!launch.mode) {
            if (launch.arguments.size() == 1 &&
                (launch.arguments.front() == "--help" || launch.arguments.front() == "-h")) {
                printHelp(); return 0;
            }
            if (launch.cli || !launch.arguments.empty()) {
                throw lsl::config::ConfigError("specify --mode publisher or --mode player before media options");
            }
#ifdef LSL_WITH_GUI
            return lsl::ui::runApplicationControl({}, {});
#else
            printHelp(); return 2;
#endif
        }
        const auto options = lsl::config::parseCommandLine(*launch.mode, launch.arguments);
#ifdef LSL_WITH_GUI
        if (!launch.cli && !queryOnly(options)) {
            return lsl::ui::runApplicationControl(launch.mode, launch.arguments);
        }
#else
        static_cast<void>(options);
#endif
        return lsl::config::runConfigApp(*launch.mode, launch.arguments,
            lsl::pipeline::runPublisher, lsl::player::runPlayer);
    } catch (const lsl::error::AppError& error) {
        lsl::logging::Logger logger;
        logger.log(lsl::logging::Level::Error, error.module, error.what(), error.code);
        return error.module == "config" ? 2 : 1;
    } catch (const std::exception& error) {
        lsl::logging::Logger logger;
        logger.log(lsl::logging::Level::Error, "application", error.what());
        return 1;
    }
}
