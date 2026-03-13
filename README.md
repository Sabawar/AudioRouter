<div align="center">

<img src="resources/icon_preview.png" width="96" alt="AudioRouter icon"/>

# AudioRouter

**Professional audio routing engine for Windows**  
Route any audio source to any output — WASAPI, ASIO, virtual sinks — with a visual node editor, noise suppression, and zero-latency mixing.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078d4?logo=windows)](https://github.com/Sabawar/AudioRouter/releases)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/17)
[![Release](https://img.shields.io/github/v/release/Sabawar/AudioRouter?color=green)](https://github.com/Sabawar/AudioRouter/releases/latest)

[**⬇ Download**](https://github.com/Sabawar/AudioRouter/releases/latest) · [Report Bug](https://github.com/Sabawar/AudioRouter/issues) · [Request Feature](https://github.com/Sabawar/AudioRouter/issues)

</div>

---

## What is AudioRouter?

AudioRouter is a lightweight, low-latency audio routing application for Windows. It lets you create flexible signal chains between microphones, speakers, virtual devices, and professional ASIO hardware — all through a visual drag-and-drop node editor, without needing a full DAW.

Think of it as a software patch bay: connect anything to anything, apply noise suppression along the way, and monitor levels in real time.

---

## ✨ Features

### 🎛 Visual Node Editor
- Drag-and-drop signal graph — connect sources to sinks with a single click
- Color-coded nodes: 🟢 WASAPI · 🔴 ASIO · 🟣 Virtual / Mixer
- Per-node gain control (dB) and real-time peak level meters
- Live format info on every node: `44100 Hz | 32bit | 2ch | 2.9ms`
- Save and load routing presets as `.patch` files

### 🔊 Audio Engine
- **10 ms mix cycle** with MMCSS thread priority and `timeBeginPeriod(1)` for minimal jitter
- Lock-free ring buffers for all signal paths
- Hard clip protection on all outputs
- Per-node gain staging with dB scaling
- Mono → stereo automatic upmix for mono capture devices

### 🖥 WASAPI Support
- Capture (microphone), Loopback (desktop audio), and Render (speakers/headphones)
- Automatic device enumeration and hot-plug detection
- Shared-mode operation — compatible with all Windows audio devices
- `IAudioEndpointVolume` integration for per-device gain

### ⚡ ASIO Support
- Direct DLL loading (`LoadLibrary` + `DllGetClassObject`) — bypasses COM registration issues that plague many ASIO setups
- Full driver enumeration from Windows Registry (including 32-bit legacy drivers)
- Live driver monitoring: detects sample rate and buffer size changes made in the ASIO control panel
- **"Reload Driver"** button appears automatically when hardware settings change
- Color-coded driver list: 🟢 available · 🟡 32-bit · 🔴 missing DLL
- Input / output channel count display with DLL path tooltip
- Resampling between ASIO sample rate and internal mix rate (linear interpolation)

### 🔇 Noise Suppression
Five modes per input node, applied before routing:

| Mode | Description |
|------|-------------|
| **Off** | Bypass — raw signal |
| **Light** | Gentle high-pass + gate |
| **Medium** | Spectral subtraction |
| **Aggressive** | Deep spectral subtraction + hard gate |
| **RNNoise** | Neural network (Mozilla RNNoise v0.1) — best quality |

- Per-node noise gate threshold (dB)
- RNNoise runs in 480-sample frames with VAD (Voice Activity Detection) gate
- Runtime AVX2 + FMA dispatch — falls back to SSE2 on older CPUs

### 🎚 Volume Mixer
- Vertical faders for all active audio sessions (system-wide app mixer)
- Real-time peak meters per application
- Mute toggle per session

### 🖼 System Tray
- Minimize to tray — no taskbar clutter
- Right-click menu: Show / Hide · Exit
- Single-instance guard: activates existing window if launched twice

### 🔄 Auto-Updater
- Checks GitHub Releases on startup (background thread, non-blocking)
- Silent banner notification when a new version is found
- One-click **Download & Install**: downloads the release `.zip`, extracts it over the existing installation, and restarts — no manual file copying needed
- Full redirect support for GitHub CDN asset delivery

### 🌐 Localization
- Russian 🇷🇺 and English 🇬🇧 — switch at runtime
- Cyrillic font loaded from system fonts (`segoeui.ttf`)

### 💾 Persistent Settings
- Window position, scale, language, and autostart saved automatically
- Routing graph auto-saved to `.patch` on exit

---

## 📋 Requirements

| | Minimum |
|-|---------|
| **OS** | Windows 10 (build 1903+) or Windows 11 |
| **CPU** | SSE2 (any x64 CPU) · AVX2+FMA recommended |
| **RAM** | 60 MB |
| **GPU** | Direct3D 11 feature level 11.0 |
| **ASIO** | Optional — any ASIO driver (32-bit or 64-bit) |

---

## 🚀 Quick Start

1. Download the latest `AudioRouter.zip` from [Releases](https://github.com/Sabawar/AudioRouter/releases/latest)
2. Extract anywhere — no installer needed
3. Run `AudioRouter.exe`
4. Click **Add Device** → choose your microphone (WASAPI Capture or ASIO Input)
5. Click **Add Device** → choose your output (WASAPI Render or ASIO Output)
6. Draw a connection from the microphone node to the output node
7. Sound flows 🎵

---

## 🏗 Building from Source

### Prerequisites

- Visual Studio 2022 (with C++ Desktop workload)
- CMake 3.20+
- Git

### Steps

```bat
git clone https://github.com/Sabawar/AudioRouter.git
cd AudioRouter

REM Delete old build folder if present
rmdir /s /q build

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output binary: `build\bin\Release\AudioRouter.exe`

### Secrets (Client ID)

ASIO and WASAPI features require a Twitch / API client ID stored in `secrets.cpp`.  
Copy the template and fill in your credentials:

```bat
copy secrets.cpp.template secrets.cpp
```

> `secrets.cpp` is in `.gitignore` and must never be committed.

---

## 🏛 Architecture

```
AudioRouter
├── AudioEngine          — 10ms mix thread, MMCSS, timeBeginPeriod(1)
│   ├── WasapiManager    — Capture / Loopback / Render streams + ring buffers
│   └── AsioManager      — Direct DLL ASIO driver loading + callback
├── RoutingGraph         — Node/edge graph, mutex-protected, .patch serialization
├── NoiseFilter          — DSP chain: gate → spectral subtract → RNNoise
├── AudioMath            — AVX2+FMA / SSE2 runtime dispatch (mix, gain, clip)
└── UI (ImGui + imnodes)
    ├── NodeEditorUI     — Visual graph editor, ASIO panel, update checker
    └── VolumeMixer      — System-wide app volume mixer
```

### Signal flow

```
[Mic / ASIO In / Loopback]
        │  (ring buffer, lock-free)
        ▼
  [NoiseFilter]  ← per-node, optional
        │
        ▼
  [Mix Engine]  ← 10ms cycle, MMCSS, gain staging
        │
        ▼
[Speaker / ASIO Out / Virtual Sink]
```

---

## ⚙ Configuration

All settings are saved automatically to `AudioRouter.ini` next to the executable.

| Setting | Description |
|---------|-------------|
| `scale` | UI DPI scale (0.75 – 2.0) |
| `lang` | 0 = Russian, 1 = English |
| `autostart` | Launch with Windows |
| `window.*` | Position and size |

Routing graphs are saved as `*.patch` files (INI-like text format, human-readable).

---

## 🔌 Supported Audio APIs

| API | Direction | Notes |
|-----|-----------|-------|
| WASAPI Capture | Input | Microphones, line-in |
| WASAPI Loopback | Input | Desktop audio (what you hear) |
| WASAPI Render | Output | Speakers, headphones |
| ASIO | Input + Output | Ultra-low latency; requires ASIO driver |
| Virtual Sink | Internal | Software mix bus between nodes |

---

## 🧠 Noise Suppression — Technical Details

AudioRouter includes a full DSP noise suppression chain:

```
Input signal
    │
    ├─[1] High-pass filter (remove rumble)
    ├─[2] Noise gate (threshold in dB, configurable per node)
    ├─[3] Spectral subtraction (Light / Medium / Aggressive modes)
    └─[4] RNNoise neural net (480-sample frames, VAD gate)
              │
              └── Mozilla RNNoise v0.1 (bundled, no external DLL)
```

RNNoise is compiled as a static library (`rnnoise_bundled`) from source — no separate installation required.

---

## 🆕 Auto-Update Flow

```
Startup
  └─ background thread → GET github.com/Sabawar/AudioRouter/releases/latest
        │
        ├─ UpToDate → silent
        │
        └─ NewVersion found
              │
              └─ Green banner + Help menu indicator
                    │
                    └─ User clicks "Download & Install"
                          │
                          └─ WinHTTP download with progress bar
                                │
                                └─ User clicks "Install & Restart"
                                      │
                                      ├─ ar_update.bat written to %TEMP%
                                      ├─ AudioRouter.exe exits
                                      ├─ bat: waits 3s
                                      ├─ bat: PowerShell Expand-Archive
                                      └─ bat: starts new AudioRouter.exe
```

---

## 🤝 Contributing

Pull requests are welcome. For major changes please open an issue first.

1. Fork the repo
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit your changes: `git commit -m 'Add my feature'`
4. Push and open a Pull Request

---

## 📄 License

[MIT](LICENSE) © 2024 Sabawar

---

<div align="center">

Made with ❤️ for audio nerds who hate latency

</div>
