#include "publisher/preprocess/VideoPreprocessor.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace lsl::preprocess {
namespace {

constexpr AVRational microseconds{1, 1000000};
constexpr std::int64_t maxImagePixels = 64LL * 1024 * 1024;
constexpr int maxImageDimension = 16384;
constexpr std::int64_t overlayOutputPixelBudget = 64LL * 1024 * 1024;

[[noreturn]] void invalid(std::string message) {
    throw error::AppError("video-preprocess", error::InvalidConfig, std::move(message));
}

int scaleFlags(std::string_view name) {
    if (name == "nearest") return SWS_POINT;
    if (name == "bilinear") return SWS_BILINEAR;
    if (name == "bicubic") return SWS_BICUBIC;
    if (name == "lanczos") return SWS_LANCZOS;
    invalid("scale_algorithm must be nearest, bilinear, bicubic or lanczos");
}

void checkImageSize(int width, int height) {
    if (width <= 0 || height <= 0 || width > maxImageDimension || height > maxImageDimension ||
        static_cast<std::int64_t>(width) * height > maxImagePixels) {
        invalid("video image dimensions exceed the supported 16384 / 64-megapixel bound");
    }
    error::throwIfFFmpegFailed(av_image_check_size(width, height, 0, nullptr),
                               "video-preprocess", "validate image dimensions");
}

void validateInput(const AVFrame& frame) {
    checkImageSize(frame.width, frame.height);
    if (frame.crop_left >= static_cast<std::size_t>(frame.width) ||
        frame.crop_right >= static_cast<std::size_t>(frame.width) - frame.crop_left ||
        frame.crop_top >= static_cast<std::size_t>(frame.height) ||
        frame.crop_bottom >= static_cast<std::size_t>(frame.height) - frame.crop_top) {
        invalid("input cropping removes the entire video frame");
    }
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const auto* desc = av_pix_fmt_desc_get(format);
    if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || !sws_isSupportedInput(format)) {
        invalid("input must be a libswscale-supported software pixel format");
    }
    std::array<bool, 4> planes{};
    for (int component = 0; component < desc->nb_components; ++component) {
        const int plane = desc->comp[component].plane;
        if (plane < 0 || plane >= static_cast<int>(planes.size())) invalid("unsupported video plane layout");
        planes[plane] = true;
    }
    for (int plane = 0; plane < static_cast<int>(planes.size()); ++plane) {
        if (!planes[plane]) continue;
        const int minimum = av_image_get_linesize(format, frame.width, plane);
        error::throwIfFFmpegFailed(minimum, "video-preprocess", "validate input linesize");
        const auto stride = static_cast<std::int64_t>(frame.linesize[plane]);
        if (!frame.data[plane] || std::abs(stride) < minimum) invalid("missing video plane or invalid linesize");
    }
    if ((desc->flags & AV_PIX_FMT_FLAG_PAL) && !frame.data[1]) invalid("missing video palette");
    if (frame.sample_aspect_ratio.num < 0 || frame.sample_aspect_ratio.den < 0 ||
        (frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den == 0)) {
        invalid("invalid input sample aspect ratio");
    }
}

media::FramePtr allocateFrame(int width, int height, const AVFrame& source) {
    checkImageSize(width, height);
    media::FramePtr frame{av_frame_alloc()};
    if (!frame) throw std::bad_alloc{};
    error::throwIfFFmpegFailed(av_frame_copy_props(frame.get(), &source),
                               "video-preprocess", "copy video properties");
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    frame->crop_left = frame->crop_right = frame->crop_top = frame->crop_bottom = 0;
    error::throwIfFFmpegFailed(av_frame_get_buffer(frame.get(), 32),
                               "video-preprocess", "allocate YUV420P frame");
    return frame;
}

struct ColorDescription {
    AVColorSpace space;
    AVColorRange range;
    int coefficients;
    bool sourceFullRange;
};

ColorDescription colors(const AVFrame& source) {
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source.format));
    const bool rgb = desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
    AVColorSpace space = source.colorspace;
    int coefficients = SWS_CS_DEFAULT;
    switch (space) {
    case AVCOL_SPC_BT709: coefficients = SWS_CS_ITU709; break;
    case AVCOL_SPC_FCC: coefficients = SWS_CS_FCC; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: coefficients = SWS_CS_ITU601; break;
    case AVCOL_SPC_SMPTE240M: coefficients = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL: coefficients = SWS_CS_BT2020; break;
    case AVCOL_SPC_UNSPECIFIED:
    case AVCOL_SPC_RGB:
        // 未声明矩阵时为 swscale 明确给出惯用默认值，并让输出元数据与之保持一致。
        space = source.height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        coefficients = source.height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        break;
    default:
        // swscale 的传统矩阵接口不能正确表达 BT.2020-CL / ICtCp 等变换。
        invalid("input color matrix is unsupported by the software YUV420P conversion");
    }
    const auto format = static_cast<AVPixelFormat>(source.format);
    const bool jpegFormat = format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P ||
        format == AV_PIX_FMT_YUVJ444P || format == AV_PIX_FMT_YUVJ440P || format == AV_PIX_FMT_YUVJ411P;
    const bool full = source.color_range == AVCOL_RANGE_JPEG ||
        ((rgb || jpegFormat) && source.color_range == AVCOL_RANGE_UNSPECIFIED);
    // RGB 采集通常为全范围，编码用 YUV 默认为电视范围；显式全范围 YUV 则保留。
    const AVColorRange range = !rgb && full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    return {space, range, coefficients, full};
}

void setOutputProperties(AVFrame& frame, const ColorDescription& color) {
    frame.sample_aspect_ratio = AVRational{1, 1};
    frame.colorspace = color.space;
    frame.color_range = color.range;
    frame.crop_left = frame.crop_right = frame.crop_top = frame.crop_bottom = 0;
    frame.pict_type = AV_PICTURE_TYPE_NONE;
    frame.pkt_dts = AV_NOPTS_VALUE;
    frame.best_effort_timestamp = AV_NOPTS_VALUE;
}

void clearGeometrySideData(AVFrame& frame, bool geometryChanged) {
    // 本模块只执行显式 options.rotation。保留输入显示矩阵会让下游再次旋转已处理画面。
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_DISPLAYMATRIX);
    if (!geometryChanged) return;
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_PANSCAN);
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_REGIONS_OF_INTEREST);
    av_frame_remove_side_data(&frame, AV_FRAME_DATA_MOTION_VECTORS);
    // 这些显式类型在最低支持版本 FFmpeg 6 中已存在，不依赖新版 side-data 描述器 API。
}

struct Geometry {
    int width;
    int height;
    int outputX;
    int outputY;
    int cropX;
    int cropY;
    int copyWidth;
    int copyHeight;
};

int evenDimension(long double value, bool roundUp) {
    if (!std::isfinite(value) || value > maxImageDimension) invalid("scaled crop dimensions exceed the supported image bound");
    const auto units = roundUp ? std::ceil(value / 2) : std::floor(value / 2);
    return std::max(2, static_cast<int>(units) * 2);
}

Geometry geometry(const AVFrame& source, const VideoPreprocessOptions& options) {
    const auto sar = source.sample_aspect_ratio;
    const long double pixelAspect = sar.num > 0 && sar.den > 0
        ? static_cast<long double>(sar.num) / sar.den : 1.0L;
    long double aspect = static_cast<long double>(source.width) * pixelAspect / source.height;
    const bool sideways = options.rotation == 90 || options.rotation == 270;
    if (sideways) aspect = 1 / aspect;
    const long double targetAspect = static_cast<long double>(options.width) / options.height;
    int width = options.width;
    int height = options.height;
    if (options.resizeMode == "fit") {
        if (aspect >= targetAspect) height = std::min(height, evenDimension(width / aspect, false));
        else width = std::min(width, evenDimension(height * aspect, false));
    } else if (options.resizeMode == "crop") {
        if (aspect >= targetAspect) width = std::max(width, evenDimension(height * aspect, true));
        else height = std::max(height, evenDimension(width / aspect, true));
    }
    checkImageSize(width, height);
    // 偏移按 2 像素对齐，保证 Y 与 4:2:0 色度平面使用同一裁剪 / 留黑区域。
    const int outputX = std::max(0, (options.width - width) / 2) & ~1;
    const int outputY = std::max(0, (options.height - height) / 2) & ~1;
    const int cropX = std::max(0, (width - options.width) / 2) & ~1;
    const int cropY = std::max(0, (height - options.height) / 2) & ~1;
    return {sideways ? height : width, sideways ? width : height,
            outputX, outputY, cropX, cropY,
            std::min(width, options.width), std::min(height, options.height)};
}

void fillBlack(AVFrame& frame) {
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? frame.width : frame.width / 2;
        const int height = plane == 0 ? frame.height : frame.height / 2;
        const int value = plane == 0 ? (frame.color_range == AVCOL_RANGE_JPEG ? 0 : 16) : 128;
        for (int row = 0; row < height; ++row) {
            std::memset(frame.data[plane] + static_cast<std::ptrdiff_t>(row) * frame.linesize[plane], value, width);
        }
    }
}

void transform(const AVFrame& source, AVFrame& output, const Geometry& layout,
               const VideoPreprocessOptions& options) {
    fillBlack(output);
    for (int plane = 0; plane < 3; ++plane) {
        const int shift = plane == 0 ? 0 : 1;
        const int width = source.width >> shift;
        const int height = source.height >> shift;
        const int rotatedWidth = options.rotation == 90 || options.rotation == 270 ? height : width;
        for (int y = 0; y < (layout.copyHeight >> shift); ++y) {
            auto* row = output.data[plane] +
                static_cast<std::ptrdiff_t>((layout.outputY >> shift) + y) * output.linesize[plane] + (layout.outputX >> shift);
            for (int x = 0; x < (layout.copyWidth >> shift); ++x) {
                int rx = (layout.cropX >> shift) + x;
                const int ry = (layout.cropY >> shift) + y;
                if (options.mirror) rx = rotatedWidth - 1 - rx;
                int sx = rx;
                int sy = ry;
                switch (options.rotation) {
                case 90: sx = ry; sy = height - 1 - rx; break;
                case 180: sx = width - 1 - rx; sy = height - 1 - ry; break;
                case 270: sx = width - 1 - ry; sy = rx; break;
                default: break;
                }
                row[x] = source.data[plane][static_cast<std::ptrdiff_t>(sy) * source.linesize[plane] + sx];
            }
        }
    }
}

// 5 x 7 的可打印 ASCII 位图；列的低位为上方像素，不依赖操作系统字体。
constexpr std::uint8_t font[95][5] = {
    {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
    {0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},{8,8,0x3e,8,8},
    {0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
    {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},{0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},
    {0,0x36,0x36,0,0},{0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},
    {2,4,8,0x10,0x20},{0,0x41,0x41,0x7f,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},
    {8,0x7e,9,1,2},{0x0c,0x52,0x52,0x52,0x3e},{0x7f,8,4,4,0x78},
    {0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},{0x7f,0x10,0x28,0x44,0},
    {0,0x41,0x7f,0x40,0},{0x7c,4,0x18,4,0x78},{0x7c,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7c},
    {0x7c,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},
    {0,8,0x36,0x41,0},{0,0,0x7f,0,0},{0,0x41,0x36,8,0},{8,4,8,0x10,8}
};

void pixel(AVFrame& frame, int x, int y, int luma) {
    frame.data[0][static_cast<std::ptrdiff_t>(y) * frame.linesize[0] + x] = static_cast<std::uint8_t>(luma);
    for (int plane = 1; plane < 3; ++plane) {
        frame.data[plane][static_cast<std::ptrdiff_t>(y / 2) * frame.linesize[plane] + x / 2] = 128;
    }
}

void drawText(AVFrame& frame, std::string_view text, bool bottom) {
    if (text.empty()) return;
    error::throwIfFFmpegFailed(av_frame_make_writable(&frame), "video-preprocess", "make overlay frame writable");
    const int scale = frame.width >= 640 && frame.height >= 360 ? 2 : 1;
    const int padding = 2;
    const int left = std::min(8, std::max(0, frame.width - (5 * scale + 2 * padding)));
    const int columns = (frame.width - left - 2 * padding) / (6 * scale);
    const int maxRows = (frame.height - 2 * padding) / (8 * scale);
    if (columns <= 0 || maxRows <= 0) return;
    const int rows = std::min(maxRows, static_cast<int>((text.size() + columns - 1) / columns));
    const int top = bottom ? std::max(0, frame.height - rows * 8 * scale - 2 * padding - 8) :
                            std::min(8, std::max(0, frame.height - rows * 8 * scale - 2 * padding));
    const int boxWidth = std::min(frame.width - left, std::min(columns, static_cast<int>(text.size())) * 6 * scale + 2 * padding);
    const int boxHeight = std::min(frame.height - top, rows * 8 * scale + 2 * padding);
    const int black = frame.color_range == AVCOL_RANGE_JPEG ? 0 : 16;
    const int white = frame.color_range == AVCOL_RANGE_JPEG ? 255 : 235;
    for (int y = top; y < top + boxHeight; ++y) {
        for (int x = left; x < left + boxWidth; ++x) pixel(frame, x, y, black);
    }
    const auto visible = std::min(text.size(), static_cast<std::size_t>(columns) * rows);
    for (std::size_t index = 0; index < visible; ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        const auto* glyph = font[(character >= 32 && character <= 126 ? character : '?') - 32];
        const int x0 = left + padding + static_cast<int>(index % columns) * 6 * scale;
        const int y0 = top + padding + static_cast<int>(index / columns) * 8 * scale;
        for (int x = 0; x < 5 * scale; ++x) {
            for (int y = 0; y < 7 * scale; ++y) {
                if ((glyph[x / scale] & (1 << (y / scale))) && x0 + x < frame.width && y0 + y < frame.height) {
                    pixel(frame, x0 + x, y0 + y, white);
                }
            }
        }
    }
}

std::string elapsedText(std::int64_t pts, int fps) {
    const auto milliseconds = av_rescale_q(pts, AVRational{1, fps}, AVRational{1, 1000});
    const auto seconds = milliseconds / 1000;
    std::ostringstream text;
    text << "LIVE " << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
         << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2) << seconds % 60
         << '.' << std::setw(3) << milliseconds % 1000;
    return text.str();
}

} // namespace

struct VideoPreprocessor::Impl {
    VideoPreprocessOptions options;
    std::int64_t originUs;
    int flags;
    media::SwsContextPtr scaler;
    media::FramePtr previous;
    bool flushed = false;
    std::int64_t lastPts = -1;
    std::int64_t firstPts = 0;
    std::int64_t firstInputUs = 0;
    std::int64_t lastInputUs = 0;
    VideoPreprocessStats counters;

    Impl(VideoPreprocessOptions value, std::int64_t origin)
        : options(std::move(value)), originUs(origin), flags(scaleFlags(options.scaleAlgorithm)) {
        checkImageSize(options.width, options.height);
        if ((options.width & 1) || (options.height & 1) || options.width < 2 || options.height < 2) {
            invalid("YUV420P output width and height must be even and at least 2");
        }
        if (options.fps < 1 || options.fps > 240) invalid("video fps must be in [1, 240]");
        if (options.resizeMode != "fit" && options.resizeMode != "crop" && options.resizeMode != "stretch") {
            invalid("resize_mode must be fit, crop or stretch");
        }
        if (options.rotation != 0 && options.rotation != 90 && options.rotation != 180 && options.rotation != 270) {
            invalid("video rotation must be 0, 90, 180 or 270");
        }
        if (options.watermark.size() > 80 || std::any_of(options.watermark.begin(), options.watermark.end(),
            [](unsigned char c) { return c < 32 || c > 126; })) {
            invalid("watermark must contain at most 80 printable ASCII characters");
        }
        if (originUs == AV_NOPTS_VALUE) invalid("video origin timestamp is missing");
    }

    std::int64_t ptsFor(std::int64_t timestampUs) const {
        if (timestampUs == AV_NOPTS_VALUE) invalid("video capture timestamp is missing");
        if (timestampUs <= originUs) return 0; // 起点前的抖动映射到首槽，随后按同槽帧丢弃。
        const auto delta = static_cast<std::uint64_t>(timestampUs) - static_cast<std::uint64_t>(originUs);
        if (delta > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            invalid("video capture timestamp exceeds the timeline range");
        }
        return av_rescale_q_rnd(static_cast<std::int64_t>(delta), microseconds,
                                AVRational{1, options.fps}, AV_ROUND_NEAR_INF);
    }

    media::FramePtr prepare(const AVFrame& source) {
        media::FramePtr cropped;
        const AVFrame* input = &source;
        if (source.crop_left || source.crop_right || source.crop_top || source.crop_bottom) {
            cropped = media::referenceFrame(source);
            // 使用 FFmpeg 的像素格式感知裁剪，保留实际可见区域；允许非对齐首地址。
            error::throwIfFFmpegFailed(av_frame_apply_cropping(cropped.get(), AV_FRAME_CROP_UNALIGNED),
                                       "video-preprocess", "apply input frame cropping");
            input = cropped.get();
            validateInput(*input);
        }
        const auto color = colors(*input);
        const auto layout = geometry(*input, options);
        media::FramePtr scaled;
        if (input->format == AV_PIX_FMT_YUV420P && input->width == layout.width && input->height == layout.height) {
            // 格式 / 尺寸已兼容时只增加引用；叠字需要写入时才由 FFmpeg 复制缓冲。
            scaled = media::referenceFrame(*input);
        } else {
            scaled = allocateFrame(layout.width, layout.height, *input);
            scaler.reset(sws_getCachedContext(scaler.release(), input->width, input->height,
                static_cast<AVPixelFormat>(input->format), layout.width, layout.height,
                AV_PIX_FMT_YUV420P, flags, nullptr, nullptr, nullptr));
            if (!scaler) invalid("cannot create video scaling / pixel-conversion context");
            const auto* coefficients = sws_getCoefficients(color.coefficients);
            error::throwIfFFmpegFailed(sws_setColorspaceDetails(scaler.get(), coefficients,
                color.sourceFullRange ? 1 : 0, coefficients, color.range == AVCOL_RANGE_JPEG ? 1 : 0,
                0, 1 << 16, 1 << 16), "video-preprocess", "configure scaling color matrix / range");
            const int rows = sws_scale(scaler.get(), input->data, input->linesize, 0, input->height,
                                        scaled->data, scaled->linesize);
            if (rows != layout.height) invalid("video scaling did not produce a complete frame");
        }
        setOutputProperties(*scaled, color);
        media::FramePtr output;
        if (options.rotation == 0 && !options.mirror && layout.width == options.width && layout.height == options.height) {
            output = std::move(scaled);
        } else {
            output = allocateFrame(options.width, options.height, *scaled);
            setOutputProperties(*output, color);
            transform(*scaled, *output, layout, options);
        }
        const auto sar = source.sample_aspect_ratio;
        const bool geometryChanged = source.crop_left || source.crop_right || source.crop_top || source.crop_bottom ||
            source.width != options.width || source.height != options.height || options.rotation != 0 || options.mirror ||
            layout.width != options.width || layout.height != options.height ||
            (sar.num > 0 && sar.den > 0 && sar.num != sar.den);
        clearGeometrySideData(*output, geometryChanged);
        if (!options.watermark.empty()) drawText(*output, options.watermark, false);
        return output;
    }

    media::FramePtr outputFrame(const AVFrame& source, std::int64_t pts) const {
        auto frame = media::referenceFrame(source);
        frame->pts = pts;
        frame->time_base = AVRational{1, options.fps};
        frame->duration = 1;
        if (options.timestampOverlay) drawText(*frame, elapsedText(pts, options.fps), true);
        return frame;
    }

    void countInput(std::int64_t timestampUs) noexcept {
        if (counters.inputFrames == 0) firstInputUs = lastInputUs = timestampUs;
        else lastInputUs = std::max(lastInputUs, timestampUs);
        ++counters.inputFrames;
    }
};

VideoPreprocessor::VideoPreprocessor(VideoPreprocessOptions options, std::int64_t originUs)
    : impl_(std::make_unique<Impl>(std::move(options), originUs)) {}

VideoPreprocessor::~VideoPreprocessor() = default;

std::vector<media::FramePtr> VideoPreprocessor::process(const AVFrame& frame, std::int64_t timestampUs) {
    auto& state = *impl_;
    if (state.flushed) invalid("cannot process video after flush");
    validateInput(frame);
    const auto pts = state.ptsFor(timestampUs);
    if (pts <= state.lastPts) {
        state.countInput(timestampUs);
        ++state.counters.droppedFrames;
        return {};
    }
    auto current = state.prepare(frame);
    const std::int64_t missing = state.previous ? pts - state.lastPts - 1 : 0;
    // 单次最多补一秒旧画面。长暂停只补靠近新帧的槽，不把暂停压缩成假连续时间。
    std::int64_t duplicates = std::min(missing, static_cast<std::int64_t>(state.options.fps));
    if (state.options.timestampOverlay) {
        // 每张时长不同，make_writable 会复制整帧。按实际对齐帧大小控制本批像素预算，
        // 避免 8K / 高帧率长缺口一次复制数 GB；超预算的单张当前帧仍允许输出。
        const int frameBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
            state.options.width, state.options.height, 32);
        error::throwIfFFmpegFailed(frameBytes, "video-preprocess", "calculate timestamp overlay frame size");
        const auto batchFrames = std::max<std::int64_t>(1, overlayOutputPixelBudget / frameBytes);
        duplicates = std::min(duplicates, batchFrames - 1);
    }
    std::vector<media::FramePtr> output;
    output.reserve(static_cast<std::size_t>(duplicates) + 1);
    for (auto index = pts - duplicates; index < pts; ++index) {
        output.push_back(state.outputFrame(*state.previous, index));
    }
    output.push_back(state.outputFrame(*current, pts));
    state.countInput(timestampUs);
    if (state.counters.outputFrames == 0) state.firstPts = output.front()->pts;
    state.counters.outputFrames += output.size();
    state.counters.duplicatedFrames += static_cast<std::uint64_t>(duplicates);
    state.counters.skippedTimelineFrames += static_cast<std::uint64_t>(missing - duplicates);
    state.lastPts = pts;
    state.previous = std::move(current);
    return output;
}

std::vector<media::FramePtr> VideoPreprocessor::flush() {
    impl_->flushed = true;
    impl_->previous.reset();
    return {};
}

VideoPreprocessStats VideoPreprocessor::stats() const noexcept {
    const auto& state = *impl_;
    auto result = state.counters;
    const long double inputSpan = static_cast<long double>(state.lastInputUs) - state.firstInputUs;
    if (result.inputFrames > 1 && inputSpan > 0) {
        result.inputFps = static_cast<double>((result.inputFrames - 1) * 1000000.0L / inputSpan);
    }
    const auto outputSpan = state.lastPts - state.firstPts;
    if (result.outputFrames > 1 && outputSpan > 0) {
        result.outputFps = static_cast<double>((result.outputFrames - 1) * static_cast<long double>(state.options.fps) / outputSpan);
    }
    return result;
}

} // namespace lsl::preprocess
