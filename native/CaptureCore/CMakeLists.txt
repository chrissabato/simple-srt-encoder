set(CAPTURECORE_SOURCES
    src/CaptureCoreApi.cpp
    src/Diagnostics.cpp
    src/capture/CaptureManager.cpp
    src/capture/uvc/UvcDeviceEnumerator.cpp
    src/capture/uvc/UvcCaptureSource.cpp
    src/capture/directshow/DirectShowGuids.cpp
    src/capture/directshow/DirectShowDeviceEnumerator.cpp
    src/capture/directshow/DirectShowCaptureSource.cpp
    src/audio/WasapiAudioCapture.cpp
    src/audio/LoudnessMeter.cpp
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

# The DeckLink SDK ships raw .idl files on Windows (unlike Mac/Linux, which ship a
# pre-generated DeckLinkAPI.h) — midl.exe must compile DeckLinkAPI.idl into a header +
# the COM GUID definitions (DeckLinkAPI_i.c) at build time. This mirrors exactly what
# Blackmagic's own sample .vcxproj files do via MSBuild's built-in <Midl> item; CMake
# has no equivalent built-in, so it's invoked directly here.
if(ENABLE_DECKLINK)
    find_program(MIDL_COMPILER midl.exe)
    if(NOT MIDL_COMPILER)
        message(WARNING "DeckLink SDK found but midl.exe is not on PATH (need a Visual Studio developer environment) - disabling DeckLink backend.")
        set(ENABLE_DECKLINK FALSE)
    else()
        set(DECKLINK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/decklink")
        set(DECKLINK_GENERATED_HEADER "${DECKLINK_GENERATED_DIR}/DeckLinkAPI.h")
        set(DECKLINK_GENERATED_IID "${DECKLINK_GENERATED_DIR}/DeckLinkAPI_i.c")
        file(MAKE_DIRECTORY "${DECKLINK_GENERATED_DIR}")

        add_custom_command(
            OUTPUT "${DECKLINK_GENERATED_HEADER}" "${DECKLINK_GENERATED_IID}"
            COMMAND "${MIDL_COMPILER}" /nologo /char signed /env x64 /Oicf
                    /h DeckLinkAPI.h /iid DeckLinkAPI_i.c
                    /out "${DECKLINK_GENERATED_DIR}"
                    "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPI.idl"
            DEPENDS "${DeckLinkSDK_INCLUDE_DIR}/DeckLinkAPI.idl"
            COMMENT "Compiling DeckLinkAPI.idl with MIDL"
            VERBATIM
        )
        add_custom_target(DeckLinkApiGenerated DEPENDS "${DECKLINK_GENERATED_HEADER}" "${DECKLINK_GENERATED_IID}")
    endif()
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

# UVC capture (Media Foundation), DirectShow (virtual cameras MF doesn't enumerate —
# see directshow/DirectShowDeviceEnumerator.h), and process/pipe control (encode/SRT
# pipeline) are always built — no proprietary SDK needed. mf.lib specifically is
# needed for MFEnumDeviceSources (device enumeration), separate from mfplat.lib.
# oleaut32 is for VARIANT/BSTR handling when reading a DirectShow device moniker's
# FriendlyName property. Deliberately not linking strmiids.lib — see the comment atop
# DirectShowGuids.h for why.
target_link_libraries(CaptureCore PRIVATE mf mfplat mfreadwrite mfuuid ole32 oleaut32)

if(ENABLE_DECKLINK)
    message(STATUS "CaptureCore: DeckLink backend ENABLED (SDK found at ${DeckLinkSDK_INCLUDE_DIR})")
    target_sources(CaptureCore PRIVATE
        src/capture/decklink/DeckLinkDeviceEnumerator.cpp
        src/capture/decklink/DeckLinkCaptureSource.cpp
        "${DECKLINK_GENERATED_IID}"
    )
    target_include_directories(CaptureCore PRIVATE "${DeckLinkSDK_INCLUDE_DIR}" "${DECKLINK_GENERATED_DIR}")
    target_compile_definitions(CaptureCore PRIVATE CAPTURECORE_ENABLE_DECKLINK=1)
    # Ensures midl.exe has produced DeckLinkAPI.h/DeckLinkAPI_i.c before anything in
    # this target tries to compile against them (CMake's dependency tracking from a
    # custom command's OUTPUT only covers files that are themselves listed as sources
    # — DeckLinkAPI.h is only #included, not compiled, so it needs this explicit edge).
    add_dependencies(CaptureCore DeckLinkApiGenerated)
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
