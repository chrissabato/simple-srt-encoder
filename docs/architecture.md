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
- **Distribution/updates**: Velopack, not MSIX — this app may ship externally (not just
  internally), and needs full-trust native device/process access (UVC, DeckLink, spawning
  ffmpeg.exe) without MSIX's cert-trust-before-install friction when not signed by a CA.
  Releases are hosted on GitHub Releases (repo is OK to be public, so the client needs no
  access token — see `GithubSource` in `Services/UpdateService.cs`). Signing reuses the
  same Azure Trusted Signing account + certificate profile as the Stadium Sound Electron
  app (one certificate profile signs for a publisher identity, not a single product) —
  `azure-trusted-signing-metadata.json` at the repo root (not secret: account/profile
  names, no credentials) is `vpk pack --azureTrustedSignFile`'s default input via
  `release.ps1`; actual auth still needs AZURE_TENANT_ID/AZURE_CLIENT_ID/
  AZURE_CLIENT_SECRET in the environment, same service-principal secrets Stadium Sound's
  `release.yml` already uses, picked up automatically by vpk's bundled Azure.Identity.
  `Program.cs` (custom `Main`, since `VelopackApp.Build().Run()` must run before any
  WinAppSDK/XAML init) + `Services/UpdateService.cs` (update check/apply, no-op when not
  running from a Velopack-installed copy) + `MainViewModel.CheckForUpdatesAsync`/
  `InstallUpdateAndRestartAsync` (checked once at startup, surfaced via an InfoBar in
  `MainPage.xaml`). `release.ps1` publishes, `vpk pack`s (signed by default), and (given
  `-RepoUrl`) `vpk upload github`s a release to
  https://github.com/chrissabato/srt-encoder.
  **Resolved (2026-10-02)**: the `REGDB_E_CLASSNOTREG`/`0xc000027b` unpackaged-launch
  crash noted above as an unverified risk turned out to be real — reproduced via an actual
  Velopack install reporting "partially succeeded", root-caused via Windows Event Viewer.
  `WindowsAppSDKSelfContained=true`'s `Condition="'$(SelfContained)'=='true'"` never
  actually matched (that property only arrives via the `.pubxml`, invisible to MSBuild
  when it evaluates the main project file's own conditions), so every publish was
  silently framework-dependent, needing a system-installed Windows App Runtime this
  machine never had. Fixed by setting it unconditionally and also passing
  `-p:SelfContained=true -p:WindowsAppSDKSelfContained=true` explicitly in `release.ps1`'s
  `dotnet publish` call. Verified for real: a from-scratch install launched clean, with
  `Microsoft.WindowsAppRuntime.dll`/`DWriteCore.dll` now actually present in the publish
  output, and zero Event Viewer errors on install or launch.

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
   - Audio: WASAPI mic/line-in capture (device picker, independent of the video
     backend/device) is implemented — see `native/CaptureCore/src/audio/WasapiAudioCapture.*`
     and `FfmpegProcessController`'s second rawaudio input. DeckLink's embedded SDI/HDMI
     audio is also wired up: pick "Embedded audio (capture source)" in the audio device
     list (sentinel ID `embedded`, `CC_EMBEDDED_AUDIO_DEVICE_ID`); `DeckLinkCaptureSource`
     buffers 48kHz/2ch/s16 PCM and `CaptureManager::EmbeddedAudioThreadMain` pumps it to
     ffmpeg. NDI embedded audio is still not wired up.
