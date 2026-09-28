set(CAPTURECORE_SOURCES
    src/CaptureCoreApi.cpp
    src/capture/CaptureManager.cpp
    src/capture/uvc/UvcDeviceEnumerator.cpp
    src/capture/uvc/UvcCaptureSource.cpp
    src/encode/SrtUrlBuilder.cpp
    src/encode/FramePipeWriter.cpp
    src/encode/FfmpegProcessController.cpp
)

# --- Optional backends -------------------------------------------------------
# DeckLink and NDI are proprietary SDKs that most developer machines won't have
# installed. AUTO detection means: build them if found, silently skip if not,
# so the rest of the app is never blocked on obtaining these SDKs.

set(ENABLE_DECKLINK "AUTO" CACHE STRING "Build the Blackmagic DeckLink capture backend (ON/OFF/AUTO)")
set(ENABLE_NDI "AUTO" CACHE STRING "Build the NDI capture backend (ON/OFF/AUTO)")

find_package(DeckLinkSDK QUIET)
find_package(NdiSDK QUIET)

if(ENABLE_DECKLINK STREQUAL "AUTO")
    set(ENABLE_DECKLINK ${DeckLinkSDK_FOUND})
endif()
if(ENABLE_NDI STREQUAL "AUTO")
    set(ENABLE_NDI ${NdiSDK_FOUND})
endif()

add_library(CaptureCore SHARED ${CAPTURECORE_SOURCES})

target_include_directories(CaptureCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        src
)

target_compile_definitions(CaptureCore PRIVATE CAPTURECORE_EXPORTS)

# windows.h's min/max macros collide with std::min/std::max (C2589 "illegal token on
# right side of '::'"), and enough files here transitively pull in windows.h (via
# Media Foundation headers, not just direct #include <windows.h>) that defining this
# once for the whole target is more robust than chasing include order per file.
target_compile_definitions(CaptureCore PRIVATE NOMINMAX)

# UVC capture (Media Foundation) and process/pipe control (encode/SRT pipeline) are
# always built — no proprietary SDK needed. mf.lib specifically is needed for
# MFEnumDeviceSources (device enumeration), separate from mfplat.lib.
target_link_libraries(CaptureCore PRIVATE mf mfplat mfreadwrite mfuuid ole32)

if(ENABLE_DECKLINK)
    message(STATUS "CaptureCore: DeckLink backend ENABLED (SDK found at ${DeckLinkSDK_INCLUDE_DIR})")
    target_include_directories(CaptureCore PRIVATE "${DeckLinkSDK_INCLUDE_DIR}")
    target_compile_definitions(CaptureCore PRIVATE CAPTURECORE_ENABLE_DECKLINK=1)
    # target_sources(CaptureCore PRIVATE src/capture/decklink/DeckLinkCaptureSource.cpp ...)  # added in Phase 2
else()
    message(STATUS "CaptureCore: DeckLink backend DISABLED (SDK not found; set DECKLINK_SDK_DIR or drop it in native/vendor/DeckLinkSDK)")
endif()

if(ENABLE_NDI)
    message(STATUS "CaptureCore: NDI backend ENABLED (SDK found at ${NdiSDK_INCLUDE_DIR})")
    target_include_directories(CaptureCore PRIVATE "${NdiSDK_INCLUDE_DIR}")
    target_link_libraries(CaptureCore PRIVATE "${NdiSDK_LIBRARY}")
    target_compile_definitions(CaptureCore PRIVATE CAPTURECORE_ENABLE_NDI=1)
    # target_sources(CaptureCore PRIVATE src/capture/ndi/NdiCaptureSource.cpp ...)  # added in Phase 3
else()
    message(STATUS "CaptureCore: NDI backend DISABLED (SDK not found; set NDI_SDK_DIR or drop it in native/vendor/NdiSDK)")
endif()

set_target_properties(CaptureCore PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
