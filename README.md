# Exo Capture

Instant replay, recording and snips for [Exo Launcher](https://github.com/UhhErix/ExoLauncher). Exo starts it as an add-on (`addons\capture`) and talks to it over NDJSON on stdin/stdout.

- Windows.Graphics.Capture for the game window or screen, converted on the GPU (D3D11 video processor) and encoded with the GPU's Media Foundation encoder (NVENC, AMF or Quick Sync).
- Instant replay keeps encoded video in memory and writes MP4 only when you save.
- When Windows capture gets no picture from a game (exclusive full screen), it can use OBS Studio's own signed game capture hook. See `third_party/obs/README.md`.

Build: `pwsh -File build.ps1` (Visual Studio C++ tools, CMake and Ninja). Output: `out\ExoCapture.exe` plus `out\obs`.

License: GPL-3.0-or-later (`LICENSE`). OBS Studio's files are GPL-2.0-or-later, from OBS Studio 32.2.2.
