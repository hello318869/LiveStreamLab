#pragma once

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_video.h>
#include <memory>

namespace lsl::media {
namespace detail {

template <auto Destroy>
struct SDLDeleter {
    template <typename T>
    void operator()(T* ptr) const noexcept { Destroy(ptr); }
};

} // namespace detail

using WindowPtr = std::unique_ptr<SDL_Window, detail::SDLDeleter<SDL_DestroyWindow>>;
using RendererPtr = std::unique_ptr<SDL_Renderer, detail::SDLDeleter<SDL_DestroyRenderer>>;
using TexturePtr = std::unique_ptr<SDL_Texture, detail::SDLDeleter<SDL_DestroyTexture>>;
// SDL_OpenAudioDeviceStream 返回的流销毁时会一起关闭设备。
using AudioStreamPtr = std::unique_ptr<SDL_AudioStream, detail::SDLDeleter<SDL_DestroyAudioStream>>;

// 按 window、renderer、texture 顺序声明，确保反序析构；销毁后再 SDL_Quit。
// 窗口/渲染器/纹理在主线程销毁；音频回调的 userdata 必须活到音频流销毁之后。
} // namespace lsl::media
