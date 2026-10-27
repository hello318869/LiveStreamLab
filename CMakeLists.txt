cmake_minimum_required(VERSION 3.24)
project(LiveStreamLab VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

option(LSL_BUILD_APPS "Build publisher and player" ON)
option(LSL_BUILD_PUBLISHER_GUI "Build the M16.1 Qt Widgets publisher controls" ON)
option(LSL_BUILD_PLAYER_GUI "Build the M16.2 Qt Widgets viewer controls" ON)
option(LSL_BUILD_TESTS "Build tests" OFF)
option(LSL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LiveStreamLabModules)
find_package(Threads REQUIRED)

# 支持工程内 SDK 和本机 D:/ffmpeg；也可显式指定 FFmpeg_ROOT。
if(NOT FFmpeg_ROOT AND NOT "$ENV{FFMPEG_ROOT}")
    foreach(base external/ffmpeg ../ffmpeg ../../ffmpeg)
        get_filename_component(base "${base}" ABSOLUTE)
        file(GLOB versions "${base}/ffmpeg-*")
        foreach(sdk "${base}" ${versions})
            if(EXISTS "${sdk}/include/libavcodec/avcodec.h")
                set(FFmpeg_ROOT "${sdk}")
                break()
            endif()
        endforeach()
        if(FFmpeg_ROOT)
            break()
        endif()
    endforeach()
endif()
find_package(FFmpeg REQUIRED)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/SDL/CMakeLists.txt")
    add_subdirectory(external/SDL EXCLUDE_FROM_ALL)
else()
    find_package(SDL3 3.2 CONFIG REQUIRED)
endif()

# 只注册已有实现，后续模块有代码时再添加目标。
add_library(lsl_common STATIC
    src/common/stats/CaptureTimestamp.cpp
    src/common/stats/PerformanceStats.hpp
    src/common/queue/BoundedBlockingQueue.hpp
    src/common/utils/NetworkStatus.hpp
    src/common/config/Config.cpp
    src/common/config/CommandLine.cpp
    src/common/logger/Logger.cpp
    src/common/error/Error.cpp)
target_include_directories(lsl_common PUBLIC src)
target_include_directories(lsl_common SYSTEM PRIVATE external/json/include)
target_link_libraries(lsl_common PUBLIC FFmpeg::FFmpeg Threads::Threads)
if(WIN32)
    target_link_libraries(lsl_common PRIVATE shell32)
endif()
lsl_set_warnings(lsl_common)

add_library(lsl_preprocess STATIC
    src/publisher/preprocess/VideoPreprocessor.cpp
    src/publisher/preprocess/AudioPreprocessor.cpp)
target_link_libraries(lsl_preprocess PUBLIC lsl_common)
lsl_set_warnings(lsl_preprocess)

add_library(lsl_video_encoder STATIC src/publisher/encoder/VideoEncoder.cpp)
target_link_libraries(lsl_video_encoder PUBLIC lsl_common)
lsl_set_warnings(lsl_video_encoder)

add_library(lsl_audio_encoder STATIC src/publisher/encoder/AudioEncoder.cpp)
target_link_libraries(lsl_audio_encoder PUBLIC lsl_common)
lsl_set_warnings(lsl_audio_encoder)

add_library(lsl_muxer STATIC
    src/publisher/muxer/VideoFileWriter.cpp
    src/publisher/muxer/AudioFileWriter.cpp)
target_link_libraries(lsl_muxer PUBLIC lsl_common)
lsl_set_warnings(lsl_muxer)

add_library(lsl_rtmp STATIC src/publisher/rtmp/RtmpPublisher.cpp)
target_link_libraries(lsl_rtmp PUBLIC lsl_common)
lsl_set_warnings(lsl_rtmp)

add_library(lsl_capture STATIC
    src/publisher/capture/DeviceManager.cpp
    src/publisher/capture/MediaInput.cpp
    src/publisher/capture/SystemAudioInput.cpp)
target_link_libraries(lsl_capture PUBLIC lsl_common)
if(WIN32)
    target_link_libraries(lsl_capture PRIVATE ole32 oleaut32 strmiids uuid)
endif()
lsl_set_warnings(lsl_capture)

# 流水线负责组合采集、预处理、编码及输出；采集模块不依赖下游。
add_library(lsl_publisher_pipeline STATIC
    src/publisher/pipeline/PublisherPipeline.cpp
    src/publisher/pipeline/PublisherSession.cpp)
target_link_libraries(lsl_publisher_pipeline PUBLIC lsl_common
    PRIVATE lsl_capture lsl_preprocess lsl_video_encoder lsl_audio_encoder lsl_muxer lsl_rtmp)
lsl_set_warnings(lsl_publisher_pipeline)

add_library(lsl_demuxer STATIC
    src/player/demuxer/Demuxer.cpp
    src/player/demuxer/PacketBuffer.cpp)
target_link_libraries(lsl_demuxer PUBLIC lsl_common)
lsl_set_warnings(lsl_demuxer)

add_library(lsl_video_decoder STATIC src/player/decoder/VideoDecoder.cpp)
target_link_libraries(lsl_video_decoder PUBLIC lsl_common)
lsl_set_warnings(lsl_video_decoder)

add_library(lsl_audio_decoder STATIC src/player/decoder/AudioDecoder.cpp)
target_link_libraries(lsl_audio_decoder PUBLIC lsl_common)
lsl_set_warnings(lsl_audio_decoder)

add_library(lsl_audio_sink STATIC src/player/audio/SDLAudioSink.cpp)
target_link_libraries(lsl_audio_sink PUBLIC lsl_common SDL3::SDL3)
lsl_set_warnings(lsl_audio_sink)

add_library(lsl_video_renderer STATIC src/player/renderer/SDLVideoRenderer.cpp)
target_link_libraries(lsl_video_renderer PUBLIC lsl_common SDL3::SDL3)
lsl_set_warnings(lsl_video_renderer)

add_library(lsl_sync STATIC src/player/sync/AVSynchronizer.cpp)
target_link_libraries(lsl_sync PUBLIC lsl_common)
lsl_set_warnings(lsl_sync)

add_library(lsl_player_pipeline STATIC
    src/player/pipeline/PlayerPipeline.cpp
    src/player/pipeline/PlayerSession.cpp
    src/player/pipeline/AudioPlayback.cpp)
target_link_libraries(lsl_player_pipeline PUBLIC lsl_common
    PRIVATE lsl_demuxer lsl_video_decoder lsl_video_renderer lsl_audio_decoder lsl_audio_sink lsl_sync)
lsl_set_warnings(lsl_player_pipeline)

if(LSL_BUILD_APPS)
    foreach(app publisher player)
        add_executable(${app} src/apps/${app}_main.cpp)
        target_link_libraries(${app} PRIVATE lsl_common)
        lsl_set_warnings(${app})
        lsl_copy_runtime(${app})
    endforeach()
    target_link_libraries(publisher PRIVATE lsl_publisher_pipeline)
    target_link_libraries(player PRIVATE lsl_player_pipeline SDL3::SDL3)

    if(LSL_BUILD_PUBLISHER_GUI OR LSL_BUILD_PLAYER_GUI)
        # 优先使用用户指定的 Qt；也支持工作区内安装的 Windows MSVC SDK。
        if(WIN32 AND NOT Qt6_DIR AND NOT Qt6_ROOT)
            file(GLOB lsl_qt_sdks "${CMAKE_CURRENT_SOURCE_DIR}/external/qt/*/msvc*_64")
            list(SORT lsl_qt_sdks COMPARE NATURAL ORDER DESCENDING)
            foreach(lsl_qt_sdk IN LISTS lsl_qt_sdks)
                if(EXISTS "${lsl_qt_sdk}/lib/cmake/Qt6/Qt6Config.cmake")
                    list(PREPEND CMAKE_PREFIX_PATH "${lsl_qt_sdk}")
                    break()
                endif()
            endforeach()
        endif()
        find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
        if(WIN32)
            find_program(LSL_WINDEPLOYQT_EXECUTABLE windeployqt
                HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
        endif()
    endif()

    if(LSL_BUILD_PUBLISHER_GUI)
        add_library(lsl_publisher_ui STATIC src/publisher/ui/PublisherControl.cpp)
        target_link_libraries(lsl_publisher_ui PUBLIC lsl_common
            PRIVATE lsl_publisher_pipeline lsl_capture Qt6::Widgets)
        lsl_set_warnings(lsl_publisher_ui)

        add_executable(publisher_gui src/apps/publisher_gui_main.cpp)
        target_link_libraries(publisher_gui PRIVATE lsl_publisher_ui lsl_publisher_pipeline)
        lsl_set_warnings(publisher_gui)
        lsl_copy_runtime(publisher_gui)
    endif()

    if(LSL_BUILD_PLAYER_GUI)
        add_library(lsl_player_ui STATIC src/player/ui/PlayerControl.cpp)
        target_link_libraries(lsl_player_ui PUBLIC lsl_common
            PRIVATE lsl_player_pipeline Qt6::Widgets SDL3::SDL3)
        lsl_set_warnings(lsl_player_ui)

        add_executable(player_gui src/apps/player_gui_main.cpp)
        target_link_libraries(player_gui PRIVATE lsl_player_ui lsl_player_pipeline)
        lsl_set_warnings(player_gui)
        lsl_copy_runtime(player_gui)
    endif()

    if(WIN32)
        # 部署只分析/复制依赖，不启动目标程序；保持构建全部由 CMake 管理。
        foreach(qt_app publisher_gui player_gui)
            if(TARGET ${qt_app})
                add_custom_command(TARGET ${qt_app} POST_BUILD
                    COMMAND "${LSL_WINDEPLOYQT_EXECUTABLE}"
                        "$<IF:$<CONFIG:Debug>,--debug,--release>"
                        --no-translations --no-system-d3d-compiler --no-compiler-runtime
                        --dir "$<TARGET_FILE_DIR:${qt_app}>" "$<TARGET_FILE:${qt_app}>"
                    VERBATIM)
            endif()
        endforeach()
        foreach(sdl_app player player_gui)
            if(TARGET ${sdl_app})
                add_custom_command(TARGET ${sdl_app} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${sdl_app}>" VERBATIM)
            endif()
        endforeach()
    endif()
endif()

if(LSL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
