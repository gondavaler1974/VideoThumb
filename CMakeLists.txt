cmake_minimum_required(VERSION 3.25)
project(VideoThumb VERSION 1.4.3 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "VideoThumb is a Windows/Total Commander WLX plugin.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build x64 only.")
endif()

# The SDK downloader extracts a BtbN shared build under ffmpeg-sdk/<folder>.
if(NOT DEFINED FFMPEG_ROOT OR FFMPEG_ROOT STREQUAL "")
    file(GLOB _ffmpeg_roots LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/ffmpeg-sdk/*")

    foreach(_root IN LISTS _ffmpeg_roots)
        if(EXISTS "${_root}/include/libavcodec/avcodec.h")
            set(
                FFMPEG_ROOT
                "${_root}"
                CACHE PATH
                "FFmpeg SDK root"
                FORCE
            )
            break()
        endif()
    endforeach()
endif()

if(
    NOT FFMPEG_ROOT
    OR NOT EXISTS "${FFMPEG_ROOT}/include/libavcodec/avcodec.h"
)
    message(
        FATAL_ERROR
        "FFmpeg headers not found. Run scripts/get_ffmpeg_sdk.ps1 first, "
        "or configure with -DFFMPEG_ROOT=C:/path/to/ffmpeg-root"
    )
endif()

# ZIP reader: pinned miniz release, compiled statically into VideoThumb.
include(FetchContent)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(miniz)

# Read-only ZIP usage only.
target_compile_definitions(
    miniz
    PRIVATE
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
        MINIZ_NO_ZLIB_APIS
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES
)

add_library(
    VideoThumb
    SHARED
    src/VideoThumb.cpp
)

target_compile_features(
    VideoThumb
    PRIVATE
        cxx_std_20
)

# Important:
# On MSVC, do NOT also add FFmpeg with target_include_directories().
#
# /external:I marks the FFmpeg headers as third-party/external headers.
# Their diagnostics therefore use the external warning level, while
# VideoThumb's own code remains compiled with /W4.
if(MSVC)
    target_compile_options(
        VideoThumb
        PRIVATE
            "/external:I${FFMPEG_ROOT}/include"
            /external:W0
            /O2
            /Oi
            /GL
            /EHsc
            /MD
            /W4
    )
else()
    target_include_directories(
        VideoThumb
        SYSTEM
        PRIVATE
            "${FFMPEG_ROOT}/include"
    )
endif()

target_compile_definitions(
    VideoThumb
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        MINIZ_NO_STDIO
        MINIZ_NO_TIME
        MINIZ_NO_ZLIB_APIS
        MINIZ_NO_ZLIB_COMPATIBLE_NAMES
)

target_link_libraries(
    VideoThumb
    PRIVATE
        kernel32
        user32
        gdi32
        ole32
        windowscodecs
        miniz
)

if(MSVC)
    target_link_options(
        VideoThumb
        PRIVATE
            /LTCG
            /OPT:REF
            /OPT:ICF
    )
endif()

set_target_properties(
    VideoThumb
    PROPERTIES
        PREFIX ""
        SUFFIX ".wlx64"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)

# Copy only the FFmpeg DLL families that VideoThumb actually loads.
file(
    GLOB
    FFMPEG_RUNTIME_DLLS
    CONFIGURE_DEPENDS
        "${FFMPEG_ROOT}/bin/avutil-*.dll"
        "${FFMPEG_ROOT}/bin/swresample-*.dll"
        "${FFMPEG_ROOT}/bin/avcodec-*.dll"
        "${FFMPEG_ROOT}/bin/avformat-*.dll"
        "${FFMPEG_ROOT}/bin/swscale-*.dll"
)

add_custom_command(
    TARGET VideoThumb
    POST_BUILD

    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        "$<TARGET_FILE_DIR:VideoThumb>"

    COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_if_different
        "${CMAKE_SOURCE_DIR}/VideoThumb.ini"
        "$<TARGET_FILE_DIR:VideoThumb>/VideoThumb.ini"

    COMMAND
        ${CMAKE_COMMAND}
        -E
        copy_if_different
        ${FFMPEG_RUNTIME_DLLS}
        "$<TARGET_FILE_DIR:VideoThumb>"

    VERBATIM
)

message(STATUS "FFMPEG_ROOT=${FFMPEG_ROOT}")
message(STATUS "Output: ${CMAKE_BINARY_DIR}/dist")