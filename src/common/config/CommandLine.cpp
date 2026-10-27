#include "common/config/CommandLine.hpp"

#include <iostream>
#include <map>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace lsl::config {
namespace {

void reportFallback(const error::AppError& e) {
    logging::Logger fallback;
    if (!fallback.log(logging::Level::Error, e.module, e.what(), e.code)) {
        std::cerr << e.what() << '\n';
    }
}

} // namespace

CommandLineOptions parseCommandLine(AppKind app, const std::vector<std::string>& args) {
    std::map<std::string, std::string> fields = {
        {"--url", app == AppKind::Publisher ? "output.url" : "input.url"},
        {"--audio-device", app == AppKind::Publisher ? "devices.audio" : "audio.device"},
        {"--sample-rate", "audio.sample_rate"}, {"--channels", "audio.channels"},
        {"--video-queue-capacity", "pipeline.video_queue_capacity"},
        {"--max-video-age-ms", "pipeline.max_video_age_ms"},
        {"--log-level", "logging.level"}, {"--log-file", "logging.file"},
        {"--log-console", "logging.console"}};
    if (app == AppKind::Publisher) {
        fields.insert({{"--video-device", "devices.video"}, {"--width", "video.width"},
                       {"--height", "video.height"}, {"--fps", "video.fps"},
                       {"--video-file", "output.video_file"},
                       {"--audio-file", "output.audio_file"},
                       {"--stream", "output.streaming"},
                       {"--connect-timeout", "output.connect_timeout_ms"},
                       {"--write-timeout", "output.write_timeout_ms"},
                       {"--reconnect-attempts", "output.reconnect_attempts"},
                       {"--reconnect-delay", "output.reconnect_delay_ms"},
                       {"--reconnect-max-delay", "output.reconnect_max_delay_ms"},
                       {"--inactivity-timeout", "output.inactivity_timeout_ms"},
                       {"--mux-queue-capacity", "output.queue_capacity"},
                       {"--audio-queue-capacity", "pipeline.audio_queue_capacity"},
                       {"--output-queue-capacity", "pipeline.packet_queue_capacity"},
                       {"--video-bitrate", "video.bitrate"}, {"--audio-bitrate", "audio.bitrate"},
                       {"--gop-size", "video_encoder.gop_size"}, {"--preset", "video_encoder.preset"},
                       {"--low-latency", "video_encoder.low_latency"},
                       {"--source", "capture.source"}, {"--input-file", "capture.file"},
                       {"--video-source", "capture.video_source"},
                       {"--audio-source", "capture.audio_source"},
                       {"--loop", "capture.loop"}, {"--realtime", "capture.realtime"},
                       {"--capture-video", "capture.video"}, {"--capture-audio", "capture.audio"},
                       {"--capture-timeout", "capture.timeout_ms"}, {"--video-format", "capture.video_format"},
                       {"--scale-algorithm", "preprocess.video.scale_algorithm"},
                       {"--resize-mode", "preprocess.video.resize_mode"},
                       {"--rotation", "preprocess.video.rotation"}, {"--mirror", "preprocess.video.mirror"},
                       {"--watermark", "preprocess.video.watermark"},
                       {"--timestamp-overlay", "preprocess.video.timestamp_overlay"},
                       {"--audio-sample-format", "preprocess.audio.sample_format"},
                       {"--audio-gain-db", "preprocess.audio.gain_db"},
                       {"--audio-frame-size", "preprocess.audio.frame_size"}});
    } else {
        fields.insert({{"--width", "video.width"}, {"--height", "video.height"},
                       {"--vsync", "video.vsync"}, {"--fullscreen", "video.fullscreen"},
                       {"--play-audio", "audio.enabled"}, {"--audio-buffer-ms", "audio.buffer_ms"},
                       {"--volume", "audio.volume"}, {"--mute", "audio.muted"},
                       {"--audio-paused", "audio.paused"},
                       {"--connect-timeout", "input.connect_timeout_ms"},
                       {"--read-timeout", "input.read_timeout_ms"},
                       {"--reconnect-attempts", "input.reconnect_attempts"},
                       {"--reconnect-delay", "input.reconnect_delay_ms"},
                       {"--reconnect-max-delay", "input.reconnect_max_delay_ms"},
                       {"--inactivity-timeout", "input.inactivity_timeout_ms"},
                       {"--packet-queue-capacity", "input.queue_capacity"},
                       {"--max-buffer-ms", "input.max_buffer_ms"},
                       {"--max-queue-bytes", "input.max_queue_bytes"}});
    }
    CommandLineOptions result;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto equals = args[i].find('=');
        const auto option = args[i].substr(0, equals);
        const bool inlineValue = equals != std::string::npos;
        if (option == "--help" || option == "-h" || option == "--check-config" || option == "--print-config" ||
            (app == AppKind::Publisher && option == "--list-devices")) {
            if (inlineValue) {
                throw ConfigError(option + ": does not take a value");
            }
            if (option == "--help" || option == "-h") result.help = true;
            if (option == "--check-config") result.checkConfig = true;
            if (option == "--print-config") result.printConfig = true;
            if (option == "--list-devices") result.listDevices = true;
            continue;
        }
        const auto field = fields.find(option);
        const bool listOptions = app == AppKind::Publisher && option == "--list-options";
        if (option != "--config" && option != "--set" && !listOptions && field == fields.end()) {
            throw ConfigError(option + ": unknown option (use --help)");
        }
        std::string value;
        if (inlineValue) {
            value = args[i].substr(equals + 1);
        } else {
            if (i + 1 == args.size() || args[i + 1].rfind("--", 0) == 0 || args[i + 1] == "-h") {
                throw ConfigError(option + ": missing value");
            }
            value = args[++i];
        }
        if (value.empty()) {
            throw ConfigError(option + ": empty value");
        }
        if (listOptions) {
            if (value != "video" && value != "audio") throw ConfigError("--list-options: expected video or audio");
            result.deviceOptions = value;
        } else if (option == "--config") {
            if (result.configFile) {
                throw ConfigError("--config: specify only one configuration file");
            }
            result.configFile = std::filesystem::u8path(value);
        } else if (option == "--set") {
            const auto split = value.find('=');
            if (split == std::string::npos || split == 0) {
                throw ConfigError("--set: expected field=value");
            }
            result.overrides.emplace_back(value.substr(0, split), value.substr(split + 1));
        } else {
            result.overrides.emplace_back(field->second, value);
        }
    }
    if (result.listDevices && !result.deviceOptions.empty()) {
        throw ConfigError("choose --list-devices or --list-options");
    }
    if ((result.listDevices || !result.deviceOptions.empty()) && (result.printConfig || result.checkConfig)) {
        throw ConfigError("device listing cannot be combined with config actions");
    }
    return result;
}

std::string commandLineHelp(AppKind app) {
    const bool publisher = app == AppKind::Publisher;
    std::string help = std::string("Usage: livestreamlab --mode ") + (publisher ? "publisher" : "player") +
        " [--cli] [options]\n\n"
        "  --mode publisher|player  Open the publishing or viewing controls\n"
        "  --cli                    Run the selected media pipeline without Qt controls\n"
        "  --help, -h               Show this help\n"
        "  --config PATH            Load a UTF-8 JSON file (otherwise use defaults)\n"
        "  --set FIELD=VALUE        Override a JSON leaf, e.g. audio.channels=1\n"
        "  --url URL                Override the stream URL\n"
        "  --audio-device NAME      Audio device name (default: default)\n"
        "  --sample-rate HZ         Audio sample rate\n"
        "  --channels N             1 (mono) or 2 (stereo)\n"
        "  --video-queue-capacity N  Raw publisher / decoded player video frames (1..64)\n"
        "  --max-video-age-ms MS    Queued realtime video age limit (20..5000)\n"
        "  --check-config           Validate and exit\n"
        "  --print-config           Print effective JSON and exit\n";
    help += "  --log-level LEVEL        DEBUG, INFO, WARN or ERROR\n"
            "  --log-file PATH          Append UTF-8 logs to a file\n"
            "  --log-console BOOL       true or false (console logs go to stderr)\n";
    if (publisher) {
        help += "  --video-device NAME      Camera name (used with --video-source camera)\n"
                "  --width N --height N     Even output dimensions; also requested from cameras\n"
                "  --fps N                  Video frame rate\n"
                "  --video-bitrate BPS      Video bitrate in bit/s\n"
                "  --video-file PATH        M05 video-only MP4 (default: output/video.mp4)\n"
                "  --gop-size N             Keyframe interval in frames (1..1200)\n"
                "  --preset NAME            x264 preset (default: veryfast)\n"
                "  --low-latency BOOL       Disable B frames and tune for low latency\n"
                "  --audio-bitrate BPS      AAC bitrate in bit/s\n"
                "  --audio-file PATH        M06 audio-only M4A (default: output/audio.m4a)\n"
                "  --stream BOOL            Enable M07 FLV/RTMP output to --url (default: false)\n"
                "  --connect-timeout MS     RTMP connection timeout (100..60000)\n"
                "  --write-timeout MS       RTMP write timeout (100..60000)\n"
                "  --reconnect-attempts N   Retry 0..100 times; 0 disables, -1 retries until stopped\n"
                "  --reconnect-delay MS     Delay between connection attempts (0..60000)\n"
                "  --reconnect-max-delay MS Exponential retry delay cap (0..60000; >= base delay)\n"
                "  --inactivity-timeout MS  No sent media timeout (100..600000; 0 disables)\n"
                "  --mux-queue-capacity N   Maximum queued encoded packets (8..4096)\n"
                "  --audio-queue-capacity N Raw captured audio blocks before encoding (1..256)\n"
                "  --output-queue-capacity N Encoded events before local muxing (8..4096)\n"
                "  --list-devices           Enumerate cameras and microphones\n"
                "  --list-options KIND      Print camera (video) / microphone (audio) capabilities\n"
                "  --source devices|file    Select live devices or a local file\n"
                "  --video-source MODE      devices only: desktop (default, Windows) or camera\n"
                "                           desktop captures the full virtual desktop with cursor\n"
                "                           camera: select input with --video-device NAME\n"
                "  --audio-source MODE      devices only: microphone (default) or system\n"
                "                           microphone: select input with --audio-device NAME\n"
                "                           system: Windows default output loopback; no mixing\n"
                "  --input-file PATH        Local media file (also set --source file)\n"
                "  --loop BOOL              Loop the file, true or false\n"
                "  --realtime BOOL          Pace file frames by timestamps\n"
                "  --capture-video BOOL     Enable video capture/track\n"
                "  --capture-audio BOOL     Enable audio capture/track\n"
                "  --capture-timeout MS     Device I/O timeout (100..60000)\n"
                "  --video-format FORMAT    Camera pixel format/codec, e.g. mjpeg\n"
                "  --scale-algorithm NAME   nearest, bilinear, bicubic or lanczos\n"
                "  --resize-mode MODE       fit (letterbox), crop or stretch\n"
                "  --rotation DEGREES       Clockwise 0, 90, 180 or 270\n"
                "  --mirror BOOL            Mirror horizontally after rotation\n"
                "  --watermark TEXT         Up to 80 printable ASCII characters\n"
                "  --timestamp-overlay BOOL Overlay elapsed live time\n"
                "  --audio-sample-format FMT PCM format; AAC encoder format takes priority\n"
                "  --audio-gain-db DB       Software PCM gain (-60..30 dB; default: 0)\n"
                "  --audio-frame-size N     PCM frame size; AAC encoder value takes priority\n";
    } else {
        help += "  --width N --height N     Initial window size (default: 1280x720)\n"
                "  --vsync BOOL            Request vertical sync (default: true)\n"
                "  --fullscreen BOOL       Start fullscreen (default: false)\n"
                "  --play-audio BOOL       Decode and play audio (default: true)\n"
                "  --audio-buffer-ms MS    PCM ring capacity (100..2000; default: 100)\n"
                "  --volume N              Player audio volume (0..100; default: 100)\n"
                "  --mute BOOL             Mute audio (default: false)\n"
                "  --audio-paused BOOL     Start with audio/video paused (default: false)\n"
                "  --connect-timeout MS     Input connection/probe timeout (100..60000)\n"
                "  --read-timeout MS        Input packet read timeout (100..60000)\n"
                "  --reconnect-attempts N   Retry 0..100 times; 0 disables, -1 retries until stopped\n"
                "  --reconnect-delay MS     Delay between connection attempts (0..60000)\n"
                "  --reconnect-max-delay MS Exponential retry delay cap (0..60000; >= base delay)\n"
                "  --inactivity-timeout MS  No valid received media timeout (100..600000; 0 disables)\n"
                "  --packet-queue-capacity N Maximum packets in each audio/video queue (8..4096)\n"
                "  --max-buffer-ms MS       Maximum queued media span/residence time (100..60000)\n"
                "  --max-queue-bytes N      Total queued media bytes (1048576..268435456)\n";
    }
    help += "\nPrecedence: defaults < JSON file < command line; last override wins.\n"
            "Options accept --name value and --name=value.\n"
            "Without --mode, livestreamlab opens the publishing/viewing selection window.\n"
            "The selected mode opens Qt controls by default; add --cli for command-line operation.\n";
    if (publisher) {
        help += "With --cli, publisher captures, preprocesses and encodes H.264/AAC until EOF or Ctrl+C (M02-M07).\n"
                "M05 writes video-only MP4; M06 writes audio-only M4A; --stream true enables M07 RTMP output.\n"
                "Use --set output.video_file= or --set output.audio_file= to disable local output.\n"
                "Use config/publisher-stream.json for RTMP output without separate local recordings.\n"
                "Streaming a local file requires --realtime true.\n";
    } else {
        help += "Player receives RTMP/RTMPS or HTTP/HTTPS-FLV with audio/video synchronization (M09-M15).\n"
                "CLI window: F / Alt+Enter / double click fullscreen; Escape / close / Ctrl+C stop.\n"
                "CLI window: Space pauses audio/video; M mutes; Up/Down changes volume by 10%.\n"
                "The default viewer provides Qt Widgets controls (M16.2); see docs/player-control.md.\n"
                "In Qt controls, Escape leaves fullscreen; use Stop or close the window to stop playback.\n";
    }
    return help;
}

std::vector<std::string> commandLineArguments(int argc, char* argv[]) {
    std::vector<std::string> args;
#ifdef _WIN32
    static_cast<void>(argc);
    static_cast<void>(argv);
    int count = 0;
    auto* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) {
        throw ConfigError("cannot read Windows command line");
    }
    const auto freeArgs = [](wchar_t** p) { LocalFree(p); };
    const std::unique_ptr<wchar_t*, decltype(freeArgs)> owner(wide, freeArgs);
    for (int i = 1; i < count; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            throw ConfigError("command line contains invalid Unicode");
        }
        std::string text(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, text.data(), bytes, nullptr, nullptr) == 0) {
            throw ConfigError("cannot convert command line to UTF-8");
        }
        text.pop_back();
        args.push_back(std::move(text));
    }
#else
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
#endif
    return args;
}

int runConfigApp(AppKind app, int argc, char* argv[], const PublisherAction& action,
                 const PlayerAction& playerAction) {
    try {
        return runConfigApp(app, commandLineArguments(argc, argv), action, playerAction);
    } catch (const error::AppError& e) {
        reportFallback(e);
        return e.module == "config" ? 2 : 1;
    } catch (const std::exception& e) {
        reportFallback(error::AppError("application", error::Unexpected, e.what()));
        return 1;
    }
}

int runConfigApp(AppKind app, const std::vector<std::string>& args, const PublisherAction& action,
                 const PlayerAction& playerAction) {
    std::unique_ptr<logging::Logger> logger;
    const auto report = [&](const error::AppError& e) {
        if (logger && logger->hasOutput() &&
            logger->log(logging::Level::Error, e.module, e.what(), e.code)) return;
        reportFallback(e);
    };
    try {
        const auto options = parseCommandLine(app, args);
        if (options.help) {
            std::cout << commandLineHelp(app);
            return 0;
        }
        LoggingConfig logConfig;
        std::string json;
        std::optional<PublisherConfig> publisherConfig;
        std::optional<PlayerConfig> playerConfig;
        if (app == AppKind::Publisher) {
            publisherConfig = loadPublisherConfig(options.configFile, options.overrides);
            logConfig = publisherConfig->logging;
            json = toJson(*publisherConfig);
        } else {
            playerConfig = loadPlayerConfig(options.configFile, options.overrides);
            logConfig = playerConfig->logging;
            json = toJson(*playerConfig);
        }
        logger = std::make_unique<logging::Logger>(logging::Options{
            logConfig.level, logConfig.console, std::filesystem::u8path(logConfig.file)});
        if (!logger->log(logging::Level::Debug, "application",
                         app == AppKind::Publisher ? "publisher startup" : "player startup") ||
            !logger->log(logging::Level::Info, "config", "Configuration OK")) {
            throw error::AppError("logger", error::Io, "cannot write startup log");
        }
        if (options.printConfig) std::cout << json << '\n';
        else if (options.checkConfig) std::cout << "Configuration OK\n";
        else if (publisherConfig && action) return action(*publisherConfig, options, *logger);
        else if (playerConfig && playerAction) return playerAction(*playerConfig, options, *logger);
        else throw error::AppError("pipeline", error::NotImplemented,
            "media pipelines are pending. Use --check-config or --print-config.");
        return 0;
    } catch (const error::AppError& e) {
        report(e);
        return e.module == "config" ? 2 : 1;
    } catch (const std::exception& e) {
        report(error::AppError("application", error::Unexpected, e.what()));
        return 1;
    }
}

} // namespace lsl::config
