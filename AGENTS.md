# AGENTS.md

## Purpose

Operating guide for coding agents working in `legoclickerC`. Build/test/lint commands, conventions, safety rules, and gotchas.

## Reference priority

1. `GUIDE.md` — canonical architecture, version mappings (Yarn vs Mojmap), implementation guardrails.
2. `.github/copilot-instructions.md` — lighter guidance, fully compatible with this file.
3. `AGENTS/` — extended reference docs (mapping tables, crash-log locations, reach/velocity deep dives). Some files may be stale; verify against source.
4. `README.md` — feature list and quick start.

## Repository Overview

- `Aoko/`: .NET 8 WPF loader + external GUI (publishes as `Aoko.exe`).
- `Aoko/Core/`: clicker engine, input hooks, profile persistence, TCP client, GTB solver.
- `McInjector/`: native bridge DLLs (`bridge.dll` for 1.8.9, `bridge_261.dll` for 26.1 / 1.21.x).
- `McInjector/src/main/cpp/`: JNI/Win32/OpenGL/ImGui/MinHook bridge sources.
- `McInjector/src/main/java/`: **Unused/obsolete Java agent code. Ignore it.** The C++ bridges perform all JNI, rendering, and TCP duties themselves.

## Required Toolchain

- Windows 10/11 x64.
- .NET SDK 8.x.
- MinGW-w64 at `C:\mingw64\mingw64\bin\g++.exe`.
- JDK 17 headers at `C:\Program Files\Java\jdk-17\include`.

## Build Commands

Run from repository root. Prefer PowerShell for compound commands (avoids path/quoting issues).

### Native bridge builds

- Build both bridges: `build_dll.bat`
- Build 1.8.9 bridge only: `McInjector\build.bat`
- Build 26.1 bridge only: `McInjector\build_261.bat`
- `build_bridge.bat` is deprecated (prints a message and exits).

Bridge build scripts auto-copy output to `Aoko\bin\Debug\`, `Release\`, and `publish\` folders. The csproj also includes `<CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>` for the two bridge DLLs, so `dotnet build` picks them up from the project root `Aoko\` folder.

### C# loader builds

- Debug build: `dotnet build Aoko\Aoko.csproj`
- Release build: `dotnet build -c Release Aoko\Aoko.csproj`
- Publish (self-contained single-file, used for distribution): `build_exe.bat`
- Full release pipeline (bridges + publish): `build_release.bat`

Note: the Release publish is self-contained (`SelfContained=true`, `PublishSingleFile=true`) with bridge DLLs excluded from the single file (`ExcludeFromSingleFile=true`).

### Run locally

- Run app: `dotnet run --project Aoko\Aoko.csproj`
- If `dotnet run` fails because `Aoko.exe` is locked, close the running app first.

## Lint and Formatting

No `.editorconfig` or `.clang-format` checked in. Practical quality gates:

- C# compile gate: `dotnet build Aoko\Aoko.csproj`
- Native compile gate: `McInjector\build_261.bat` and/or `McInjector\build.bat`
- Optional formatting check (if `dotnet format` is installed):
  `dotnet format Aoko\Aoko.csproj --verify-no-changes`

## Test Commands

C# tests use **xUnit** (`Aoko.Tests\`). Native harness tests are a standalone C++ exe.

- Run all C# tests: `dotnet test Aoko.Tests\Aoko.Tests.csproj`
- List tests: `dotnet test Aoko.Tests\Aoko.Tests.csproj --list-tests`
- Run a single test: `dotnet test Aoko.Tests\Aoko.Tests.csproj --filter "FullyQualifiedName~Namespace.ClassName.TestName"`
- Run one test class: `dotnet test Aoko.Tests\Aoko.Tests.csproj --filter "FullyQualifiedName~Namespace.ClassName"`
- Run native tests: `McInjector\run_tests.bat`

## Debugging

- Bridge debug logs are written to the DLL load directory:
  - `bridge_debug.log` (legacy 1.8.9 bridge)
  - `bridge_261_debug.log` (modern 26.1 / 1.21 bridge)
- C# logging goes to `Debug.WriteLine`.
- For JVM crash dumps, check `hs_err_pid*.log` in Lunar's working directory, `%USERPROFILE%\.lunarclient\`, or `%TEMP%`.

## Architecture Notes

- Loader and bridge communicate over TCP on port `25590`.
- Input simulation happens in C# via Win32 `SendInput`; bridge code must NOT send packets or call gameplay methods.
- `bridge_261.cpp` uses Yarn-first, Mojmap-fallback class name arrays to support both 1.21 (obfuscated, Yarn mappings) and 26.1 (unobfuscated, Mojang mappings) from a single DLL.
- `bridge.cpp` (1.8.9) now links the shared ImGui/OpenGL backend and MinHook sources. Do not assume legacy rendering is raw GL-only.
- **Menu-injection compatibility (1.8.9):** mappings and features must recover correctly when injected while in menus/lobby, not only when already in a world.
- Release publish `build_release.bat` copies DLLs from `McInjector\` (not `Aoko\`), so both bridges must be built first.

## Coding Style

### General
- Match existing style in the touched file; do not reformat unrelated code.
- Keep changes minimal. Avoid adding dependencies.

### C# (.NET 8, nullable enabled)
- File-scoped namespaces (`namespace X;`), 4-space indentation.
- `PascalCase` for public types/methods/properties; `_camelCase` for private fields; `camelCase` for locals/parameters.
- Remove unused usings. Group BCL namespaces before project namespaces.
- Bindable state raises `PropertyChanged`. Use `CancellationToken` for background loops. Marshal to `Dispatcher` for UI updates.
- Catch expected failures around I/O, process attach, and socket operations.

### XAML/WPF
- `DynamicResource`-based theming. Explicit bindings (`Mode=TwoWay`, `UpdateSourceTrigger=PropertyChanged` where needed).
- Reuse styles from resources rather than repeating control properties.

### C++ Bridge (C++11)
- Keep include order stable. Use `Mutex`/`LockGuard` for shared globals.
- Validate and clamp config values parsed from TCP JSON.
- Keep render-thread (wglSwapBuffers hook) code lightweight; cache method/field IDs.
- JNI: check nulls, clear exceptions, manage local refs/frames in loops.

## Domain Safety Rules

- Do NOT add packet-sending or gameplay-mutating calls in bridge code.
- Do NOT call in-game combat methods (`attackEntity`, packet APIs, etc.).
- Prefer read-only JNI state access. Use Win32 `SendInput` from C# for simulated input.
- Limited JNI writes allowed when ghost-safe (reach via entity attributes, velocity scaling, nametag visibility suppression). Keep overlays draw-only; mutate state only when justified and stealthy.

## Configuration Sync

When adding a setting, update ALL of:
- `Clicker` property
- `Profile` save/load mapping
- `GameStateClient` config payload (if bridge-relevant)
- `BridgeCapabilities.cs` and `bridge_capabilities.h` if version/module gating changes
- `InputHooks` and keybind UI maps if a module gets a keybind
- bridge parser/usage (if native overlay/module behavior depends on it)

## High-Signal Gotchas

- `JNIEnv*` is thread-local. Using it from an unattached thread will crash. Always `AttachCurrentThread` from non-render threads.
- `build_bridge.bat` is a no-op stub. Use `McInjector\build.bat` or `McInjector\build_261.bat`.
- The csproj auto-copies bridge DLLs from `Aoko\` root. Build scripts also copy into `bin\` folders. After a native rebuild, ensure the active run configuration has the latest DLL.
- `bridge_261.cpp` fallback-array parsing: Yarn names are tried first, then Mojmap. Adding a new class lookup MUST follow the same pattern or dual-version support breaks.

## Agent Workflow

- Before finishing, run the relevant build command(s).
- Report exact commands run and results.
- Do not revert unrelated user changes.
