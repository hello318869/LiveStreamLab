#pragma once

#include <cstdint>

struct AVFrame;

namespace lsl::renderer {

struct VideoRenderOptions {
    int width = 1280;
    int height = 720;
    bool vsync = true;
    bool fullscreen = false;
};

struct VideoRenderStats {
    std::uint64_t renderedFrames = 0;
    std::uint64_t textureUploads = 0;
    std::uint64_t droppedFrames = 0;
    std::uint64_t renderTimeUs = 0; // 后端 render 调用耗时；SDL 后端包含上传、绘制及垂直同步等待。
    std::uint64_t textureRebuilds = 0;
};

struct AudioControls {
    bool togglePaused = false;
    bool toggleMuted = false;
    int volumeDelta = 0;
};

// 由一个协调线程独占。窗口后端必须在应用主线程调用；无窗口的帧交接后端可在后台协调线程使用。
// 同步/流水线只依赖此接口，后端不能跨线程直接更新 UI 控件。
class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;
    virtual bool pollEvents() = 0; // 关闭窗口或 Esc 后返回 false。
    virtual AudioControls takeAudioControls() = 0;
    virtual void render(const AVFrame& frame) = 0;
    virtual void clear() = 0; // 换代时撤下上一会话的画面。
    virtual void noteDroppedFrames(std::uint64_t count) = 0;
    virtual VideoRenderStats stats() const = 0;
};

} // namespace lsl::renderer
