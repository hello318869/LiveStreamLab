#include "common/config/CommandLine.hpp"

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/error.h>
}
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <thread>

namespace {
using namespace lsl::config;
using namespace lsl::logging;
using namespace lsl::error;
int checks = 0;

void require(bool ok, const std::string& name) {
    ++checks;
    if (!ok) throw std::runtime_error(name);
}

template <typename Exception = ConfigError, typename Action>
void rejects(Action action, const std::string& expected) {
    try { action(); }
    catch (const Exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    require(false, "invalid input accepted: " + expected);
}

struct Fixtures {
    std::filesystem::path dir = std::filesystem::current_path() /
        ("common-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixtures() { std::filesystem::create_directory(dir); }
    ~Fixtures() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};

void configuration() {
    require(loadPublisherConfig().video.width == 1280 && loadPlayerConfig().channels == 2, "defaults");
    const auto c = parsePublisherConfig(R"({"video":{"width":640,"fps":30},"devices":{"video":"摄像头"}})",
        {{"video.fps", "50"}, {"video.fps", "60"}, {"audio.channels", "1"}});
    require(c.video.width == 640 && c.video.height == 720 && c.video.fps == 60 &&
            c.audio.channels == 1 && c.videoDevice == u8"摄像头", "partial JSON and ordered overrides");
    require(toJson(parsePublisherConfig(toJson(c))) == toJson(c), "publisher round trip");
    const auto p = parsePlayerConfig(R"({"audio":{"device":"扬声器","sample_rate":44100},
        "logging":{"level":"WARN","console":false}})");
    require(p.audioDevice == u8"扬声器" && p.sampleRate == 44100 &&
            p.logging.level == Level::Warn && !p.logging.console, "player JSON");
    require(toJson(parsePlayerConfig(toJson(p))) == toJson(p), "player round trip");
    require(parsePublisherConfig("\xEF\xBB\xBF{}").video.fps == 25, "UTF-8 BOM");
    for (const auto& [text, field] : std::vector<std::pair<std::string, std::string>>{
        {"{", "JSON"}, {"[]", "object"}, {R"({"video":{"fpps":30}})", "video.fpps"},
        {R"({"video":null})", "object"}, {R"({"video":{"fps":"30"}})", "integer"},
        {R"({"video":{"fps":25.5}})", "integer"}, {R"({"logging":{"console":1}})", "boolean"},
        {R"({"video":{"fps":18446744073709551615}})", "int range"},
        {R"({"video":{"width":641}})", "even"}, {R"({"video":{"fps":0}})", "video.fps"},
        {R"({"audio":{"channels":3}})", "channels"}, {R"({"audio":{"sample_rate":12345}})", "sample_rate"},
        {R"({"devices":{"video":" "}})", "devices.video"}, {R"({"output":{"url":"http://host/live/a"}})", "output.url"},
        {R"({"logging":{"level":"TRACE"}})", "logging.level"}, {R"({"logging":{"file":"a\u0000.log"}})", "logging.file"}}) {
        rejects([&] { parsePublisherConfig(text); }, field);
    }
    for (const auto& value : {"25fps", "1.5", "999999999999", ""}) {
        rejects([&] { parsePublisherConfig("{}", {{"video.fps", value}}); }, "decimal integer");
    }
    rejects([] { parsePlayerConfig("{}", {{"audio", "x"}}); }, "leaf field");
    rejects([] { parsePlayerConfig("{}", {{"logging.console", "0"}}); }, "true or false");
    for (const auto& value : {"ftp://host/live/a", "rtmp:///live/a", "http://host/", "http://host/a b"}) {
        rejects([&] { parsePlayerConfig("{}", {{"input.url", value}}); }, "input.url");
    }
    const auto cli = parseCommandLine(AppKind::Publisher,
        {"--fps=30", "--set", "audio.channels=1", "--fps", "60", "--log-console=false", "--print-config"});
    const auto overridden = loadPublisherConfig(std::nullopt, cli.overrides);
    require(cli.printConfig && overridden.video.fps == 60 && overridden.audio.channels == 1 &&
            !overridden.logging.console, "CLI aliases and order");
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"--unknown"}, {"--fps"}, {"--fps", "--check-config"}, {"--help=true"}, {"--set", "=1"}}) {
        rejects([&] { parseCommandLine(AppKind::Publisher, args); }, ":");
    }
}

void loggingAndErrors(const std::filesystem::path& dir) {
    const auto path = dir / std::filesystem::u8path(u8"日志.log");
    {
        Logger log({Level::Info, false, path});
        require(log.log(Level::Debug, "config", "filtered"), "filtered log");
        require(log.log(Level::Warn, "config", "中文\nnext", 1001), "warning log");
        std::vector<std::thread> workers;
        for (int t = 0; t < 6; ++t) {
            workers.emplace_back([&, t] {
                for (int n = 0; n < 100; ++n)
                    log.log(Level::Info, "worker", "record-" + std::to_string(t * 100 + n));
            });
        }
        for (auto& worker : workers) worker.join();
    }
    { Logger log({Level::Error, false, path}); require(log.log(Level::Error, "logger", "appended", 1003), "append"); }
    std::ifstream in(path, std::ios::binary);
    std::set<std::string> records;
    std::string line;
    int lines = 0;
    while (std::getline(in, line)) {
        ++lines;
        require(line.find("Z level=") != std::string::npos &&
                line.find(" thread=") != std::string::npos && line.find(" code=") != std::string::npos,
                "complete log fields");
        const auto pos = line.find("message=record-");
        if (pos != std::string::npos) records.insert(line.substr(pos));
        if (lines == 1) require(line.find(u8"中文\\nnext") != std::string::npos, "single-line UTF-8 message");
    }
    require(lines == 602 && records.size() == 600, "600 unique concurrent logs, filter and append");
    rejects<AppError>([&] { Logger log({Level::Info, false, dir}); }, "cannot open log file");
    require(isRecoverableFFmpegError("decoder", AVERROR(EAGAIN)), "retryable EAGAIN");
    require(isRecoverableFFmpegError("network", AVERROR(ETIMEDOUT)) &&
            !isRecoverableFFmpegError("decoder", AVERROR(ETIMEDOUT)), "network context");
    require(!isRecoverableFFmpegError("decoder", AVERROR_INVALIDDATA) &&
            !isRecoverableFFmpegError("decoder", AVERROR_EOF), "fatal/control-flow errors");
    throwIfFFmpegFailed(0, "decoder", "success");

    bool released = false;
    try {
        AVDictionary* raw = nullptr;
        require(av_dict_set(&raw, "test", "value", 0) >= 0, "allocate FFmpeg dictionary");
        const auto cleanup = [&](AVDictionary* ptr) { av_dict_free(&ptr); released = true; };
        std::unique_ptr<AVDictionary, decltype(cleanup)> resource(raw, cleanup);
        throwIfFFmpegFailed(AVERROR_INVALIDDATA, "decoder", "decode");
    } catch (const FFmpegError& e) {
        require(e.module == "decoder" && e.code == AVERROR_INVALIDDATA && !e.recoverable &&
                std::string(e.what()).find(ffmpegErrorString(e.code)) != std::string::npos, "FFmpeg error metadata");
    }
    require(released, "RAII cleanup during exception");
}
} // namespace

int main() {
    try {
        Fixtures files;
        configuration();
        loggingAndErrors(files.dir);
        std::cout << checks << " checks passed (including 600 concurrent log records)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
