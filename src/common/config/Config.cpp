#include "common/config/Config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iterator>

namespace lsl::config {
namespace {

using Json = nlohmann::json;

[[noreturn]] void fail(const std::string& field, const std::string& message) {
    throw ConfigError(field + ": " + message);
}

Json document(const LoggingConfig& c) {
    return {{"level", logging::levelName(c.level)}, {"console", c.console}, {"file", c.file}};
}

LoggingConfig loggingConfig(const Json& data) {
    const auto level = logging::parseLevel(data.at("level").get<std::string>());
    if (!level) fail("logging.level", "expected DEBUG, INFO, WARN or ERROR");
    return {*level, data.at("console").get<bool>(), data.at("file").get<std::string>()};
}

void validateLogging(const LoggingConfig& c) {
    if (!logging::parseLevel(logging::levelName(c.level))) {
        fail("logging.level", "expected DEBUG, INFO, WARN or ERROR");
    }
    if (std::any_of(c.file.begin(), c.file.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
        fail("logging.file", "path must not contain control characters");
    }
}

Json document(const PublisherConfig& c) {
    return {{"devices", {{"video", c.videoDevice}, {"audio", c.audioDevice}}},
            {"output", {{"url", c.outputUrl}, {"video_file", c.videoFile}, {"audio_file", c.audioFile},
                        {"streaming", c.streaming.enabled},
                        {"connect_timeout_ms", c.streaming.connectTimeoutMs},
                        {"write_timeout_ms", c.streaming.writeTimeoutMs},
                        {"reconnect_attempts", c.streaming.reconnectAttempts},
                        {"reconnect_delay_ms", c.streaming.reconnectDelayMs},
                        {"reconnect_max_delay_ms", c.streaming.reconnectMaxDelayMs},
                        {"inactivity_timeout_ms", c.streaming.inactivityTimeoutMs},
                        {"queue_capacity", c.streaming.queueCapacity},
                        {"max_interleave_delta_ms", c.streaming.maxInterleaveDeltaMs}}},
            {"video", {{"width", c.video.width}, {"height", c.video.height},
                       {"fps", c.video.fps}, {"bitrate", c.video.bitrate}}},
            {"video_encoder", {{"gop_size", c.videoEncoder.gopSize},
                               {"preset", c.videoEncoder.preset},
                               {"low_latency", c.videoEncoder.lowLatency}}},
            {"audio", {{"sample_rate", c.audio.sampleRate}, {"channels", c.audio.channels},
                       {"bitrate", c.audio.bitrate}}},
            {"capture", {{"source", c.capture.source}, {"video_source", c.capture.videoSource},
                         {"audio_source", c.capture.audioSource},
                         {"file", c.capture.file},
                         {"loop", c.capture.loop}, {"realtime", c.capture.realtime},
                         {"video", c.capture.video}, {"audio", c.capture.audio},
                         {"timeout_ms", c.capture.timeoutMs}, {"video_format", c.capture.videoFormat}}},
            {"preprocess", {
                {"video", {{"scale_algorithm", c.preprocess.video.scaleAlgorithm},
                           {"resize_mode", c.preprocess.video.resizeMode},
                           {"rotation", c.preprocess.video.rotation}, {"mirror", c.preprocess.video.mirror},
                           {"watermark", c.preprocess.video.watermark},
                           {"timestamp_overlay", c.preprocess.video.timestampOverlay}}},
                {"audio", {{"sample_format", c.preprocess.audio.sampleFormat},
                           {"frame_size", c.preprocess.audio.frameSize},
                           {"gain_db", c.preprocess.audio.gainDb},
                           {"discontinuity_threshold_ms", c.preprocess.audio.discontinuityThresholdMs}}}}},
            {"pipeline", {{"video_queue_capacity", c.pipeline.videoQueueCapacity},
                          {"audio_queue_capacity", c.pipeline.audioQueueCapacity},
                          {"packet_queue_capacity", c.pipeline.packetQueueCapacity},
                          {"max_video_age_ms", c.pipeline.maxVideoAgeMs}}},
            {"logging", document(c.logging)}};
}

Json document(const PlayerConfig& c) {
    return {{"input", {{"url", c.inputUrl},
                        {"connect_timeout_ms", c.demux.connectTimeoutMs},
                        {"read_timeout_ms", c.demux.readTimeoutMs},
                        {"reconnect_attempts", c.demux.reconnectAttempts},
                        {"reconnect_delay_ms", c.demux.reconnectDelayMs},
                        {"reconnect_max_delay_ms", c.demux.reconnectMaxDelayMs},
                        {"inactivity_timeout_ms", c.demux.inactivityTimeoutMs},
                        {"queue_capacity", c.demux.queueCapacity},
                        {"max_buffer_ms", c.demux.maxBufferMs},
                        {"max_queue_bytes", c.demux.maxQueueBytes},
                        {"discontinuity_threshold_ms", c.demux.discontinuityThresholdMs}}},
            {"audio", {{"device", c.audioDevice}, {"sample_rate", c.sampleRate},
                       {"channels", c.channels}, {"enabled", c.audioPlayback.enabled},
                       {"buffer_ms", c.audioPlayback.bufferMs}, {"volume", c.audioPlayback.volume},
                       {"muted", c.audioPlayback.muted}, {"paused", c.audioPlayback.paused}}},
            {"video", {{"width", c.video.width}, {"height", c.video.height},
                       {"vsync", c.video.vsync}, {"fullscreen", c.video.fullscreen}}},
            {"sync", {{"tolerance_ms", c.sync.toleranceMs},
                      {"drop_threshold_ms", c.sync.dropThresholdMs},
                      {"audio_wait_ms", c.sync.audioWaitMs},
                      {"discontinuity_threshold_ms", c.sync.discontinuityThresholdMs}}},
            {"pipeline", {{"video_queue_capacity", c.pipeline.videoQueueCapacity},
                          {"max_video_age_ms", c.pipeline.maxVideoAgeMs}}},
            {"logging", document(c.logging)}};
}

Json parse(std::string_view text) {
    try {
        return Json::parse(text.begin(), text.end());
    } catch (const Json::exception& e) {
        throw ConfigError(std::string("JSON: ") + e.what());
    }
}

void merge(Json& base, const Json& patch, const std::string& path = "") {
    if (!patch.is_object()) {
        fail(path.empty() ? "JSON" : path, "expected an object");
    }
    for (const auto& item : patch.items()) {
        const auto field = path.empty() ? item.key() : path + "." + item.key();
        auto found = base.find(item.key());
        if (found == base.end()) {
            fail(field, "unknown field");
        }
        if (found->is_object()) {
            merge(*found, item.value(), field);
        } else {
            const bool validType = found->is_string() ? item.value().is_string()
                : found->is_boolean() ? item.value().is_boolean() : item.value().is_number_integer();
            if (!validType) {
                fail(field, found->is_string() ? "expected a string" :
                            found->is_boolean() ? "expected a boolean" : "expected an integer");
            }
            *found = item.value();
        }
    }
}

int decimal(std::string_view text, const std::string& field) {
    int result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        fail(field, "expected a decimal integer in the int range");
    }
    return result;
}

void applyOverrides(Json& data, const Overrides& overrides) {
    for (const auto& [path, text] : overrides) {
        Json* node = &data;
        std::size_t start = 0;
        for (;;) {
            const auto dot = path.find('.', start);
            const auto part = path.substr(start, dot == std::string::npos ? dot : dot - start);
            if (part.empty() || !node->is_object() || !node->contains(part)) {
                fail(path, "unknown field");
            }
            node = &node->at(part);
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        if (node->is_string()) {
            *node = text;
        } else if (node->is_boolean()) {
            if (text != "true" && text != "false") fail(path, "expected true or false");
            *node = text == "true";
        } else if (node->is_number_integer()) {
            *node = decimal(text, path);
        } else {
            fail(path, "override a leaf field, not an object");
        }
    }
}

int integer(const Json& value, const std::string& field) {
    return decimal(value.dump(), field); // from_chars 同时检查 int 溢出。
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) fail(path.u8string(), "cannot open configuration file");
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) fail(path.u8string(), "cannot read configuration file");
    return text;
}

void range(int value, int min, int max, const std::string& field) {
    if (value < min || value > max) {
        fail(field, "must be in [" + std::to_string(min) + ", " + std::to_string(max) + "]");
    }
}

void device(const std::string& value, const std::string& field) {
    const bool visible = std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch > 0x20;
    });
    const bool control = std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
    if (!visible || control) {
        fail(field, "must be a non-blank device name without control characters");
    }
}

void mediaFile(const std::string& value, const std::string& field, const std::string& expectedExtension) {
    if (value.empty()) return;
    device(value, field);
    try {
        const auto path = std::filesystem::u8path(value);
        const auto filename = path.filename();
        if (filename.empty() || filename == "." || filename == "..") {
            fail(field, "expected a local file path with a filename");
        }
        auto extension = path.extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension != expectedExtension) fail(field, "expected a " + expectedExtension + " file");
#ifdef _WIN32
        for (std::size_t index = 0; index < value.size(); ++index) {
            const unsigned char ch = static_cast<unsigned char>(value[index]);
            const bool driveColon = index == 1 && std::isalpha(static_cast<unsigned char>(value[0]));
            if (ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*' ||
                (ch == ':' && !driveColon)) {
                fail(field, "path contains a character unsupported by Windows");
            }
        }
#endif
    } catch (const std::filesystem::filesystem_error&) {
        fail(field, "invalid local file path");
    }
}

void audio(int sampleRate, int channels) {
    constexpr std::array<int, 13> rates = {7350, 8000, 11025, 12000, 16000, 22050, 24000,
                                         32000, 44100, 48000, 64000, 88200, 96000};
    if (std::find(rates.begin(), rates.end(), sampleRate) == rates.end()) {
        fail("audio.sample_rate", "must be a standard AAC sample rate (7350..96000 Hz)");
    }
    range(channels, 1, 2, "audio.channels");
}

void url(const std::string& value, bool publisher) {
    const std::string field = publisher ? "output.url" : "input.url";
    if (std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; })) {
        fail(field, "URL must not contain whitespace or control characters");
    }
    const auto separator = value.find("://");
    const auto scheme = value.substr(0, separator);
    if (separator == std::string::npos ||
        (scheme != "rtmp" && scheme != "rtmps" &&
         (publisher || (scheme != "http" && scheme != "https")))) {
        fail(field, publisher ? "expected rtmp:// or rtmps:// URL"
                              : "expected rtmp://, rtmps://, http:// or https:// URL");
    }
    const auto path = value.find('/', separator + 3);
    if (path == std::string::npos || path + 1 == value.size() || value[path + 1] == '?' || value[path + 1] == '#') {
        fail(field, "URL must contain a stream path");
    }
    if (path == separator + 3) fail(field, "URL must contain a host");
    // 端口、IPv6、认证信息等细节交给 FFmpeg 打开流时解析。
}

} // namespace

void validate(const PublisherConfig& c) {
    validateLogging(c.logging);
    range(c.pipeline.videoQueueCapacity, 1, 64, "pipeline.video_queue_capacity");
    range(c.pipeline.audioQueueCapacity, 1, 256, "pipeline.audio_queue_capacity");
    range(c.pipeline.packetQueueCapacity, 8, 4096, "pipeline.packet_queue_capacity");
    range(c.pipeline.maxVideoAgeMs, 20, 5000, "pipeline.max_video_age_ms");
    device(c.videoDevice, "devices.video");
    device(c.audioDevice, "devices.audio");
    url(c.outputUrl, true);
    mediaFile(c.videoFile, "output.video_file", ".mp4");
    mediaFile(c.audioFile, "output.audio_file", ".m4a");
    range(c.streaming.connectTimeoutMs, 100, 60000, "output.connect_timeout_ms");
    range(c.streaming.writeTimeoutMs, 100, 60000, "output.write_timeout_ms");
    range(c.streaming.reconnectAttempts, -1, 100, "output.reconnect_attempts");
    range(c.streaming.reconnectDelayMs, 0, 60000, "output.reconnect_delay_ms");
    range(c.streaming.reconnectMaxDelayMs, 0, 60000, "output.reconnect_max_delay_ms");
    if (c.streaming.reconnectMaxDelayMs < c.streaming.reconnectDelayMs) {
        fail("output.reconnect_max_delay_ms", "must be greater than or equal to output.reconnect_delay_ms");
    }
    if (c.streaming.inactivityTimeoutMs != 0) {
        range(c.streaming.inactivityTimeoutMs, 100, 600000, "output.inactivity_timeout_ms");
    }
    range(c.streaming.queueCapacity, 8, 4096, "output.queue_capacity");
    range(c.streaming.maxInterleaveDeltaMs, 1, 5000, "output.max_interleave_delta_ms");
    if (c.streaming.enabled && c.capture.source == "file" && !c.capture.realtime) {
        fail("capture.realtime", "must be true when streaming a local file");
    }
    range(c.video.width, 2, 7680, "video.width");
    range(c.video.height, 2, 4320, "video.height");
    if (c.video.width % 2 != 0 || c.video.height % 2 != 0) {
        fail("video", "width and height must be even for YUV420/H.264");
    }
    range(c.video.fps, 1, 120, "video.fps");
    range(c.video.bitrate, 64000, 100000000, "video.bitrate");
    range(c.videoEncoder.gopSize, 1, 1200, "video_encoder.gop_size");
    constexpr std::array<std::string_view, 10> presets = {
        "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
    if (std::find(presets.begin(), presets.end(), c.videoEncoder.preset) == presets.end()) {
        fail("video_encoder.preset", "expected an x264 preset: ultrafast..placebo");
    }
    audio(c.audio.sampleRate, c.audio.channels);
    range(c.audio.bitrate, 16000, 512000, "audio.bitrate");
    if (c.capture.source != "devices" && c.capture.source != "file") {
        fail("capture.source", "expected devices or file");
    }
    if (c.capture.videoSource != "desktop" && c.capture.videoSource != "camera") {
        fail("capture.video_source", "expected desktop or camera");
    }
    if (c.capture.audioSource != "microphone" && c.capture.audioSource != "system") {
        fail("capture.audio_source", "expected microphone or system");
    }
#ifndef _WIN32
    if (c.capture.source == "devices" && c.capture.audio && c.capture.audioSource == "system") {
        fail("capture.audio_source", "system audio capture is supported only on Windows");
    }
#endif
    if (!c.capture.video && !c.capture.audio) fail("capture", "enable video or audio");
    if (c.capture.source == "file") device(c.capture.file, "capture.file");
    range(c.capture.timeoutMs, 100, 60000, "capture.timeout_ms");
    if (!c.capture.videoFormat.empty()) device(c.capture.videoFormat, "capture.video_format");
    const auto& video = c.preprocess.video;
    if (video.scaleAlgorithm != "nearest" && video.scaleAlgorithm != "bilinear" &&
        video.scaleAlgorithm != "bicubic" && video.scaleAlgorithm != "lanczos") {
        fail("preprocess.video.scale_algorithm", "expected nearest, bilinear, bicubic or lanczos");
    }
    if (video.resizeMode != "fit" && video.resizeMode != "crop" && video.resizeMode != "stretch") {
        fail("preprocess.video.resize_mode", "expected fit, crop or stretch");
    }
    if (video.rotation != 0 && video.rotation != 90 && video.rotation != 180 && video.rotation != 270) {
        fail("preprocess.video.rotation", "expected 0, 90, 180 or 270");
    }
    if (video.watermark.size() > 80 || std::any_of(video.watermark.begin(), video.watermark.end(),
        [](unsigned char ch) { return ch < 0x20 || ch > 0x7e; })) {
        fail("preprocess.video.watermark", "expected at most 80 printable ASCII characters");
    }
    const auto& preprocessingAudio = c.preprocess.audio;
    constexpr std::array<std::string_view, 12> formats = {
        "u8", "u8p", "s16", "s16p", "s32", "s32p", "s64", "s64p", "flt", "fltp", "dbl", "dblp"};
    if (std::find(formats.begin(), formats.end(), preprocessingAudio.sampleFormat) == formats.end()) {
        fail("preprocess.audio.sample_format", "unknown PCM sample format");
    }
    range(preprocessingAudio.frameSize, 1, 8192, "preprocess.audio.frame_size");
    range(preprocessingAudio.gainDb, -60, 30, "preprocess.audio.gain_db");
    range(preprocessingAudio.discontinuityThresholdMs, 1, 1000, "preprocess.audio.discontinuity_threshold_ms");
}

void validate(const PlayerConfig& c) {
    validateLogging(c.logging);
    range(c.pipeline.videoQueueCapacity, 1, 64, "pipeline.video_queue_capacity");
    range(c.pipeline.maxVideoAgeMs, 20, 5000, "pipeline.max_video_age_ms");
    url(c.inputUrl, false);
    device(c.audioDevice, "audio.device");
    audio(c.sampleRate, c.channels);
    range(c.audioPlayback.bufferMs, 100, 2000, "audio.buffer_ms");
    range(c.audioPlayback.volume, 0, 100, "audio.volume");
    range(c.sync.toleranceMs, 1, 100, "sync.tolerance_ms");
    range(c.sync.dropThresholdMs, 1, 1000, "sync.drop_threshold_ms");
    range(c.sync.audioWaitMs, 100, 5000, "sync.audio_wait_ms");
    range(c.sync.discontinuityThresholdMs, 500, 10000, "sync.discontinuity_threshold_ms");
    if (c.sync.dropThresholdMs < c.sync.toleranceMs) {
        fail("sync.drop_threshold_ms", "must be greater than or equal to sync.tolerance_ms");
    }
    if (c.sync.discontinuityThresholdMs <= c.sync.dropThresholdMs) {
        fail("sync.discontinuity_threshold_ms", "must be greater than sync.drop_threshold_ms");
    }
    range(c.video.width, 160, 7680, "video.width");
    range(c.video.height, 90, 4320, "video.height");
    range(c.demux.connectTimeoutMs, 100, 60000, "input.connect_timeout_ms");
    range(c.demux.readTimeoutMs, 100, 60000, "input.read_timeout_ms");
    range(c.demux.reconnectAttempts, -1, 100, "input.reconnect_attempts");
    range(c.demux.reconnectDelayMs, 0, 60000, "input.reconnect_delay_ms");
    range(c.demux.reconnectMaxDelayMs, 0, 60000, "input.reconnect_max_delay_ms");
    if (c.demux.reconnectMaxDelayMs < c.demux.reconnectDelayMs) {
        fail("input.reconnect_max_delay_ms", "must be greater than or equal to input.reconnect_delay_ms");
    }
    if (c.demux.inactivityTimeoutMs != 0) {
        range(c.demux.inactivityTimeoutMs, 100, 600000, "input.inactivity_timeout_ms");
    }
    range(c.demux.queueCapacity, 8, 4096, "input.queue_capacity");
    range(c.demux.maxBufferMs, 100, 60000, "input.max_buffer_ms");
    range(c.demux.maxQueueBytes, 1048576, 268435456, "input.max_queue_bytes");
    range(c.demux.discontinuityThresholdMs, 100, 60000, "input.discontinuity_threshold_ms");
}

PublisherConfig parsePublisherConfig(std::string_view text, const Overrides& overrides) {
    auto data = document(PublisherConfig{});
    merge(data, parse(text));
    applyOverrides(data, overrides);
    PublisherConfig c;
    c.videoDevice = data["devices"]["video"].get<std::string>();
    c.audioDevice = data["devices"]["audio"].get<std::string>();
    c.outputUrl = data["output"]["url"].get<std::string>();
    c.videoFile = data["output"]["video_file"].get<std::string>();
    c.audioFile = data["output"]["audio_file"].get<std::string>();
    const auto& output = data["output"];
    c.streaming.enabled = output["streaming"].get<bool>();
    c.streaming.connectTimeoutMs = integer(output["connect_timeout_ms"], "output.connect_timeout_ms");
    c.streaming.writeTimeoutMs = integer(output["write_timeout_ms"], "output.write_timeout_ms");
    c.streaming.reconnectAttempts = integer(output["reconnect_attempts"], "output.reconnect_attempts");
    c.streaming.reconnectDelayMs = integer(output["reconnect_delay_ms"], "output.reconnect_delay_ms");
    c.streaming.reconnectMaxDelayMs = integer(output["reconnect_max_delay_ms"], "output.reconnect_max_delay_ms");
    c.streaming.inactivityTimeoutMs = integer(output["inactivity_timeout_ms"], "output.inactivity_timeout_ms");
    c.streaming.queueCapacity = integer(output["queue_capacity"], "output.queue_capacity");
    c.streaming.maxInterleaveDeltaMs = integer(output["max_interleave_delta_ms"], "output.max_interleave_delta_ms");
    c.video.width = integer(data["video"]["width"], "video.width");
    c.video.height = integer(data["video"]["height"], "video.height");
    c.video.fps = integer(data["video"]["fps"], "video.fps");
    c.video.bitrate = integer(data["video"]["bitrate"], "video.bitrate");
    c.videoEncoder.gopSize = integer(data["video_encoder"]["gop_size"], "video_encoder.gop_size");
    c.videoEncoder.preset = data["video_encoder"]["preset"].get<std::string>();
    c.videoEncoder.lowLatency = data["video_encoder"]["low_latency"].get<bool>();
    c.audio.sampleRate = integer(data["audio"]["sample_rate"], "audio.sample_rate");
    c.audio.channels = integer(data["audio"]["channels"], "audio.channels");
    c.audio.bitrate = integer(data["audio"]["bitrate"], "audio.bitrate");
    const auto& capture = data["capture"];
    c.capture.source = capture["source"].get<std::string>();
    c.capture.videoSource = capture["video_source"].get<std::string>();
    c.capture.audioSource = capture["audio_source"].get<std::string>();
    c.capture.file = capture["file"].get<std::string>();
    c.capture.loop = capture["loop"].get<bool>();
    c.capture.realtime = capture["realtime"].get<bool>();
    c.capture.video = capture["video"].get<bool>();
    c.capture.audio = capture["audio"].get<bool>();
    c.capture.timeoutMs = integer(capture["timeout_ms"], "capture.timeout_ms");
    c.capture.videoFormat = capture["video_format"].get<std::string>();
    const auto& videoPreprocess = data["preprocess"]["video"];
    c.preprocess.video.scaleAlgorithm = videoPreprocess["scale_algorithm"].get<std::string>();
    c.preprocess.video.resizeMode = videoPreprocess["resize_mode"].get<std::string>();
    c.preprocess.video.rotation = integer(videoPreprocess["rotation"], "preprocess.video.rotation");
    c.preprocess.video.mirror = videoPreprocess["mirror"].get<bool>();
    c.preprocess.video.watermark = videoPreprocess["watermark"].get<std::string>();
    c.preprocess.video.timestampOverlay = videoPreprocess["timestamp_overlay"].get<bool>();
    const auto& audioPreprocess = data["preprocess"]["audio"];
    c.preprocess.audio.sampleFormat = audioPreprocess["sample_format"].get<std::string>();
    c.preprocess.audio.frameSize = integer(audioPreprocess["frame_size"], "preprocess.audio.frame_size");
    c.preprocess.audio.gainDb = integer(audioPreprocess["gain_db"], "preprocess.audio.gain_db");
    c.preprocess.audio.discontinuityThresholdMs = integer(audioPreprocess["discontinuity_threshold_ms"],
        "preprocess.audio.discontinuity_threshold_ms");
    const auto& pipeline = data["pipeline"];
    c.pipeline.videoQueueCapacity = integer(pipeline["video_queue_capacity"], "pipeline.video_queue_capacity");
    c.pipeline.audioQueueCapacity = integer(pipeline["audio_queue_capacity"], "pipeline.audio_queue_capacity");
    c.pipeline.packetQueueCapacity = integer(pipeline["packet_queue_capacity"], "pipeline.packet_queue_capacity");
    c.pipeline.maxVideoAgeMs = integer(pipeline["max_video_age_ms"], "pipeline.max_video_age_ms");
    c.logging = loggingConfig(data["logging"]);
    validate(c);
    return c;
}

PlayerConfig parsePlayerConfig(std::string_view text, const Overrides& overrides) {
    auto data = document(PlayerConfig{});
    merge(data, parse(text));
    applyOverrides(data, overrides);
    PlayerConfig c;
    const auto& input = data["input"];
    c.inputUrl = input["url"].get<std::string>();
    c.demux.connectTimeoutMs = integer(input["connect_timeout_ms"], "input.connect_timeout_ms");
    c.demux.readTimeoutMs = integer(input["read_timeout_ms"], "input.read_timeout_ms");
    c.demux.reconnectAttempts = integer(input["reconnect_attempts"], "input.reconnect_attempts");
    c.demux.reconnectDelayMs = integer(input["reconnect_delay_ms"], "input.reconnect_delay_ms");
    c.demux.reconnectMaxDelayMs = integer(input["reconnect_max_delay_ms"], "input.reconnect_max_delay_ms");
    c.demux.inactivityTimeoutMs = integer(input["inactivity_timeout_ms"], "input.inactivity_timeout_ms");
    c.demux.queueCapacity = integer(input["queue_capacity"], "input.queue_capacity");
    c.demux.maxBufferMs = integer(input["max_buffer_ms"], "input.max_buffer_ms");
    c.demux.maxQueueBytes = integer(input["max_queue_bytes"], "input.max_queue_bytes");
    c.demux.discontinuityThresholdMs = integer(input["discontinuity_threshold_ms"], "input.discontinuity_threshold_ms");
    c.audioDevice = data["audio"]["device"].get<std::string>();
    c.sampleRate = integer(data["audio"]["sample_rate"], "audio.sample_rate");
    c.channels = integer(data["audio"]["channels"], "audio.channels");
    c.audioPlayback.enabled = data["audio"]["enabled"].get<bool>();
    c.audioPlayback.bufferMs = integer(data["audio"]["buffer_ms"], "audio.buffer_ms");
    c.audioPlayback.volume = integer(data["audio"]["volume"], "audio.volume");
    c.audioPlayback.muted = data["audio"]["muted"].get<bool>();
    c.audioPlayback.paused = data["audio"]["paused"].get<bool>();
    c.video.width = integer(data["video"]["width"], "video.width");
    c.video.height = integer(data["video"]["height"], "video.height");
    c.video.vsync = data["video"]["vsync"].get<bool>();
    c.video.fullscreen = data["video"]["fullscreen"].get<bool>();
    const auto& sync = data["sync"];
    c.sync.toleranceMs = integer(sync["tolerance_ms"], "sync.tolerance_ms");
    c.sync.dropThresholdMs = integer(sync["drop_threshold_ms"], "sync.drop_threshold_ms");
    c.sync.audioWaitMs = integer(sync["audio_wait_ms"], "sync.audio_wait_ms");
    c.sync.discontinuityThresholdMs = integer(sync["discontinuity_threshold_ms"], "sync.discontinuity_threshold_ms");
    const auto& pipeline = data["pipeline"];
    c.pipeline.videoQueueCapacity = integer(pipeline["video_queue_capacity"], "pipeline.video_queue_capacity");
    c.pipeline.maxVideoAgeMs = integer(pipeline["max_video_age_ms"], "pipeline.max_video_age_ms");
    c.logging = loggingConfig(data["logging"]);
    validate(c);
    return c;
}

PublisherConfig loadPublisherConfig(const std::optional<std::filesystem::path>& file, const Overrides& overrides) {
    return parsePublisherConfig(file ? readFile(*file) : "{}", overrides);
}

PlayerConfig loadPlayerConfig(const std::optional<std::filesystem::path>& file, const Overrides& overrides) {
    return parsePlayerConfig(file ? readFile(*file) : "{}", overrides);
}

std::string toJson(const PublisherConfig& c) {
    validate(c);
    return document(c).dump(2);
}

std::string toJson(const PlayerConfig& c) {
    validate(c);
    return document(c).dump(2);
}

} // namespace lsl::config
