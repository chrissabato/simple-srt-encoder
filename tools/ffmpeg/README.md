# tools/ffmpeg

Phase 1 adds `fetch-ffmpeg.ps1` here, which downloads a pinned, static Windows FFmpeg
build (libx264 + SRT support) into `ui/SimpleSrtEncoder/ffmpeg/ffmpeg.exe` at provision/build
time. Not present yet — Phase 0 only scaffolds the toolchain and native/C# interop path.

Not committed to git (binary, license-bearing) — see `.gitignore`.
