# LegoClicker

A Minecraft utility client for Lunar Client. Consists of a lightweight C# WPF loader and a C++ DLL injected into the game process.

## Screenshots

### Gameplay HUD
![Gameplay HUD](screenshots/gameplay.png)

### Internal ClickGUI
![Internal ClickGUI](screenshots/clickgui.png)

## Features

- **Autoclicker** — left and right click, configurable CPS range with Gaussian jitter, block-only mode, inventory/chest safety
- **Nametags** — in-game player name tags with health bars and armor points, color-coded with chroma/accent support
- **Closest Player HUD** — compact panel showing the nearest player's name, distance, direction, health, and held item
- **Chest ESP** — highlights chests through walls with distance labels
- **Internal ClickGUI** — in-game draggable panel (Right Shift), per-module settings and keybinds
- **Per-module keybinds** — set a key for any module directly from the in-game GUI
- **Profiles** — saved to `%AppData%\LegoClicker\profiles\` as JSON

## Requirements

- Windows 10/11 x64
- [.NET 8.0 Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)
- [Lunar Client](https://www.lunarclient.com/) (Java Minecraft 1.8.9+)
- MinGW-w64 (for building the DLL — not needed to run)

### Lunar Client JVM flags

Add these to **Custom JVM Arguments** in Lunar Client settings:

```
-XX:+EnableDynamicAgentLoading -XX:-DisableAttachMechanism
```

## Building

### 1. C++ Bridge DLL

```bat
cd McInjector
build.bat
```

Requires MinGW-w64 at `C:\mingw64\mingw64\bin\g++.exe` and JDK 17 headers at `C:\Program Files\Java\jdk-17\`.  
The script compiles `bridge.dll` and copies it to `LegoClickerCS\bridge.dll` automatically.

### 2. C# Loader

```bat
cd LegoClickerCS
dotnet build
dotnet run
```

## Usage

1. Launch Lunar Client with the JVM flags above
2. Run `LegoClicker.exe` (or `dotnet run` from `LegoClickerCS/`)
3. Click **Inject / Connect** in the loader window
4. Press **Right Shift** in-game to open the ClickGUI
5. Press **Backtick** (`` ` ``) to toggle the autoclicker (default bind)

## Architecture

```
LegoClickerCS (C# WPF)          McInjector (C++ DLL)
─────────────────────           ──────────────────────────────────
MainWindow        ──inject──▶  DllMain → background thread
GameStateClient   ◀──TCP 25590─  TCP server (game state ~20Hz)
InputHooks        ──TCP 25590──▶  config push + commands
Clicker (engine)                SwapBuffers hook → overlays + ClickGUI
Profile (JSON)                  JNI → reads entity/world state
```

The bridge uses **dynamic class discovery** via JVMTI and JNI reflection — no hardcoded obfuscated names — making it compatible across Lunar Client versions.

## Project layout

```
legoclickerC/
├── LegoClickerCS/          # C# WPF loader (.NET 8.0)
│   ├── Core/               # Clicker, InputHooks, Profile, GameStateClient, ...
│   ├── MainWindow.xaml     # Loader window
│   ├── HudWindow.xaml      # Transparent WPF overlay (reserved)
│   └── bridge.dll          # Copied from McInjector on build
├── McInjector/
│   ├── build.bat           # MinGW build script
│   └── src/main/cpp/
│       └── bridge.cpp      # Full bridge: JNI, TCP, GL rendering, ClickGUI
├── PROJECT_GUIDE.md        # Architecture, rules, and developer notes
└── README.md               # This file
```

## Important rules

- The bridge is **strictly read-only** with respect to game state. It never sends packets, modifies player data, or calls any gameplay methods.
- All actions (clicks) are performed via Windows `SendInput` — external and indistinguishable from real hardware input.
- See [PROJECT_GUIDE.md](PROJECT_GUIDE.md) for the full rulebook and JNI safety requirements.
#
