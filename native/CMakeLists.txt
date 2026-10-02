cmake_minimum_required(VERSION 3.26)
# C is needed alongside CXX for DeckLinkAPI_i.c — the plain-C file midl.exe generates
# from the DeckLink SDK's .idl (see CaptureCore/CMakeLists.txt). Without C enabled here,
# CMake has no compiler configured for that file and silently drops it from the build
# entirely (no error at configure time — it just never appears in the link line, which
# then fails on unresolved GUID externs).
project(SrtEncoderNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_subdirectory(CaptureCore)
