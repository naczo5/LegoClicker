# LegoClicker

LegoClicker is a Windows utility client for Lunar Client.

## Current status

- Supported versions: **26.1**, **1.21.x**, and **1.8.9**.
- All supported versions are used through the external GUI in `LegoClickerCS`.

## Features (current)

- Autoclicker (left/right, CPS range, jitter, block-only options)
- Aim Assist
- Triggerbot
- Reach and Velocity controls
- GTB Helper
- Nametags, Closest Player panel, Chest ESP
- Per-module keybinds
- Profiles saved in `%AppData%\LegoClicker\profiles\`
- GUI customization (palette, module list style, show logo)

## Screenshots

![GUI Showcase GIF](screenshots/gui.gif)

## Requirements

- Windows 10/11 x64
- Lunar Client
- .NET 8 SDK (build only)
- MinGW-w64 + JDK 17 headers (native build only)

### Required Lunar JVM flags

Add this to Lunar Client custom JVM arguments:

```text
-XX:+EnableDynamicAgentLoading -XX:-DisableAttachMechanism
```

## Quick start

1. Start Lunar Client.
2. Run `LegoClicker.exe`.
3. Click **Inject**.
4. Use the external GUI.

## Build

Run from repository root unless noted.

### Native bridge DLLs

- Build both: `build_dll.bat`
- Build 26.1 only: `McInjector\build_261.bat`
- Build 1.8.9 only: `McInjector\build.bat`

### Loader (C#)

- Debug build: `dotnet build LegoClickerCS\LegoClickerCS.csproj`
- Release build: `dotnet build -c Release LegoClickerCS\LegoClickerCS.csproj`
- Run: `dotnet run --project LegoClickerCS\LegoClickerCS.csproj`
- Publish release exe: `build_exe.bat`

### Full release pipeline

- `build_release.bat`

## Notes on versions

- `bridge_261.dll` is the modern bridge used for both 26.1 and 1.21 injection.
- `bridge.dll` (1.8.9) is supported.
- Supported runtime bridges are configured through the external GUI.

## Project structure

```text
legoclickerC/
|- LegoClickerCS/              # WPF loader + external GUI (.NET 8)
|  |- Core/                    # Clicker, hooks, profile, TCP client
|  |- MainWindow.xaml(.cs)     # Main UI
|  |- bridge.dll               # 1.8.9 bridge (legacy)
|  `- bridge_261.dll           # 26.1 bridge
|- McInjector/
|  |- build.bat                # 1.8.9 bridge build (legacy)
|  |- build_261.bat            # 26.1 bridge build
|  `- src/main/cpp/            # Native bridge sources
|- docs/                       # Website
`- README.md
```

## Architecture (short)

- `LegoClickerCS` injects bridge DLL into Lunar and manages settings/UI.
- Bridge and loader communicate over TCP (`25590`).
- Bridge renders overlays and reads game state via JNI.
- Input actions are sent through Win32 `SendInput`.

## Safety constraint used by this project

- Bridge-side logic is intended to be read-only for game state.
- Do not add direct packet sending or in-game combat method calls.

## 1.8.9

![Gameplay HUD](screenshots/gameplay.png)
![Internal ClickGUI (1.8.9 path)](screenshots/clickgui.png)
