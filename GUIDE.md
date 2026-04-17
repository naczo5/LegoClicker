# LegoClicker Developer Guide

Welcome to the LegoClicker project! This guide provides a comprehensive overview of the architecture, workflow, implementation details, and version differences for anyone working on this repository, including AI agents.

## 1. Project Architecture

LegoClicker is a split-architecture utility client designed for Lunar Client (Minecraft).

*   **LegoClickerCS (C# / .NET 8 WPF):** 
    *   Acts as the external GUI, profile manager, and loader.
    *   Hosts the cheat logic (`Clicker.cs`), simulating inputs using Win32 `SendInput`.
    *   Connects to the native bridge over a TCP socket (`127.0.0.1:25590`) via `GameStateClient.cs`.
*   **McInjector (C++ Native Bridge DLLs):** 
    *   Injected into the Lunar Client process.
    *   `bridge.dll`: Legacy bridge for Minecraft 1.8.9.
    *   `bridge_261.dll`: Modern bridge for Minecraft 1.21.x and Lunar 26.1.
    *   Uses JNI to read game state data directly from the JVM.
    *   Hosts the TCP server on port `25590` to send JSON data to the C# client and receive configuration updates.
    *   Hooks OpenGL (`wglSwapBuffers`) for rendering in-game overlays (ImGui for modern, raw GL for legacy).
    *   Hooks `WndProc` to manage cursor state and block game input when the internal ClickGUI is open.

**Note on Unused Code:**
The repository contains a Java Agent (`McInjector/src/main/java/me/legoclicker/agent/LegoAgent.java`, `GameStateServer.java`, etc.). Analysis shows this is likely **obsolete/unused legacy code**. The native C++ bridges (`bridge.cpp`, `bridge_261.cpp`) now perform all JNI reflections and host the TCP server on port `25590` themselves.

---

## 2. Working with the Project

### Prerequisites
*   Windows 10/11 x64
*   Lunar Client
*   .NET 8 SDK
*   MinGW-w64 + JDK 17 headers

### Building
Use the provided batch scripts in the root directory:
*   `build_dll.bat`: Builds both C++ bridge DLLs.
*   `build_exe.bat`: Publishes the C# loader.
*   `build_release.bat`: Full pipeline build.

### Running
Run `LegoClicker.exe` (or `dotnet run` in `LegoClickerCS`), select the version, and click "Inject".

For legacy 1.8.9, menu injection is supported: you can inject while in menus/lobby and mappings will bootstrap when entering a world.

---

## 3. How to Implement New Features

### Adding Cheat Logic (e.g., a new Aim or Click feature)
**Where:** `LegoClickerCS/Core/Clicker.cs`
1.  Add configuration properties (e.g., `MyNewCheatEnabled`).
2.  Add a new Loop method (e.g., `MyNewCheatLoop`) and manage its lifecycle via `CancellationTokenSource`, similar to `AimAssistLoop` or `TriggerbotLoop`.
3.  Simulate input using Win32 `SendInput` (`_leftClickInputs`, `_aimAssistMoveInput`, etc.). Do not send packets directly.
4.  Update the TCP config payload in `GameStateClient.cs` (`ConfigSenderLoop`) to send the toggle state to the bridge (so the in-game GUI knows about it).

### Adding Visual Overlays
**Where:** `McInjector/src/main/cpp/bridge_261.cpp` (and `bridge.cpp` for legacy)
1.  Read the incoming config state in `ParseConfig` in the bridge.
2.  Implement rendering logic inside the `wglSwapBuffers` hook using ImGui (for modern) or raw OpenGL (for legacy).
3.  Use the `WorldToScreen` functions to project 3D game coordinates to 2D screen coordinates.

### Extracting New Game State
**Where:** C++ Bridges and `GameStateClient.cs`
1.  **C++ Bridge:** Use JNI to discover the target fields/methods in the `DiscoverMappings` or `TryResolveRenderMappings` routines. 
2.  **C++ Bridge:** Read the data during the render loop/background threads and append it to the JSON payload sent to the C# client.
3.  **C# Loader:** Update `GameState.cs` to include the new fields. Ensure it parses the incoming JSON correctly.

---

## 4. The Golden Rules: What to Do and Not to Do

*   **DO NOT** modify the game state directly via JNI (e.g., do not set health, do not force rotations via Java fields). 
*   **DO NOT** call in-game combat methods (like `clickMouse()` or `attackEntity()`) or send network packets from the C++ bridge. 
*   **DO** treat the bridge-side logic as strictly **read-only**.
*   **DO** use `SendInput` from the external C# loader to simulate human input.
*   **DO** respect the cross-thread limitations of JNI. Only use JNI calls from threads properly attached to the JVM. Avoid heavy JNI reflection inside the high-frequency `wglSwapBuffers` render thread; cache method and field IDs beforehand!
*   **DO** preserve **menu-injection compatibility** in all code changes (especially 1.8.9): mappings and feature behavior must recover correctly when injected in menus/lobby, not only when injected in-world.
*   **DO** prefer a single deterministic path when runtime evidence shows fallback branches are unnecessary. Keep fallback/recovery logic only where logs prove it is needed.

---

## 5. Version Differences & Mappings

The codebase is split strictly into two runtime paradigms due to massive differences in the Minecraft codebase between 1.8.9 and modern versions (1.21.x).

### Legacy Bridge (`bridge.cpp` - Minecraft 1.8.9)
*   **Mappings:** Minecraft 1.8.9 is heavily obfuscated without official deobfuscation maps. 
*   **Discovery:** The bridge uses deep reflection and heuristic scanning. For example, it finds the `thePlayer` field by scanning for a singleton object, checking its methods for one returning a `float` named `getHealth`, and verifying field types.
*   **World Structure:** Entities and TileEntities (chests) are stored in flat lists (`playerEntities`, `loadedTileEntityList`) on the `WorldClient` object.
*   **Camera:** Matrix data is retrieved from `ActiveRenderInfo` (`MODELVIEW` and `PROJECTION` FloatBuffers). Viewer position is taken from `RenderManager`.

### Modern Bridge (`bridge_261.cpp` - Minecraft 1.21 / Lunar 26.1)
*   **The 1.21 vs 26.1 Mapping Shift:** There is a profound difference in how Minecraft 1.21 and Minecraft 26.1 are structured, which is why `bridge_261.cpp` relies on a fallback array mechanism:
    *   **Minecraft 1.21 (Obfuscated Era):** The game is obfuscated. Lunar 1.21 runs on Fabric, meaning the bridge must target **Yarn Intermediary mappings** (e.g., `net.minecraft.class_1657`, `method_18798`).
    *   **Minecraft 26.1 (Unobfuscated Era):** Starting with 26.1, Mojang officially stopped obfuscating Minecraft. Mappings like Yarn are deprecated. The game natively uses **Official Mojang Mappings** (e.g., `net.minecraft.world.entity.player.Player`, `getDeltaMovement`). [Read more on the transition to unobfuscated Minecraft](https://fabricmc.net/).
    
    To support *both* seamlessly, the bridge checks the Yarn (`class_`) name first for 1.21 compatibility, and falls back to the Official/Mojmap name for 26.1.
    
    *Code Example of Class Lookup in `bridge_261.cpp`:*
    ```cpp
    // The bridge tries Yarn (1.21) first, then Official/Mojmap (26.1)
    const char* names[] = {
        "net.minecraft.class_239",                     // 1.21 Yarn
        "net.minecraft.world.phys.HitResult",          // 26.1 Official / 1.21 Mojmap
        nullptr
    };
    ```
    
    *Exact Mapping Differences (1.21 vs 26.1):*
    *   **PlayerEntity:** 1.21 uses `net.minecraft.class_1657` | 26.1 uses `net.minecraft.world.entity.player.Player`
    *   **HitResult:** 1.21 uses `net.minecraft.class_239` | 26.1 uses `net.minecraft.world.phys.HitResult`
    *   **BlockState:** 1.21 uses `net.minecraft.class_2680` | 26.1 uses `net.minecraft.world.level.block.state.BlockState`
    *   **Identifier / ResourceLocation:** 1.21 uses `net.minecraft.class_2960` | 26.1 uses `net.minecraft.resources.ResourceLocation`
    *   **getVelocity Method:** 1.21 uses `method_18798` | 26.1 uses `getDeltaMovement` (or `getVelocity`)

*   **World Structure:** Block Entities (like Chests) are no longer in a flat list. They are chunk-based. The bridge must query `WorldChunk.blockEntities` (1.21 Yarn `field_12833`) Map to find them.
*   **Camera:** `ActiveRenderInfo` is gone. Matrices are retrieved via `GameRenderer` (1.21 `class_757`) and `Camera` (1.21 `class_4184`) classes, grabbing JOML `Matrix4f` objects.
*   **HitResults:** The `HitResult` system is different. Looking at air still returns a `BlockHitResult`, but with a `Type` of `MISS`. Reach and Triggerbot logic in C# accounts for this.

---

## 6. Module Implementations & Design Differences

Because the rendering and state extraction differ heavily, the design of visual modules and state modifications is distinct between versions.

### External Modules (Shared Logic)
*   **Aim Assist & Triggerbot:** These are handled almost entirely in the external C# application (`LegoClickerCS/Core/Clicker.cs`). They rely on the game state (player position, rotations, entity lists) streamed over TCP. 
    *   **Aim Assist** calculates pitch/yaw differences and simulates mouse movement using Win32 `SendInput`.
    *   **Triggerbot** tracks server attack cooldowns and crosshair state, simulating clicks via `SendInput`.

### Visual Modules (Nametags, Chest ESP, ClickGUI)
*   **Modern Design (1.21.x / 26.1):** Heavily utilizes **ImGui**. The ClickGUI uses standard ImGui windows. Overlays use `ImGui::GetBackgroundDrawList()` to draw text/rectangles. Entity and chunk iterations are used for Nametags and Chest ESP with JOML matrices for projection.
*   **Legacy Design (1.8.9):** Uses **Raw OpenGL** (`glBegin`, `glVertex2f`). Custom font rendering (`g_fontTexture`) is used right inside the `wglSwapBuffers` hook.
*   **Nametags Option:** Nametags include a hide-vanilla toggle that attempts native nametag visibility suppression (no visual mask fallback). The modern bridge now supports Mojmap/Yarn scoreboard variants, retries mapping resolution when startup mapping is incomplete, and exposes **Reload Mappings** as a full JNI remap across modules (not nametag-only). If required mappings are unsupported on a runtime/build, it fails open and logs exactly what is missing.

### Internal Game State Modules (Reach & Velocity)
These modules modify the game state and execute within the C++ bridges, presenting the biggest architectural splits:
*   **Reach:**
    *   *Modern (1.21):* Exploits the `ENTITY_INTERACTION_RANGE` entity attribute introduced in newer Minecraft versions. The bridge fetches the player's attribute instance and dynamically invokes `setBaseValue()` to increase reach.
    *   *Legacy (1.8.9):* Performs a manual math-based raycast. When the user clicks, if an entity is within the extended reach range, the bridge overwrites the game's `objectMouseOver` field (and `pointedEntity`) with the target.
*   **Velocity:**
    *   *Modern (1.21):* Tracks the player's `hurtTime` to detect knockback. When hit, it fetches the player's `Vec3` velocity via `getVelocity()`, scales the X/Y/Z components based on the config, and applies it using `setVelocity()` (`setDeltaMovement`).
    *   *Legacy (1.8.9):* Directly scales the primitive `motionX`, `motionY`, and `motionZ` fields on the `EntityPlayer` object when `hurtTime` spikes.

---

## 7. Logs and Debugging

When working with JNI and class mappings, failures are common.
*   The C++ bridges output extensive debug information to a file named `bridge_debug.log` located in the same directory as the injected DLL.
*   Always check `bridge_debug.log` to see the "Mapping Report" output. It will explicitly list which fields (Player, World, Matrices, etc.) failed to resolve via JNI.
*   C# logging is routed to `Debug.WriteLine` and can be viewed in Visual Studio's output window or via a standard debugger attached to the loader process.
