#pragma once

#include "player/renderer/IVideoRenderer.hpp"
#include "common/logger/Logger.hpp"

#include <memory>

namespace lsl::renderer {

class SDLVideoRenderer final : public IVideoRenderer {
public:
    SDLVideoRenderer(const VideoRenderOptions& options, logging::Logger& logger);
    ~SDLVideoRenderer() override;
    SDLVideoRenderer(const SDLVideoRenderer&) = delete;
    SDLVideoRenderer& operator=(const SDLVideoRenderer&) = delete;

    bool pollEvents() override;
    AudioControls takeAudioControls() override;
    void render(const AVFrame& frame) override;
    void clear() override;
    void noteDroppedFrames(std::uint64_t count) override;
    VideoRenderStats stats() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lsl::renderer
