# Dear ImGui dependency

- Upstream: https://github.com/ocornut/imgui
- Release: **v1.93.0** (unmodified upstream sources).
- Source archive: https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0.zip
- Downloaded from: https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.93.0
- Downloaded archive SHA-256: `e21907fd8dd5ea59aa00538632a02ad0a3ededb4ddc533b94702331fbcd6c470`
- License: MIT; see `LICENSE.txt`.

Only the core implementation/headers, bundled stb headers, and official SDL3 +
SDL_Renderer3 backends are included. Demos, examples, and test tools are omitted.
The project builds these files through its root CMakeLists.txt. No network
download, separate build script, or installed ImGui package is required.

Chinese fonts are loaded from the user's operating system at runtime; no system
font files are redistributed with this dependency.
