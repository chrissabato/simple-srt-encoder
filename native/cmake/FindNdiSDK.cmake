#[[
FindNdiSDK.cmake

Locates the proprietary NDI SDK (Vizrt/NDI), which is NOT redistributable and must be
downloaded separately from https://ndi.video/for-developers/ by each developer.

Search order:
  1. $ENV{NDI_SDK_DIR}
  2. ${CMAKE_SOURCE_DIR}/vendor/NdiSDK  (gitignored local drop location)

On success, defines:
  NdiSDK_FOUND
  NdiSDK_INCLUDE_DIR
  NdiSDK_LIBRARY        - Processing.NDI.Lib.x64.lib (import lib; the actual Processing.NDI.Lib.x64.dll
                           must be present on PATH or alongside the app at runtime)
]]

find_path(NdiSDK_INCLUDE_DIR
    NAMES Processing.NDI.Lib.h
    HINTS
        "$ENV{NDI_SDK_DIR}"
        "${CMAKE_SOURCE_DIR}/vendor/NdiSDK"
    PATH_SUFFIXES
        "Include"
        "include"
)

find_library(NdiSDK_LIBRARY
    NAMES Processing.NDI.Lib.x64
    HINTS
        "$ENV{NDI_SDK_DIR}"
        "${CMAKE_SOURCE_DIR}/vendor/NdiSDK"
    PATH_SUFFIXES
        "Lib/x64"
        "lib/x64"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NdiSDK
    REQUIRED_VARS NdiSDK_INCLUDE_DIR NdiSDK_LIBRARY
)

mark_as_advanced(NdiSDK_INCLUDE_DIR NdiSDK_LIBRARY)
