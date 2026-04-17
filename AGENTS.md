# AGENTS.md

## Purpose

This file is the operating guide for coding agents working in `legoclickerC`.
It documents build/test/lint commands, repository conventions, and safety rules.

## Primary technical reference

- Read `GUIDE.md` first for project architecture, version-specific mapping behavior, and implementation guardrails.
- When this file and `GUIDE.md` overlap, keep behavior decisions aligned with `GUIDE.md`.

## Repository Overview

- `LegoClickerCS/`: .NET 8 WPF loader + external GUI (`LegoClicker` executable).
- `LegoClickerCS/Core/`: clicker engine, input hooks, profile persistence, TCP client.
- `McInjector/`: native bridge DLL builds (`bridge.dll`, `bridge_261.dll`).
- `McInjector/src/main/cpp/`: JNI/Win32/OpenGL/ImGui bridge sources.
- `build_dll.bat`, `build_exe.bat`, `build_release.bat`: top-level build helpers.

## Required Toolchain

- Windows 10/11 x64.
- .NET SDK 8.x.
- MinGW-w64 at `C:\mingw64\mingw64\bin\g++.exe`.
- JDK 17 headers at `C:\Program Files\Java\jdk-17\include`.

## Build Commands

Run from repository root unless noted.

Build shell preference:

- prefer PowerShell invocation for build scripts and compound Windows build commands
- avoid `cmd.exe /c ...` unless explicitly required for a specific script behavior
- this avoids intermittent path/quoting issues observed in this repository tooling

### Native bridge builds

- Build both bridges: `build_dll.bat`
- Build 1.8.9 bridge only: `McInjector\build.bat`
- Build 26.1 bridge only: `McInjector\build_261.bat`
- `build_bridge.bat` is deprecated and kept as a compatibility stub.

### C# loader builds

- Debug build: `dotnet build LegoClickerCS\LegoClickerCS.csproj`
- Release build: `dotnet build -c Release LegoClickerCS\LegoClickerCS.csproj`
- Publish release exe: `build_exe.bat`
- Full release pipeline: `build_release.bat`

### Run locally

- Run app: `dotnet run --project LegoClickerCS\LegoClickerCS.csproj`
- If `dotnet run` fails because `LegoClicker.exe` is locked, close the running app first.

## Lint and Formatting

There is no dedicated lint config checked in (`.editorconfig`, `.clang-format`, and linter configs are not present).

Use these as practical quality gates:

- C# compile gate: `dotnet build LegoClickerCS\LegoClickerCS.csproj`
- Optional formatting check (if available in environment):
  - `dotnet format LegoClickerCS\LegoClickerCS.csproj --verify-no-changes`
- Native compile gate: `McInjector\build_261.bat` and/or `McInjector\build.bat`

## Test Commands

Current state: no dedicated test projects are present in this repository.

When tests are added, use standard .NET test commands:

- Run all tests in a project:
  - `dotnet test path\to\YourTests.csproj`
- List tests:
  - `dotnet test path\to\YourTests.csproj --list-tests`
- Run a single test (important):
  - `dotnet test path\to\YourTests.csproj --filter "FullyQualifiedName~Namespace.ClassName.TestName"`
- Run tests by class:
  - `dotnet test path\to\YourTests.csproj --filter "FullyQualifiedName~Namespace.ClassName"`

## Coding Style - General

- Match existing style in the touched file; do not reformat unrelated code.
- Keep changes minimal and targeted.
- Prefer clear, descriptive names over clever abbreviations.
- Avoid adding dependencies unless required.
- Do not introduce non-ASCII unless file/content requires it.

## Coding Style - C#

- Language/version: .NET 8, nullable enabled.
- Namespace style: file-scoped (`namespace X;`).
- Indentation: 4 spaces, no tabs.
- Usings:
  - Keep `using` directives at file top.
  - Remove unused usings.
  - Group BCL/usual namespaces before project namespaces.
- Naming:
  - `PascalCase`: public types, methods, properties, events.
  - `_camelCase`: private fields.
  - `camelCase`: locals/parameters.
  - Constants follow existing local pattern (both `PascalCase` and all-caps VK constants exist).
- Properties/events:
  - Raise `PropertyChanged` for bindable state.
  - Keep `StateChanged` event signaling behavior consistent with existing modules.
- Validation:
  - Clamp or validate values at setters/boundaries (see CPS/range/chance patterns).
- Async/concurrency:
  - Keep background loops cancellable via `CancellationToken`.
  - Avoid blocking UI thread; marshal to `Dispatcher` when touching UI-bound state.
- Error handling:
  - Catch expected runtime failures around I/O, process attach, and socket operations.
  - Prefer fail-safe defaults and status/log updates over hard crashes.

## Coding Style - XAML/WPF

- Preserve `DynamicResource`-based theming model.
- Keep bindings explicit (`Mode=TwoWay`, `UpdateSourceTrigger=PropertyChanged` where required).
- Prefer reusable styles in resources over repeated control properties.
- New controls should use established visual language in `MainWindow.xaml`.

## Coding Style - C++ Bridge

- Standard target: C++11 (per build scripts).
- Keep include order stable and compatible with MinGW/JNI headers.
- Follow existing brace/spacing style in target file.
- Use existing lock primitives (`Mutex`, `LockGuard`) for shared globals.
- Validate and clamp config values parsed from TCP JSON.
- Keep render-thread code lightweight; avoid expensive JNI calls on render path unless already patterned.
- JNI safety:
  - Check nulls before method/field use.
  - Clear/handle JNI exceptions where applicable.
  - Clean local refs / frames in iterative or long-running paths.

## Domain Safety Rules (Critical)

The project intentionally behaves like an external input/overlay tool.

- Do not add packet-sending or gameplay-mutating calls in bridge code.
- Do not call in-game combat actions directly (`attackEntity`, packet APIs, etc.).
- Prefer read-only JNI state access + Win32 `SendInput` for simulated input behavior.
- Keep overlay work draw-only; avoid mutating Minecraft state.

## Configuration and Persistence Conventions

- Keep `Clicker` state, `Profile` serialization, and bridge config JSON in sync.
- When adding a setting, update all of:
  - `Clicker` property
  - `Profile` save/load mapping
  - `GameStateClient` config payload (if bridge-relevant)
  - bridge parser/usage (if native overlay/module behavior depends on it)

## Agent Workflow Expectations

- Before finishing, run relevant build command(s) for changed areas.
- Report exact commands run and meaningful results.
- If unable to run a step (tool missing, process lock, environment issue), state it clearly and provide the next manual command.
- Do not revert unrelated user changes in a dirty worktree.

## Cursor/Copilot Rules

- `.cursor/rules/`: not found in this repository.
- `.cursorrules`: not found in this repository.
- `.github/copilot-instructions.md`: present and should be treated as higher-priority assistant guidance.

If additional higher-priority instruction files are added later, treat them as authoritative and update this file accordingly.
