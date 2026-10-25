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
    std::uint64_t renderTimeUs = 0; // 上传、绘制与提交耗时，包含垂直同步等待。
    std::uint64_t textureRebuilds = 0;
};

struct AudioControls {
    bool togglePaused = false;
    bool toggleMuted = false;
    int volumeDelta = 0;
};

// 窗口事件与视频显示由主线程调用；同步/流水线只依赖此接口。
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
