#include "publisher/encode/VideoFileWriter.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <filesystem>
#include <utility>

namespace lsl::encode {
namespace {

constexpr const char* module = "video-file";

[[noreturn]] void invalid(std::string message) {
    throw error::AppError(module, error::InvalidConfig, std::move(message));
}

std::string preparePath(const std::string& path) {
    if (path.empty()) invalid("video output path must not be empty");
    // 先转成本地绝对路径，避免将文件名误解释成 FFmpeg 的协议地址。
    std::error_code ec;
    const auto result = std::filesystem::absolute(std::filesystem::u8path(path), ec).lexically_normal();
    if (ec) throw error::AppError(module, error::Io, "resolve video output path: " + ec.message());
    const bool exists = std::filesystem::exists(result, ec);
    if (ec) throw error::AppError(module, error::Io, "inspect video output path: " + ec.message());
    if (exists) throw error::AppError(module, error::Io, "video output already exists: " + result.u8string());
    std::filesystem::create_directories(result.parent_path(), ec);
    if (ec) throw error::AppError(module, error::Io, "create video output directory: " + ec.message());
    return result.u8string();
}

} // namespace

VideoFileWriter::VideoFileWriter(std::string path, const AVCodecParameters& parameters,
                                 AVRational encoderTimeBase)
    : encoderTimeBase_(encoderTimeBase) {
    if (encoderTimeBase_.num <= 0 || encoderTimeBase_.den <= 0) {
        invalid("video encoder time base must be positive");
    }
    if (parameters.codec_type != AVMEDIA_TYPE_VIDEO || parameters.codec_id != AV_CODEC_ID_H264) {
        invalid("M05 video output requires H.264 video parameters");
    }
    if (!parameters.extradata || parameters.extradata_size <= 0) {
        invalid("MP4 output requires H.264 global header parameters from the opened encoder");
    }
    path_ = preparePath(path);

    AVFormatContext* raw = nullptr;
    const int allocated = avformat_alloc_output_context2(&raw, nullptr, "mp4", path_.c_str());
    media::OutputFormatPtr context{raw};
    error::throwIfFFmpegFailed(allocated, module, "allocate MP4 output context");
    if (!context) throw std::bad_alloc{};

    AVStream* stream = avformat_new_stream(context.get(), nullptr);
    if (!stream) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(avcodec_parameters_copy(stream->codecpar, &parameters),
                               module, "copy H.264 output parameters");
    stream->codecpar->codec_tag = 0;
    stream->time_base = encoderTimeBase_;
    stream->avg_frame_rate = av_inv_q(encoderTimeBase_);
    stream->r_frame_rate = stream->avg_frame_rate;

    error::throwIfFFmpegFailed(avio_open(&context->pb, path_.c_str(), AVIO_FLAG_WRITE),
                               module, "open MP4 video output");
    error::throwIfFFmpegFailed(avformat_write_header(context.get(), nullptr),
                               module, "write MP4 header");
    // write_header 可以调整 stream->time_base，逐包写入时使用调整后的值。
    stream_ = stream;
    context_ = std::move(context);
}

VideoFileWriter::~VideoFileWriter() noexcept {
    try {
        finish();
    } catch (...) {
        // 析构只尽力保全文件；调用方必须显式 finish() 才能确认写尾和关闭成功。
    }
}

void VideoFileWriter::write(media::PacketPtr packet) {
    if (finished_) invalid("cannot write video packets after finishing the MP4 file");
    if (failed_) throw error::AppError(module, error::Io, "MP4 output has already failed");
    if (!packet || !packet->data || packet->size <= 0) invalid("video output packet must contain encoded data");
    if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
        invalid("MP4 video packets require encoder PTS and DTS");
    }

    av_packet_rescale_ts(packet.get(), encoderTimeBase_, stream_->time_base);
    packet->time_base = stream_->time_base;
    packet->stream_index = stream_->index;
    packet->pos = -1;
    const int result = av_interleaved_write_frame(context_.get(), packet.get());
    if (result < 0) {
        failed_ = true;
        throw error::FFmpegError(module, result, "write H.264 packet to MP4");
    }
}

void VideoFileWriter::finish() {
    if (finished_) {
        if (failed_) throw error::AppError(module, error::Io, "MP4 output did not finish successfully");
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

    error::throwIfFFmpegFailed(trailerResult, module, "write MP4 trailer");
    error::throwIfFFmpegFailed(ioResult, module, "flush MP4 output");
    error::throwIfFFmpegFailed(closeResult, module, "close MP4 output");
    if (previousFailure) {
        throw error::AppError(module, error::Io, "MP4 output is incomplete after an earlier packet write failure");
    }
}

} // namespace lsl::encode
