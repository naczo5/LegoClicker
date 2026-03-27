# 1.21 to 1.8.9 Port Progress Tracker

Last updated: 2026-03-27

## Purpose

This document tracks the ongoing parity port from the 1.21 bridge/behavior set to 1.8.9.

Primary objective:

- minimize practical differences between `bridge_121.dll` and `bridge.dll`
- keep one consistent user experience in the external control center
- avoid version-specific surprises where possible

## Scope

In scope:

- feature behavior parity (modules, settings, telemetry)
- overlay/HUD parity (module list, GTB panel, rendering behavior)
- config and command protocol parity between C# and native bridge
- runtime reliability parity (menu inject, world join, GUI state handling)

Out of scope:

- introducing new modules not present in 1.21
- changing project safety rules (no packet/combat API injection)

## Workflow Agreement

To make rollback and bisect easier during this port:

- create small local checkpoint commits while progressing through phases
- do not push commits unless explicitly requested
- prefer one logical commit per completed slice (protocol, UI gating, module behavior, etc.)

## Status Legend

- Not started: no implementation yet
- In progress: partially implemented, not parity-complete
- Blocked: depends on prerequisite work
- Done: merged and verified with defined acceptance checks

## Baseline Already Completed

These are completed and serve as the starting point for the remaining parity port.

- [x] Insert key no longer opens GUI on either version
- [x] External control center flow works for 1.8.9 (`toggleExternalGui` path)
- [x] 1.21 module-list styles/themes are rendered in 1.8.9 HUD
- [x] 1.21-only modules are marked unavailable on 1.8.9 in WPF UI
- [x] 1.21-only toggles are blocked in input hooks when not on 1.21
- [x] stale unsupported toggles auto-clear on non-1.21
- [x] module list hidden in menus on 1.8.9 (`state.guiOpen` gating)
- [x] major menu-injection mapping recovery work added (late binding/retry)
- [x] bridge log path fixed to write next to loaded DLL
- [x] 1.8.9 build copy flow improved to reduce stale DLL mismatch

## Parity Snapshot (Current)

| Area | 1.21 | 1.8.9 current | Status | Gap summary |
|---|---|---|---|---|
| External GUI flow | External-first | External-first | Done | Core behavior aligned |
| Insert behavior | Disabled | Disabled | Done | Aligned |
| Theme + module list style | Full | Full | Done | Aligned |
| Module list module coverage | Includes all enabled modules | Missing some entries | In progress | Add missing labels (AimAssist/Triggerbot/GTB/Reach/Velocity) |
| Config parsing parity | Broad | Partial | In progress | 1.8.9 ignores several sent fields |
| State telemetry parity | Rich state payload | Minimal payload | In progress | Missing actionBar/entity/cooldown related fields |
| GTB helper runtime | Enabled | UI-gated unavailable | Not started | Needs action bar + overlay parity |
| Aim Assist runtime | Enabled | UI-gated unavailable | Not started | Needs stable entity telemetry parity |
| Triggerbot runtime | Enabled | UI-gated unavailable | Not started | Needs lookingAtEntity/cooldown parity signals |
| Reach runtime | Enabled | UI-gated unavailable | Not started | Needs 1.8-specific implementation |
| Velocity runtime | Enabled | UI-gated unavailable | Not started | Needs 1.8-specific implementation |
| Nametag/chest caps | Supported (`nametagMaxCount`, `chestEspMaxCount`) | Not applied | Not started | Parse and enforce caps in 1.8.9 |

## Ordered Port Roadmap (First to Last)

## P1 - Capability Handshake and Unified Gating

Status: In progress

Why first:

- removes brittle version-string checks
- allows progressive enablement per feature on 1.8.9 without UI churn

Implementation tasks:

- [x] add bridge -> C# capability packet (supported modules + supported settings)
- [x] keep version string, but make capability set the source of truth for availability
- [x] route `MainWindow.xaml.cs` availability UI through capabilities
- [x] route `InputHooks.cs` toggle guards through capabilities
- [x] route loop enable conditions in `Clicker.cs` through capabilities

Progress notes:

- Added capability packet emission in both bridges:
  - `McInjector/src/main/cpp/bridge.cpp`
  - `McInjector/src/main/cpp/bridge_121.cpp`
- Added capability model and parser:
  - `LegoClickerCS/Core/BridgeCapabilities.cs`
  - `LegoClickerCS/Core/GameStateClient.cs`
- Migrated key gating call sites to capability checks:
  - `LegoClickerCS/MainWindow.xaml.cs`
  - `LegoClickerCS/Core/InputHooks.cs`
  - `LegoClickerCS/Core/Clicker.cs`

Remaining for P1 completion:

- [ ] live runtime verification that capability packet is always received after connect
- [ ] verify fallback behavior when capability packet is absent/delayed

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`
- `McInjector/src/main/cpp/bridge_121.cpp`
- `LegoClickerCS/Core/GameStateClient.cs`
- `LegoClickerCS/MainWindow.xaml.cs`
- `LegoClickerCS/Core/InputHooks.cs`
- `LegoClickerCS/Core/Clicker.cs`

Exit criteria:

- availability and toggling behavior no longer depend on `StartsWith("1.21")`
- unsupported modules/settings are disabled by capability, not by hardcoded version check

## P2 - State Payload Parity (1.8.9 -> C#)

Status: Not started

Why second:

- required for Triggerbot/AimAssist/GTB parity
- prevents duplicate fallback logic in C#

Implementation tasks:

- [ ] extend 1.8.9 `GameState` JSON payload with:
  - `actionBar`
  - `lookingAtEntity`
  - `lookingAtEntityLatched`
  - `breakingBlock`
  - `attackCooldown`
  - `attackCooldownPerTick`
  - `stateMs`
- [ ] ensure safe defaults when world/player are unavailable
- [ ] keep payload format compatible with current `GameState.cs`

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`
- `LegoClickerCS/Core/GameState.cs` (only if schema changes)

Exit criteria:

- C# receives non-empty values for these fields on 1.8.9
- no JSON parse regressions in `GameStateClient.ReadLoop`

## P3 - Config Parsing Parity (C# -> 1.8.9)

Status: In progress

Current state:

- 1.8.9 parses core toggles and style/theme
- 1.8.9 does not yet parse several values already sent by C#

Implementation tasks:

- [ ] parse and clamp in 1.8.9:
  - `gtbHint`, `gtbCount`, `gtbPreview`
  - `nametagMaxCount`, `chestEspMaxCount`
  - `reachMin`, `reachMax`, `reachChance`
  - `velocityHorizontal`, `velocityVertical`, `velocityChance`
- [ ] add any missing keybind fields if later needed for parity

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`

Exit criteria:

- all settings sent from `ConfigSenderLoop` are either consumed in 1.8.9 or intentionally documented as unsupported

## P4 - Entity Telemetry Decoupling in 1.8.9

Status: Not started

Why fourth:

- currently 1.8.9 entity payload production is coupled to nametags path
- AimAssist/Triggerbot should not require nametags module to be enabled

Implementation tasks:

- [ ] generate entity telemetry whenever any consumer requires it (aim/trigger/closest/nametags)
- [ ] keep CPU budget controlled (adaptive sleep/update interval)
- [ ] preserve current stability fixes for menu-injection transitions

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`

Exit criteria:

- `CurrentState.Entities` updates on 1.8.9 even when nametags are disabled, if other consumers are active

## P5 - Overlay/HUD Parity in 1.8.9

Status: In progress

Current state:

- styles/themes/logo parity is done
- module list coverage and GTB panel parity are incomplete

Implementation tasks:

- [ ] add missing module list entries when enabled:
  - `Aim Assist`, `Triggerbot`, `GTB Helper`, `Reach`, `Velocity`
- [ ] add GTB info panel rendering using `gtbHint/gtbCount/gtbPreview`
- [ ] enforce `nametagMaxCount` in nametag rendering
- [ ] enforce `chestEspMaxCount` in chest ESP rendering

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`

Exit criteria:

- on same settings, 1.8.9 and 1.21 HUD content are functionally equivalent

## P6 - GTB Helper Runtime Parity

Status: Not started

Implementation tasks:

- [ ] extract action bar text on 1.8.9 bridge side
- [ ] send action bar text in state payload on 1.8.9
- [ ] remove 1.21-only dispatch gate for GTB state update in C#
- [ ] validate solver behavior and panel output in both versions

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`
- `LegoClickerCS/Core/GameStateClient.cs`
- `LegoClickerCS/Core/Clicker.cs` (if gating logic changes)

Exit criteria:

- GTB helper works equivalently on both versions

## P7 - Aim Assist Runtime Parity

Status: Not started

Implementation tasks:

- [ ] remove 1.21-only runtime gate for AimAssist when capability available
- [ ] validate targeting quality on 1.8.9 with same settings
- [ ] ensure no operation during GUI/menu screens

Files expected:

- `LegoClickerCS/Core/Clicker.cs`
- `LegoClickerCS/MainWindow.xaml.cs` (availability)
- optional bridge updates in `bridge.cpp` if additional telemetry needed

Exit criteria:

- same UX and control semantics for aim assist on both versions

## P8 - Triggerbot Runtime Parity

Status: Not started

Implementation tasks:

- [ ] remove 1.21-only runtime gate for Triggerbot when capability available
- [ ] align entity detection and latency/cooldown timing behavior as closely as 1.8 allows
- [ ] keep break-block and GUI safety behavior consistent

Files expected:

- `LegoClickerCS/Core/Clicker.cs`
- optional bridge updates in `bridge.cpp`

Exit criteria:

- Triggerbot behavior is comparable across versions with same settings

## P9 - Reach Parity

Status: Not started

Implementation tasks:

- [ ] implement 1.8.9-side reach behavior with settings parity:
  - enable toggle
  - min/max range
  - chance
- [ ] keep implementation compliant with project safety constraints

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`
- `LegoClickerCS/MainWindow.xaml.cs` (availability flip when ready)
- `LegoClickerCS/Core/InputHooks.cs` (remove guard when ready)

Exit criteria:

- Reach module is available and functioning on 1.8.9 with same controls

## P10 - Velocity Parity

Status: Not started

Implementation tasks:

- [ ] implement 1.8.9-side velocity scaling with settings parity:
  - enable toggle
  - horizontal %, vertical %, chance
- [ ] ensure stable detection and no unintended movement side effects

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`
- `LegoClickerCS/MainWindow.xaml.cs` (availability flip when ready)
- `LegoClickerCS/Core/InputHooks.cs` (remove guard when ready)

Exit criteria:

- Velocity module is available and functioning on 1.8.9 with same controls

## P11 - UI and Keybind Unification Cleanup

Status: Not started

Implementation tasks:

- [ ] remove `Unavailable: 1.21 only` for modules once parity is done
- [ ] ensure keybind rows/buttons and module cards are consistent across versions
- [ ] confirm profile save/load symmetry after all gates removed

Files expected:

- `LegoClickerCS/MainWindow.xaml`
- `LegoClickerCS/MainWindow.xaml.cs`
- `LegoClickerCS/Core/Profile.cs`

Exit criteria:

- no user-facing version split for completed modules

## P12 - Final Hardening and Regression Pass

Status: Not started

Implementation tasks:

- [ ] run full build gates:
  - `build_dll.bat`
  - `dotnet build LegoClickerCS\\LegoClickerCS.csproj`
- [ ] execute parity scenario matrix (below)
- [ ] remove dead/legacy code paths no longer needed
- [ ] freeze tracker and mark parity release candidate

Exit criteria:

- parity checklist is green
- no known critical regressions in menu/world transitions

## Verification Matrix

Run these for each major phase completion:

- [ ] inject in main menu -> join world -> overlays and toggles work
- [ ] inject in-world -> overlays and toggles work
- [ ] module list hidden in menus in both versions
- [ ] external control center opens reliably from bridge command
- [ ] no Insert-based GUI opening on either version
- [ ] profiles persist and re-load all module settings correctly
- [ ] keybind capture and toggle behavior match expected availability

## Risks and Notes

- 1.8.9 JNI mapping variability can still create edge-case render telemetry gaps
- 1.8.9 and 1.21 combat internals differ; Triggerbot parity should be behaviorally close, not byte-identical
- Reach/Velocity on 1.8.9 must stay within project safety constraints

## Progress Log

### 2026-03-27

- Baseline parity prep completed:
  - external GUI path active on 1.8.9
  - Insert disabled on both versions
  - module list style/theme parity on 1.8.9
  - 1.21-only module unavailability UI/guards added
  - menu visibility and mapping recovery fixes applied
- Remaining work shifted to full feature and telemetry parity plan (P1-P12)

## Quick Triage Checklist (When New Bug Is Reported)

- [ ] classify as protocol, rendering, input, or module logic gap
- [ ] mark impacted phase (P1-P12)
- [ ] add reproduction steps and expected parity behavior
- [ ] add fix task under corresponding phase
- [ ] link verification scenario from matrix
