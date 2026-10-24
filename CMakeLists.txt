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

add_library(lsl_video_encode STATIC
    src/publisher/encode/VideoEncoder.cpp
    src/publisher/encode/VideoFileWriter.cpp)
target_link_libraries(lsl_video_encode PUBLIC lsl_common)
lsl_set_warnings(lsl_video_encode)

add_library(lsl_audio_encode STATIC
    src/publisher/encode/AudioEncoder.cpp
    src/publisher/encode/AudioFileWriter.cpp)
target_link_libraries(lsl_audio_encode PUBLIC lsl_common)
lsl_set_warnings(lsl_audio_encode)

add_library(lsl_capture STATIC
    src/publisher/capture/DeviceManager.cpp
    src/publisher/capture/MediaInput.cpp
    src/publisher/capture/SystemAudioInput.cpp
    src/publisher/capture/CaptureApp.cpp)
target_link_libraries(lsl_capture PUBLIC lsl_common PRIVATE lsl_preprocess lsl_video_encode lsl_audio_encode)
if(WIN32)
    target_link_libraries(lsl_capture PRIVATE ole32 oleaut32 strmiids uuid)
endif()
lsl_set_warnings(lsl_capture)

if(LSL_BUILD_APPS)
    foreach(app publisher player)
        add_executable(${app} src/apps/${app}_main.cpp)
        target_link_libraries(${app} PRIVATE lsl_common)
        lsl_set_warnings(${app})
        lsl_copy_runtime(${app})
    endforeach()
    target_link_libraries(publisher PRIVATE lsl_capture)
    target_link_libraries(player PRIVATE SDL3::SDL3)
    if(WIN32)
        add_custom_command(TARGET player POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:player>" VERBATIM)
    endif()
endif()

if(LSL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
