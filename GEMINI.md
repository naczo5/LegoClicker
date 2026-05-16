# LegoClicker Project Instructions

This `GEMINI.md` file serves as a comprehensive guide for AI agents and developers working on the LegoClicker project. It provides an overview of the architecture, build instructions, and core development conventions.

Primary reference: see `GUIDE.md` for the deeper architecture/mapping notes and implementation constraints used across this repository.

## Project Overview

**LegoClicker** is a Windows utility client for Lunar Client (Minecraft).
- **Supported versions:** **26.1**, **1.21.x**, and **1.8.9**.
- **Features:** Autoclicker, Aim Assist, Triggerbot, SpeedBridge, Reach and Velocity controls, GTB Helper, Nametags, Closest Player panel, Chest ESP, and customizable GUI with per-module keybinds.

### Architecture

The project consists of two main components communicating over TCP on port `25590`:
1. **Aoko (.NET 8 WPF):** The external GUI and loader. It manages settings, profiles (saved in `%AppData%\Aoko\profiles\`), and injects the native bridge DLL into the Lunar Client process.
2. **McInjector (Native C++):** The bridge DLL injected into Lunar Client. It renders overlays using ImGui/OpenGL, hooks functions using MinHook, and reads the game state via JNI. The Java agent sources under `McInjector/src/main/java` are obsolete.

## Building and Running

### Requirements
- Windows 10/11 x64
- Lunar Client
- .NET 8 SDK
- MinGW-w64 + JDK 17 headers

### Build Commands

Run these from the repository root unless noted otherwise:

**Full Release Pipeline:**
- Build full release: `build_release.bat`

**Native Bridge DLLs (C++):**
- Build both active bridges (1.8.9 & modern): `build_dll.bat`
- Build 26.1 only: `McInjector\build_261.bat`
- Build 1.8.9 only (legacy): `McInjector\build.bat`

**Loader / UI (C#):**
- Debug build: `dotnet build Aoko\Aoko.csproj`
- Release build: `dotnet build -c Release Aoko\Aoko.csproj`
- Run: `dotnet run --project Aoko\Aoko.csproj`
- Publish release executable: `build_exe.bat`

## Development Conventions & Constraints

- **Safety Constraint (CRITICAL):** The bridge-side (C++) logic should treat the game state as **read-first**. **Do not** add direct packet sending or invoke in-game combat methods via JNI.
- **Input Simulation:** All input actions (e.g., clicking, aiming) must be performed using Win32 `SendInput` from the external C# or appropriate safe native channels.
- **Limited State Writes:** Thoughtful, minimal JNI state modifications are permitted when ghost-safe and undetectable (e.g., reach via entity attributes, velocity scaling, nametag visibility). Keep overlay work draw-only; mutate state only when justified and stealthy.
- **Version Scope:** Keep all supported versions (`bridge_261.dll` for 26.1/1.21, `bridge.dll` for 1.8.9) working through the same external GUI flow.
- **UI Modifications:** The C# external GUI (`Aoko/MainWindow.xaml`) is the primary user interface.
- **Capability Sync:** Bridge capability packets drive version-aware UI gating. Keep `BridgeCapabilities.cs`, `bridge_capabilities.h`, profiles, keybinds, TCP config, and tests in sync when adding modules.
