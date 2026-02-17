# LegoClicker — Project Guide & Rulebook

## Overview

LegoClicker is a **Minecraft utility client** consisting of two components:

- **C# WPF loader** (`LegoClickerCS`) — a minimal launcher window that injects the bridge DLL and hosts system-wide input hooks.
- **C++ bridge DLL** (`McInjector/bridge.dll`) — injected into the Minecraft / Lunar Client process. Attaches to the JVM via JNI, renders an in-game ClickGUI and overlays (nametags, Chest ESP, closest-player HUD), and communicates state to the loader over TCP on port **25590**.

> **Target client:** Lunar Client (JDK 17, x64). The bridge uses MinGW-compiled x64 DLL.

---

## Features

### Autoclicker (Combat)
- Left-click and right-click, independently configurable
- CPS range with Gaussian jitter for human-like variance
- **Block-Only** mode: right-click fires only when a block is held (requires injection)
- **Click In Chests** toggle
- Inventory safety: pauses when a GUI is open (cursor detection + bridge screen name)

### Visuals (Render)
- **Nametags** — renders player name, health bar, and armor points above players; color-coded (chroma or accent hue)
- **Closest Player HUD** — compact panel above the hotbar showing distance, direction (Front/Left/Right/Back), name, health, and held item
- **Chest ESP** — highlights chests through walls with distance labels

### Internal ClickGUI
- Opened with **Right Shift** in-game
- Draggable panel with Combat / Render categories
- Per-module settings in a right-hand panel (sliders, toggles)
- **Per-module keybind** row at the bottom of each settings panel: left-click to capture a key, right-click to unbind
- HUD style controls (accent hue slider, chroma toggle, chroma speed)

### Per-Module Keybinds
- Autoclicker: default `` ` `` (backtick, `0xC0`)
- Nametags / Closest Player / Chest ESP: unbound by default
- Keybinds saved in the loader profile (`config.json` in `%AppData%\LegoClicker\profiles\`)
- Binds are captured inside the in-game GUI; the C# hooks dispatch the toggles

### Profiles
- Saved as JSON to `%AppData%\LegoClicker\profiles\<name>.json`
- Auto-saved as `config` on loader close, auto-loaded on startup

---

## Project Structure

```
legoclickerC/
├── LegoClickerCS/               # C# WPF loader (.NET 8.0)
│   ├── Core/
│   │   ├── Clicker.cs           # Autoclicker engine (singleton, INotifyPropertyChanged)
│   │   ├── GameState.cs         # Data model for bridge state (EntityInfo, GameState, etc.)
│   │   ├── GameStateClient.cs   # TCP client, config push (~5Hz), bridge command handler
│   │   ├── NativeInjector.cs    # Win32 DLL injection (CreateRemoteThread + LoadLibraryA)
│   │   ├── InputHooks.cs        # WH_KEYBOARD_LL / WH_MOUSE_LL hooks; per-module keybinds
│   │   ├── WindowDetection.cs   # Minecraft window finder, cursor visibility check
│   │   ├── Profile.cs           # JSON profile save/load (ProfileManager)
│   │   └── ThemeManager.cs      # WPF resource dictionary theme switching
│   ├── MainWindow.xaml/.cs      # Loader window: inject button, connection status
│   ├── HudWindow.xaml/.cs       # Transparent WPF overlay (currently hidden; nametags rendered by bridge)
│   ├── App.xaml/.cs             # App entry point, theme resources
│   └── bridge.dll               # Copied from McInjector on build
│
└── McInjector/
    ├── build.bat                # MinGW g++ build script, copies DLL to LegoClickerCS/
    └── src/main/cpp/
        └── bridge.cpp           # Single-file bridge (~3500 lines)
```

---

## Injection & Communication Architecture

### Injection Flow
1. C# loader finds `javaw.exe` (Lunar Client)
2. `NativeInjector.cs` calls `CreateRemoteThread` + `LoadLibraryA` to load `bridge.dll` into the game process
3. `DllMain` (thread attach) spawns a background thread
4. Background thread: finds the JVM via `JNI_GetCreatedJavaVMs`, attaches, runs `DiscoverMappings`
5. **TCP server** starts on `0.0.0.0:25590`; the loader connects and exchange begins
6. **SwapBuffers hook** (IAT hook on `opengl32.dll`) fires every frame; renders overlays and the ClickGUI

### Dynamic Class Discovery
The bridge does **not** use hardcoded obfuscated class names. It discovers Minecraft classes at runtime via JVMTI `GetLoadedClasses` and JNI reflection patterns:

| Target | Discovery method |
|---|---|
| `Minecraft` class | Has a `static` field of its own type (singleton) |
| `thePlayer` field | Non-static field whose type hierarchy has `getHealth() → float` |
| `currentScreen` field | Non-static field whose type hierarchy has `drawScreen()` |
| Position (`posX/Y/Z`) | First three `double` fields in the entity superclass |
| `EntityList` | Field iterable to yield entities with `getName()` |
| `GuiChat` | Class constructable with a `String` arg, displayable via `displayGuiScreen` |
| Held item / inventory | `EntityPlayer.inventory.getCurrentItem()` → `ItemBlock` check |

This makes the bridge compatible across obfuscation mappings and Lunar Client versions.

### TCP Protocol
- **Loader → Bridge (config push, ~5Hz):**
  ```json
  {"type":"config","armed":true,"minCPS":10,"maxCPS":14,"left":true,"right":false,
   "nametags":true,"closestPlayerInfo":false,"chestEsp":false,
   "keybindAutoclicker":192,"keybindNametags":0, ...}
  ```
- **Bridge → Loader (game state, ~20Hz):**
  ```json
  {"type":"state","hp":20,"guiOpen":false,"screenName":"","posX":0,"posY":64,"posZ":0,
   "holdingBlock":false,"entities":[...],"chests":[...]}
  ```
- **Bridge → Loader (GUI commands):**
  ```json
  {"type":"cmd","action":"toggleArmed"}
  {"type":"cmd","action":"setKeybind","module":"nametags","key":72}
  {"type":"cmd","action":"setMinCPS","value":12}
  ```

---

## Building

### C++ Bridge
```bat
cd McInjector
build.bat
```
Requires MinGW-w64 at `C:\mingw64\mingw64\bin\g++.exe` and JDK 17 headers at `C:\Program Files\Java\jdk-17\`.
Outputs `bridge.dll` and copies it to `LegoClickerCS\bridge.dll` automatically.

### C# Loader
```bat
cd LegoClickerCS
dotnet build
dotnet run
```
Requires .NET 8.0 SDK. `bridge.dll` is copied to the output directory by the `.csproj`.

### Lunar Client JVM Flags (required for injection)
Add to Custom JVM Arguments in Lunar Client settings:
```
-XX:+EnableDynamicAgentLoading -XX:-DisableAttachMechanism
```

---

## Golden Rules

### 1. NEVER Trigger Detectable In-Game Actions

**FORBIDDEN:**
- ❌ Calling `attackEntity()`, `sendPacket()`, or any method that emits a network packet
- ❌ Modifying player position, velocity, rotation, or inventory via game code
- ❌ Calling `clickMouse()`, `rightClickMouse()`, `handleMouseClick()`, or `doAttack()`
- ❌ Hooking or replacing any game tick/update methods
- ❌ Using bytecode instrumentation to modify game logic

**ALLOWED:**
- ✅ Reading game state via JNI `GetField` / `CallMethod` (getters only, no side effects)
- ✅ Simulating input via Windows `SendInput` (external; indistinguishable from hardware)
- ✅ Rendering overlays via the hooked `SwapBuffers` (draw-only, no game state changes)
- ✅ Opening `GuiChat` via `displayGuiScreen` to unlock the cursor for the ClickGUI

### 2. Human-Like Behavior
- Randomized CPS via Gaussian distribution (not flat random)
- Timing jitter between clicks
- No clicks while a GUI is open; respect inventory and chest screens

### 3. JNI Safety
- Always call `PopLocalFrame` for every `PushLocalFrame` (all early-return paths included)
- Delete all local refs created in rendering loops (`DeleteLocalRef`)
- Never call methods on `nullptr` / unchecked jobjects; always check and `ExceptionClear`

