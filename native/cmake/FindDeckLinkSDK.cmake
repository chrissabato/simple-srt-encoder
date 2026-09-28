#[[
FindDeckLinkSDK.cmake

Locates Blackmagic's proprietary DeckLink SDK, which is NOT redistributable and must be
downloaded separately from https://www.blackmagicdesign.com/developer/ by each developer.

Search order:
  1. $ENV{DECKLINK_SDK_DIR}
  2. ${CMAKE_SOURCE_DIR}/vendor/DeckLinkSDK  (gitignored local drop location)

On success, defines:
  DeckLinkSDK_FOUND
  DeckLinkSDK_INCLUDE_DIR  - directory containing DeckLinkAPI.h / DeckLinkAPI_i.c

Does not require any library to link against — the DeckLink SDK ships its COM interface
headers plus a generated IDL-derived .c file that the consuming target compiles directly.
]]

find_path(DeckLinkSDK_INCLUDE_DIR
    NAMES DeckLinkAPI.h
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
