# Architecture

See the project plan for full context. Summary of the load-bearing decisions:

- **UI**: WinUI 3 (`ui/SrtEncoderApp`), chosen over WPF for `SwapChainPanel` +
  `ISwapChainPanelNative`, which gives a zero-copy GPU preview path (Phase 1.5).
- **Native core**: `native/CaptureCore` builds `CaptureCore.dll`, a flat C ABI
  (`capturecore_api.h`) consumed from C# via `[LibraryImport]`/P/Invoke
  (`ui/SrtEncoderApp/Interop/NativeMethods.cs`). Structs cross the boundary, not classes.
- **Capture backends**: `ICaptureSource` / `ICaptureDeviceEnumerator` (added Phase 1).
  UVC via Media Foundation always builds. DeckLink/NDI are optional — gated in
  `native/CaptureCore/CMakeLists.txt` by `ENABLE_DECKLINK`/`ENABLE_NDI` (AUTO-detected via
  `find_package(DeckLinkSDK)` / `find_package(NdiSDK)`), so the app builds and runs
  UVC-only until those proprietary SDKs are obtained and dropped into `native/vendor/`
  (or pointed to via `DECKLINK_SDK_DIR`/`NDI_SDK_DIR`).
- **Encode + SRT**: FFmpeg run as a subprocess (not linked libav*, to avoid GPL
  static-linking entanglement and a large vcpkg dependency surface), fed raw frames via a
  named pipe, targeting FFmpeg's built-in `srt://` muxer. Live setting changes = a
  controlled stop/restart of the ffmpeg process.
- **Presets**: JSON under `%LOCALAPPDATA%\SrtEncoder\Presets\<guid>.json`, all
  persistence/schema logic in C# (`ui/SrtEncoderApp/Services/PresetService.cs`, Phase 1);
  the native ABI only ever sees plain setting structs, so preset schema can evolve
  independently.

## Build

Two separate build systems, tied together by `build.ps1` at the repo root:

- `native/` — CMake (presets in `native/CMakePresets.json`; requires the VS "Desktop
  development with C++" workload).
- `ui/SrtEncoderApp/` — `dotnet build`. Its `CopyNativeCore` MSBuild target copies
  `CaptureCore.dll` from the native build output into the app's output directory after
  each build; run `native/`'s build first (or via `build.ps1`, which does both in order).

Run `./build.ps1` (or `./build.ps1 -SkipNative` if the C++ toolchain isn't installed yet —
the app still builds/runs, showing a fallback message in place of the native version
string).

## Phased delivery

0. Scaffolding (this state): CMake "hello DLL" + WinUI3 app that P/Invokes it. Proves the
   toolchain end-to-end.
1. UVC capture + preview + FFmpeg/SRT push + presets — first real usable app.
1.5. Zero-copy `SwapChainPanel` preview.
2. DeckLink backend (once the SDK is obtained).
3. NDI backend (once the SDK is obtained).
4. Hardware encoders, audio, reconnect/backoff, preset import/export.
