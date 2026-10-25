#include "player/renderer/SDLVideoRenderer.hpp"
#include "common/media/FFmpegResources.hpp"
#include "common/media/SDLResources.hpp"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace lsl::renderer {
namespace {
using Clock = std::chrono::steady_clock;
constexpr int maximumDimension = 16384;
constexpr int maximumFrameBytes = 128 * 1024 * 1024;

[[noreturn]] void failSDL(const char* operation) {
    throw error::AppError("video-renderer", error::Io, std::string(operation) + ": " + SDL_GetError());
}

void requireSDL(bool result, const char* operation) {
    if (!result) failSDL(operation);
}

class VideoSubsystem {
public:
    VideoSubsystem() {
        // 保留现有普通 main；SDL3 不再由 SDL.h 自动引入 SDL_main.h。
        SDL_SetMainReady();
        requireSDL(SDL_InitSubSystem(SDL_INIT_VIDEO), "initialize SDL video");
    }
    ~VideoSubsystem() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
    VideoSubsystem(const VideoSubsystem&) = delete;
    VideoSubsystem& operator=(const VideoSubsystem&) = delete;
};

class Properties {
public:
    Properties() : value_(SDL_CreateProperties()) {
        if (!value_) failSDL("create texture properties");
    }
    ~Properties() { SDL_DestroyProperties(value_); }
    void number(const char* key, Sint64 value) {
        requireSDL(SDL_SetNumberProperty(value_, key, value), "set texture property");
    }
    SDL_PropertiesID get() const noexcept { return value_; }
private:
    SDL_PropertiesID value_;
};

SDL_Colorspace bt2020ColorspaceOf(const AVFrame& frame, bool full) {
    // FFmpeg 与 SDL 的 primaries/transfer 都采用 H.273 编号。只接受双方已有的
    // 标准编号，避免把 FFmpeg 扩展值截入 SDL 的五位字段。未标注时按 SDR BT2020。
    int primaries = static_cast<int>(frame.color_primaries);
    if (primaries == AVCOL_PRI_UNSPECIFIED) primaries = SDL_COLOR_PRIMARIES_BT2020;
    if (!(primaries == 1 || (primaries >= 4 && primaries <= 12) || primaries == 22)) {
        throw error::AppError("video-renderer", error::NotImplemented, "unsupported BT2020 color primaries");
    }
    int transfer = static_cast<int>(frame.color_trc);
    if (transfer == AVCOL_TRC_UNSPECIFIED) transfer = SDL_TRANSFER_CHARACTERISTICS_BT2020_10BIT;
    if (!(transfer == 1 || (transfer >= 4 && transfer <= 18))) {
        throw error::AppError("video-renderer", error::NotImplemented, "unsupported BT2020 transfer characteristics");
    }
    SDL_ChromaLocation chroma;
    switch (frame.chroma_location) {
    case AVCHROMA_LOC_UNSPECIFIED:
    case AVCHROMA_LOC_LEFT: chroma = SDL_CHROMA_LOCATION_LEFT; break;
    case AVCHROMA_LOC_CENTER: chroma = SDL_CHROMA_LOCATION_CENTER; break;
    case AVCHROMA_LOC_TOPLEFT: chroma = SDL_CHROMA_LOCATION_TOPLEFT; break;
    default:
        throw error::AppError("video-renderer", error::NotImplemented, "unsupported BT2020 chroma sample location");
    }
    // SDL_COLORSPACE_BT2020_* 常量隐含 PQ，不能用于所有 BT2020 输入。
    // 此处仅准确标记输入；本模块不执行 HDR tone mapping 或高位深重建。
    return static_cast<SDL_Colorspace>(SDL_DEFINE_COLORSPACE(SDL_COLOR_TYPE_YCBCR,
        full ? SDL_COLOR_RANGE_FULL : SDL_COLOR_RANGE_LIMITED,
        static_cast<SDL_ColorPrimaries>(primaries), static_cast<SDL_TransferCharacteristics>(transfer),
        SDL_MATRIX_COEFFICIENTS_BT2020_NCL, chroma));
}

SDL_Colorspace colorspaceOf(const AVFrame& frame) {
    const bool full = frame.color_range == AVCOL_RANGE_JPEG;
    switch (frame.colorspace) {
    case AVCOL_SPC_BT709:
        return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    case AVCOL_SPC_BT2020_NCL:
        return bt2020ColorspaceOf(frame, full);
    case AVCOL_SPC_BT2020_CL:
        throw error::AppError("video-renderer", error::NotImplemented,
            "BT2020 constant-luminance video is unsupported; convert it to a supported non-constant-luminance matrix first");
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    default:
        // 未标注矩阵时沿用常见 SD/HD 规则；未标注范围按视频有限范围。
        if (frame.height > 576) return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    }
}

SDL_PixelFormat pixelFormatOf(const AVFrame& frame) {
    if (frame.format == AV_PIX_FMT_YUV420P) return SDL_PIXELFORMAT_IYUV;
    if (frame.format == AV_PIX_FMT_NV12) return SDL_PIXELFORMAT_NV12;
    throw error::AppError("video-renderer", error::InvalidConfig,
                          "renderer requires YUV420P or NV12 software frames");
}

bool validatePlanes(const AVFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.width > maximumDimension || frame.height > maximumDimension) {
        throw error::AppError("video-renderer", error::InvalidConfig, "invalid video frame dimensions");
    }
    const auto format = static_cast<AVPixelFormat>(frame.format);
    const int bytes = av_image_get_buffer_size(format, frame.width, frame.height, 32);
    if (bytes < 0 || bytes > maximumFrameBytes) {
        throw error::AppError("video-renderer", error::InvalidConfig, "video frame exceeds upload memory limit");
    }
    const int planes = format == AV_PIX_FMT_NV12 ? 2 : 3;
    bool negative = false;
    for (int plane = 0; plane < planes; ++plane) {
        const auto pitch = static_cast<std::int64_t>(frame.linesize[plane]);
        const auto rowBytes = plane == 0 ? frame.width :
            ((frame.width + 1) / 2) * (format == AV_PIX_FMT_NV12 ? 2 : 1);
        if (!frame.data[plane] || (pitch >= 0 ? pitch : -pitch) < rowBytes) {
            throw error::AppError("video-renderer", error::InvalidConfig, "invalid video frame plane or pitch");
        }
        negative = negative || pitch < 0;
    }
    return negative;
}
} // namespace

struct SDLVideoRenderer::Impl {
    // 声明顺序保证 texture -> renderer -> window -> SDL 视频子系统依次释放。
    VideoSubsystem video;
    logging::Logger& logger;
    const std::thread::id owner = std::this_thread::get_id();
    media::WindowPtr window;
    media::RendererPtr renderer;
    media::TexturePtr texture;
    media::FramePtr uploadFrame;
    VideoRenderStats counters;
    AudioControls audioControls;
    SDL_WindowID windowId = 0;
    int textureWidth = 0;
    int textureHeight = 0;
    SDL_PixelFormat textureFormat = SDL_PIXELFORMAT_UNKNOWN;
    SDL_Colorspace textureColorspace = SDL_COLORSPACE_UNKNOWN;
    int logicalWidth = 0;
    int logicalHeight = 0;
    bool closed = false;

    Impl(const VideoRenderOptions& options, logging::Logger& output) : logger(output) {
        if (options.width <= 0 || options.height <= 0 || options.width > maximumDimension ||
            options.height > maximumDimension) {
            throw error::AppError("video-renderer", error::InvalidConfig, "invalid initial window dimensions");
        }
        window.reset(SDL_CreateWindow("LiveStreamLab - video", options.width, options.height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
        if (!window) failSDL("create video window");
        windowId = SDL_GetWindowID(window.get());

        // SDL3 按驱动名选择后端。先按 SDL 的优先级尝试硬件后端，再降级 software。
        std::string drivers;
        for (int index = 0; index < SDL_GetNumRenderDrivers(); ++index) {
            const auto* name = SDL_GetRenderDriver(index);
            if (name && std::string(name) != "software") {
                if (!drivers.empty()) drivers += ',';
                drivers += name;
            }
        }
        if (!drivers.empty()) renderer.reset(SDL_CreateRenderer(window.get(), drivers.c_str()));
        if (!renderer) {
            logger.log(logging::Level::Warn, "video-renderer", "hardware renderer unavailable; trying SDL software renderer");
            renderer.reset(SDL_CreateRenderer(window.get(), "software"));
        }
        if (!renderer) failSDL("create video renderer");
        if (!SDL_SetRenderVSync(renderer.get(), options.vsync ? 1 : 0)) {
            logger.log(logging::Level::Warn, "video-renderer", std::string("cannot set requested vsync: ") + SDL_GetError());
            if (options.vsync && !SDL_SetRenderVSync(renderer.get(), 0)) {
                logger.log(logging::Level::Warn, "video-renderer", std::string("cannot disable vsync: ") + SDL_GetError());
            }
        }
        requireSDL(SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255), "set video background");
        if (options.fullscreen) requireSDL(SDL_SetWindowFullscreen(window.get(), true), "enter fullscreen");
        draw();
        const auto* name = SDL_GetRendererName(renderer.get());
        logger.log(logging::Level::Info, "video-renderer", std::string("SDL renderer=") + (name ? name : "unknown") +
                   "; F / Alt+Enter / double-click toggles fullscreen; Esc closes");
    }

    void checkThread() const {
        if (owner != std::this_thread::get_id()) {
            throw error::AppError("video-renderer", error::Unexpected, "SDL video must be used on its creating main thread");
        }
    }

    void draw() {
        requireSDL(SDL_RenderClear(renderer.get()), "clear video background");
        if (texture) requireSDL(SDL_RenderTexture(renderer.get(), texture.get(), nullptr, nullptr), "draw video texture");
        requireSDL(SDL_RenderPresent(renderer.get()), "present video");
    }

    void toggleFullscreen() {
        const bool fullscreen = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_FULLSCREEN) != 0;
        if (!SDL_SetWindowFullscreen(window.get(), !fullscreen)) {
            logger.log(logging::Level::Warn, "video-renderer", std::string("fullscreen switch failed: ") + SDL_GetError());
        }
    }

    void ensureTexture(const AVFrame& frame, SDL_PixelFormat format) {
        const auto colorspace = colorspaceOf(frame);
        if (!texture || frame.width != textureWidth || frame.height != textureHeight ||
            format != textureFormat || colorspace != textureColorspace) {
            Properties properties;
            properties.number(SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
            properties.number(SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
            properties.number(SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame.width);
            properties.number(SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame.height);
            properties.number(SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
            media::TexturePtr replacement{SDL_CreateTextureWithProperties(renderer.get(), properties.get())};
            if (!replacement) failSDL("create video texture");
            requireSDL(SDL_SetTextureScaleMode(replacement.get(), SDL_SCALEMODE_LINEAR), "set video scaling");
            texture = std::move(replacement);
            textureWidth = frame.width;
            textureHeight = frame.height;
            textureFormat = format;
            textureColorspace = colorspace;
            ++counters.textureRebuilds;
        }

        // sample_aspect_ratio 描述像素形状；逻辑显示尺寸同时考虑它与编码宽高。
        double width = static_cast<double>(frame.width);
        double height = static_cast<double>(frame.height);
        if (frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0) {
            width *= static_cast<double>(frame.sample_aspect_ratio.num) / frame.sample_aspect_ratio.den;
        }
        const double scale = std::min(1.0, maximumDimension / std::max(width, height));
        const int displayWidth = std::max(1, static_cast<int>(std::llround(width * scale)));
        const int displayHeight = std::max(1, static_cast<int>(std::llround(height * scale)));
        if (displayWidth != logicalWidth || displayHeight != logicalHeight) {
            requireSDL(SDL_SetRenderLogicalPresentation(renderer.get(), displayWidth, displayHeight,
                                                       SDL_LOGICAL_PRESENTATION_LETTERBOX), "set video aspect ratio");
            logicalWidth = displayWidth;
            logicalHeight = displayHeight;
        }
    }

    const AVFrame& positivePitchFrame(const AVFrame& frame, bool negativePitch) {
        if (!negativePitch) return frame;
        // FFmpeg 允许首指针位于最后一行并使用负步长；SDL 上传接口要求正 pitch。
        // av_frame_copy 按源步长逐行读取，保留正确行序，包含奇数宽高的末行色度。
        if (!uploadFrame || uploadFrame->width != frame.width || uploadFrame->height != frame.height ||
            uploadFrame->format != frame.format) {
            media::FramePtr replacement{av_frame_alloc()};
            if (!replacement) throw std::bad_alloc{};
            replacement->width = frame.width;
            replacement->height = frame.height;
            replacement->format = frame.format;
            error::throwIfFFmpegFailed(av_frame_get_buffer(replacement.get(), 32), "video-renderer", "allocate upload frame");
            uploadFrame = std::move(replacement);
        }
        error::throwIfFFmpegFailed(av_frame_copy(uploadFrame.get(), &frame), "video-renderer", "normalize video plane pitch");
        return *uploadFrame;
    }
};

SDLVideoRenderer::SDLVideoRenderer(const VideoRenderOptions& options, logging::Logger& logger)
    : impl_(std::make_unique<Impl>(options, logger)) {}
SDLVideoRenderer::~SDLVideoRenderer() = default;

bool SDLVideoRenderer::pollEvents() {
    impl_->checkThread();
    bool redraw = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            impl_->closed = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == impl_->windowId) impl_->closed = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.windowID != impl_->windowId || event.key.repeat) break;
            if (event.key.key == SDLK_ESCAPE) impl_->closed = true;
            if (event.key.key == SDLK_SPACE) impl_->audioControls.togglePaused = !impl_->audioControls.togglePaused;
            if (event.key.key == SDLK_M) impl_->audioControls.toggleMuted = !impl_->audioControls.toggleMuted;
            if (event.key.key == SDLK_UP) impl_->audioControls.volumeDelta += 10;
            if (event.key.key == SDLK_DOWN) impl_->audioControls.volumeDelta -= 10;
            if (event.key.key == SDLK_F || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT))) {
                impl_->toggleFullscreen();
                redraw = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.windowID == impl_->windowId && event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2) {
                impl_->toggleFullscreen();
                redraw = true;
            }
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            redraw = redraw || event.window.windowID == impl_->windowId;
            break;
        case SDL_EVENT_RENDER_DEVICE_RESET:
            // 已有纹理失效；下一帧重建，等待期间显示黑色而不沿用旧设备资源。
            impl_->texture.reset();
            redraw = true;
            break;
        default:
            break;
        }
    }
    if (redraw && !impl_->closed) impl_->draw();
    return !impl_->closed;
}

AudioControls SDLVideoRenderer::takeAudioControls() {
    impl_->checkThread();
    const auto result = impl_->audioControls;
    impl_->audioControls = {};
    return result;
}

void SDLVideoRenderer::render(const AVFrame& frame) {
    impl_->checkThread();
    if (impl_->closed) return;
    const auto begin = Clock::now();
    const auto format = pixelFormatOf(frame);
    const bool negativePitch = validatePlanes(frame);
    impl_->ensureTexture(frame, format);
    const auto& upload = impl_->positivePitchFrame(frame, negativePitch);
    if (format == SDL_PIXELFORMAT_IYUV) {
        requireSDL(SDL_UpdateYUVTexture(impl_->texture.get(), nullptr,
            upload.data[0], upload.linesize[0], upload.data[1], upload.linesize[1],
            upload.data[2], upload.linesize[2]), "upload YUV420P video");
    } else {
        requireSDL(SDL_UpdateNVTexture(impl_->texture.get(), nullptr,
            upload.data[0], upload.linesize[0], upload.data[1], upload.linesize[1]), "upload NV12 video");
    }
    ++impl_->counters.textureUploads;
    impl_->draw();
    ++impl_->counters.renderedFrames;
    impl_->counters.renderTimeUs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
}

void SDLVideoRenderer::clear() {
    impl_->checkThread();
    impl_->texture.reset();
    impl_->uploadFrame.reset();
    if (!impl_->closed) impl_->draw();
}

void SDLVideoRenderer::noteDroppedFrames(std::uint64_t count) {
    impl_->checkThread();
    impl_->counters.droppedFrames += count;
}

VideoRenderStats SDLVideoRenderer::stats() const {
    impl_->checkThread();
    return impl_->counters;
}

} // namespace lsl::renderer
