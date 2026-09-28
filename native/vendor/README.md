# native/vendor

Drop locally-obtained proprietary SDKs here (this directory is gitignored except for this
file). Detected automatically by `native/cmake/FindDeckLinkSDK.cmake` and
`native/cmake/FindNdiSDK.cmake` on the next CMake configure — no code or path changes
needed.

- `native/vendor/DeckLinkSDK/` — Blackmagic DeckLink SDK, from
  https://www.blackmagicdesign.com/developer/ (needs a free Blackmagic account).
  Expects `Win/include/DeckLinkAPI.h` inside this folder (i.e. the SDK zip's contents
  extracted directly here).
- `native/vendor/NdiSDK/` — NDI SDK, from https://ndi.video/for-developers/.
  Expects `Include/Processing.NDI.Lib.h` and `Lib/x64/Processing.NDI.Lib.x64.lib` inside
  this folder (the NDI SDK installer's install directory, copied/symlinked here).

Alternatively, set the `DECKLINK_SDK_DIR` / `NDI_SDK_DIR` environment variables to point
at wherever the SDKs are installed instead of copying them into this repo.
