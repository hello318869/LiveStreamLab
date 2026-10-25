#include "publisher/muxer/AudioFileWriter.hpp"

#include <filesystem>
#include <utility>

namespace lsl::muxer {
namespace {

constexpr const char* module = "audio-file";

[[noreturn]] void invalid(std::string message) {
    throw error::AppError(module, error::InvalidConfig, std::move(message));
}

std::string preparePath(const std::string& path) {
    if (path.empty()) invalid("audio output path must not be empty");
    // 使用本地绝对路径，避免将文件名误解释成 FFmpeg 的协议地址。
    std::error_code ec;
    const auto result = std::filesystem::absolute(std::filesystem::u8path(path), ec).lexically_normal();
    if (ec) throw error::AppError(module, error::Io, "resolve audio output path: " + ec.message());
    const bool exists = std::filesystem::exists(result, ec);
    if (ec) throw error::AppError(module, error::Io, "inspect audio output path: " + ec.message());
    if (exists) throw error::AppError(module, error::Io, "audio output already exists: " + result.u8string());
    std::filesystem::create_directories(result.parent_path(), ec);
    if (ec) throw error::AppError(module, error::Io, "create audio output directory: " + ec.message());
    return result.u8string();
}

} // namespace

AudioFileWriter::AudioFileWriter(std::string path, const AVCodecParameters& parameters,
                                 AVRational encoderTimeBase)
    : encoderTimeBase_(encoderTimeBase) {
    if (encoderTimeBase_.num <= 0 || encoderTimeBase_.den <= 0) {
        invalid("audio encoder time base must be positive");
    }
    if (parameters.codec_type != AVMEDIA_TYPE_AUDIO || parameters.codec_id != AV_CODEC_ID_AAC) {
        invalid("M06 audio output requires AAC audio parameters");
    }
    if (!parameters.extradata || parameters.extradata_size <= 0) {
        invalid("M4A output requires AAC global header parameters from the opened encoder");
    }
    path_ = preparePath(path);

    AVFormatContext* raw = nullptr;
    const int allocated = avformat_alloc_output_context2(&raw, nullptr, "ipod", path_.c_str());
    media::OutputFormatPtr context{raw};
    error::throwIfFFmpegFailed(allocated, module, "allocate M4A output context");
    if (!context) throw std::bad_alloc{};

    AVStream* stream = avformat_new_stream(context.get(), nullptr);
    if (!stream) throw std::bad_alloc{};
    // 连同 AAC AudioSpecificConfig 和 initial_padding 复制，供容器描述编码延迟。
    error::throwIfFFmpegFailed(avcodec_parameters_copy(stream->codecpar, &parameters),
                               module, "copy AAC output parameters");
    stream->codecpar->codec_tag = 0;
    stream->time_base = encoderTimeBase_;
    // AAC 首包通常有负 PTS；交由 MOV/M4A 编辑列表描述 priming，不强制整体平移。
    context->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;

    error::throwIfFFmpegFailed(avio_open(&context->pb, path_.c_str(), AVIO_FLAG_WRITE),
                               module, "open M4A audio output");
    error::throwIfFFmpegFailed(avformat_write_header(context.get(), nullptr),
                               module, "write M4A header");
    // write_header 可以调整 stream->time_base，逐包写入时使用调整后的值。
    stream_ = stream;
    context_ = std::move(context);
}

AudioFileWriter::~AudioFileWriter() noexcept {
    try {
        finish();
    } catch (...) {
        // 析构只尽力保全文件；调用方必须显式 finish() 才能确认写尾和关闭成功。
    }
}

void AudioFileWriter::write(media::PacketPtr packet) {
    if (finished_) invalid("cannot write audio packets after finishing the M4A file");
    if (failed_) throw error::AppError(module, error::Io, "M4A output has already failed");
    if (!packet || !packet->data || packet->size <= 0) invalid("audio output packet must contain encoded data");
    if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
        invalid("M4A audio packets require encoder PTS and DTS");
    }

    // 原包直接交给封装器，保留 SKIP_SAMPLES 等 side data；不裁去负的 priming 时间戳。
    av_packet_rescale_ts(packet.get(), encoderTimeBase_, stream_->time_base);
    packet->time_base = stream_->time_base;
    packet->stream_index = stream_->index;
    packet->pos = -1;
    const int result = av_interleaved_write_frame(context_.get(), packet.get());
    if (result < 0) {
        failed_ = true;
        throw error::FFmpegError(module, result, "write AAC packet to M4A");
    }
}

void AudioFileWriter::finish() {
    if (finished_) {
        if (failed_) throw error::AppError(module, error::Io, "M4A output did not finish successfully");
        return;
    }
    finished_ = true;
    if (!context_) return;

    // 即使写包失败也尽力写尾，但不能将该文件报告为完整输出。
    const bool previousFailure = failed_;
    const int trailerResult = av_write_trailer(context_.get());
    int ioResult = 0;
    int closeResult = 0;
    if (context_->pb) {
        avio_flush(context_->pb);
        ioResult = context_->pb->error;
        closeResult = avio_closep(&context_->pb);
    }
    context_.reset();
    stream_ = nullptr;
    failed_ = previousFailure || trailerResult < 0 || ioResult < 0 || closeResult < 0;

    error::throwIfFFmpegFailed(trailerResult, module, "write M4A trailer");
    error::throwIfFFmpegFailed(ioResult, module, "flush M4A output");
    error::throwIfFFmpegFailed(closeResult, module, "close M4A output");
    if (previousFailure) {
        throw error::AppError(module, error::Io, "M4A output is incomplete after an earlier packet write failure");
    }
}

} // namespace lsl::muxer
