# FFmpeg >= 6: FFmpeg_ROOT points to an SDK with include/, lib/, and bin/.
set(FFmpeg_ROOT "" CACHE PATH "FFmpeg SDK root (headers, libraries, DLLs)")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND AND NOT FFmpeg_ROOT)
    pkg_check_modules(PC_FFMPEG QUIET IMPORTED_TARGET
        libavdevice>=60 libavformat>=60 libavcodec>=60 libavutil>=58 libswscale>=7 libswresample>=4)
endif()

if(PC_FFMPEG_FOUND)
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    set_property(TARGET FFmpeg::FFmpeg PROPERTY INTERFACE_LINK_LIBRARIES PkgConfig::PC_FFMPEG)
    set(FFmpeg_FOUND TRUE)
else()
    find_path(FFmpeg_INCLUDE_DIR libavformat/avformat.h
        HINTS "${FFmpeg_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES include)
    set(_ffmpeg_libraries)
    set(_ffmpeg_required FFmpeg_INCLUDE_DIR)
    foreach(component avdevice avformat avcodec swresample swscale avutil)
        find_library(FFmpeg_${component}_LIBRARY NAMES ${component}
            HINTS "${FFmpeg_ROOT}" ENV FFMPEG_ROOT PATH_SUFFIXES lib)
        list(APPEND _ffmpeg_libraries "${FFmpeg_${component}_LIBRARY}")
        list(APPEND _ffmpeg_required FFmpeg_${component}_LIBRARY)
    endforeach()
    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(FFmpeg REQUIRED_VARS ${_ffmpeg_required}
        REASON_FAILURE_MESSAGE "Set -DFFmpeg_ROOT=<SDK directory>, or install FFmpeg development packages and pkg-config.")
    if(FFmpeg_FOUND)
        add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
        set_target_properties(FFmpeg::FFmpeg PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_ffmpeg_libraries}")
        get_filename_component(_ffmpeg_prefix "${FFmpeg_INCLUDE_DIR}" DIRECTORY)
        file(GLOB FFmpeg_RUNTIME_DLLS "${_ffmpeg_prefix}/bin/*.dll")
    endif()
endif()
