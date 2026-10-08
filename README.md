# SimpleSRT Encoder

A Windows desktop app for live SRT streaming from UVC, Blackmagic DeckLink, and NDI video
sources, with a live preview and saved presets. See `docs/architecture.md` for the design.

## Status

**Phase 0 — scaffolding.** The toolchain and native-core/UI interop path are wired up
end-to-end (a "hello DLL" native core, P/Invoked from a WinUI 3 app). Real capture,
encode/SRT, and preset functionality land in Phase 1.

## Prerequisites

- **.NET SDK 10+** — already installed if `dotnet --version` works.
- **Visual Studio 2022/2026 with the "Desktop development with C++" workload** — needed to
  build `native/` (brings CMake + Ninja). Check via the Visual Studio Installer; without
  it, `cmake`/`ninja` won't be on PATH.
  - Without this installed, you can still build/run the UI-only app with
    `./build.ps1 -SkipNative` — it'll show a fallback message instead of talking to the
    native core.

## Build

```powershell
./build.ps1                  # builds native/ then ui/SimpleSrtEncoder (Debug)
./build.ps1 -Configuration Release
./build.ps1 -SkipNative      # UI only, if the C++ toolchain isn't installed
```

## Run

```powershell
dotnet run --project ui/SimpleSrtEncoder/SimpleSrtEncoder.csproj
```

## Optional capture backends

DeckLink and NDI support require their (proprietary, separately licensed) SDKs. The app
builds and runs with UVC support only until you add them — see `native/vendor/README.md`
for where to drop them (or set `DECKLINK_SDK_DIR` / `NDI_SDK_DIR`). No code changes are
needed; the next CMake configure picks them up automatically.
