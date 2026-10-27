#pragma once

#include "common/config/CommandLine.hpp"
#include "common/stats/PerformanceStats.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace lsl::audio { class IAudioSink; }
namespace lsl::renderer { class IVideoRenderer; }

namespace lsl::player {

struct PlayerPipelineControls {
    bool muted = false;
    bool paused = false;
    int volume = 100;
    std::uint64_t revision = 0;
};

enum class PlayerPipelineState { Connecting, Buffering, Playing, Paused, Reconnecting };

struct PlayerPipelineSnapshot {
    PlayerPipelineState state = PlayerPipelineState::Connecting;
    PlayerPipelineControls controls;
    std::uint64_t serial = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    stats::PlayerPerformance performance;
};

using PlayerPipelineCallback = std::function<void(const PlayerPipelineSnapshot&)>;
using PlayerControlsReader = std::function<PlayerPipelineControls()>;

// SDL 音频子系统必须在调用者主线程构造/析构；流操作本身由现有 sink 锁保护。
std::unique_ptr<audio::IAudioSink> createPlayerAudioSink(const config::PlayerConfig& config,
                                                      logging::Logger& logger);

// 无进程信号/具体窗口/SDL 子系统初始化。renderer 由当前协调线程独占。
int runPlayerPipeline(const config::PlayerConfig& config, logging::Logger& logger,
                      renderer::IVideoRenderer& display, const std::atomic<bool>& stop,
                      audio::IAudioSink* audioOutput = nullptr,
                      const PlayerPipelineCallback& callback = {},
                      const PlayerControlsReader& controlsReader = {});

int runPlayer(const config::PlayerConfig& config, const config::CommandLineOptions& options,
              logging::Logger& logger);

} // namespace lsl::player
