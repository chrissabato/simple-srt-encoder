#[[
FindDeckLinkSDK.cmake

Locates Blackmagic's proprietary DeckLink SDK, which is NOT redistributable and must be
downloaded separately from https://www.blackmagicdesign.com/developer/ by each developer.

Search order:
  1. $ENV{DECKLINK_SDK_DIR}
  2. ${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK  (gitignored local drop location)

On success, defines:
  DeckLinkSDK_FOUND
  DeckLinkSDK_INCLUDE_DIR  - directory containing DeckLinkAPI.idl (and the other .idl
                             files it #imports)

Windows ships raw .idl files, not a pre-generated header (unlike Mac/Linux) — the
consuming CMakeLists.txt compiles DeckLinkAPI.idl with midl.exe at build time to produce
DeckLinkAPI.h and DeckLinkAPI_i.c, so this module looks for the .idl itself as the
"is the SDK present" marker, not a header that doesn't exist yet.
]]

find_path(DeckLinkSDK_INCLUDE_DIR
    NAMES DeckLinkAPI.idl
    HINTS
        "$ENV{DECKLINK_SDK_DIR}"
        "${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK"
    PATH_SUFFIXES
        "Win/include"
        "include"
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeckLinkSDK
    REQUIRED_VARS DeckLinkSDK_INCLUDE_DIR
)

mark_as_advanced(DeckLinkSDK_INCLUDE_DIR)
