#pragma once

#include "common/error/Error.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <memory>

namespace lsl::media {
namespace detail {

template <auto Free>
struct Deleter {
    template <typename T>
    void operator()(T* ptr) const noexcept { Free(ptr); }
};

template <auto Free>
struct IndirectDeleter {
    template <typename T>
    void operator()(T* ptr) const noexcept { Free(&ptr); }
};

struct OutputFormatDeleter {
    void operator()(AVFormatContext* ptr) const noexcept {
        // 普通输出拥有 avio_open 创建的 pb；自定义/格式内部 I/O 由其所有者释放。
        if (ptr->pb && !(ptr->flags & AVFMT_FLAG_CUSTOM_IO) &&
            (!ptr->oformat || !(ptr->oformat->flags & AVFMT_NOFILE))) {
            avio_closep(&ptr->pb);
        }
        avformat_free_context(ptr);
    }
};

struct CustomAvioDeleter {
    void operator()(AVIOContext* ptr) const noexcept {
        av_freep(&ptr->buffer);
        avio_context_free(&ptr);
    }
};

} // namespace detail

// 只接管拥有所有权的指针；AVStream、stream->codecpar、查找到的 AVCodec 等为借用。
using FramePtr = std::unique_ptr<AVFrame, detail::IndirectDeleter<av_frame_free>>;
using PacketPtr = std::unique_ptr<AVPacket, detail::IndirectDeleter<av_packet_free>>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, detail::IndirectDeleter<avcodec_free_context>>;
using CodecParametersPtr = std::unique_ptr<AVCodecParameters, detail::IndirectDeleter<avcodec_parameters_free>>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, detail::IndirectDeleter<avformat_close_input>>;
using OutputFormatPtr = std::unique_ptr<AVFormatContext, detail::OutputFormatDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, detail::Deleter<sws_freeContext>>;
using SwrContextPtr = std::unique_ptr<SwrContext, detail::IndirectDeleter<swr_free>>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, detail::Deleter<av_audio_fifo_free>>;
using DictionaryPtr = std::unique_ptr<AVDictionary, detail::IndirectDeleter<av_dict_free>>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, detail::IndirectDeleter<av_buffer_unref>>;
using BufferPtr = std::unique_ptr<unsigned char, detail::Deleter<av_free>>;

// AvioPtr 接管 avio_open/open2 的结果；不能重复接管 OutputFormatPtr 已拥有的 pb。
using AvioPtr = std::unique_ptr<AVIOContext, detail::IndirectDeleter<avio_closep>>;
// CustomAvioPtr 接管 avio_alloc_context 的结果及其 buffer，opaque 仍由调用方管理。
using CustomAvioPtr = std::unique_ptr<AVIOContext, detail::CustomAvioDeleter>;

// 新对象独占 AVFrame/AVPacket 结构，底层媒体缓冲通过 FFmpeg 引用计数共享。
// 跨线程交接后，修改缓冲前必须调用 av_frame_make_writable / av_packet_make_writable。
inline FramePtr referenceFrame(const AVFrame& source) {
    FramePtr frame{av_frame_alloc()};
    if (!frame) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(av_frame_ref(frame.get(), &source), "media", "reference frame");
    return frame;
}

inline PacketPtr referencePacket(const AVPacket& source) {
    PacketPtr packet{av_packet_alloc()};
    if (!packet) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(av_packet_ref(packet.get(), &source), "media", "reference packet");
    return packet;
}

// 例：FramePtr frame{av_frame_alloc()}; PacketPtr copy{av_packet_clone(packet)};
// 独占所有权，可移动不可复制；分配失败检查、ref/unref、刷新和写尾由调用方负责。
} // namespace lsl::media
