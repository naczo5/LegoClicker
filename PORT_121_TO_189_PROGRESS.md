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
- porting Triggerbot runtime to 1.8.9 (module is cooldown-era PvP specific)

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
| Module list module coverage | Includes all enabled modules | Missing some entries | In progress | Add missing labels (AimAssist/GTB/Reach/Velocity); Triggerbot intentionally omitted |
| Config parsing parity | Broad | Partial | In progress | 1.8.9 ignores several sent fields |
| State telemetry parity | Rich state payload | Minimal payload | In progress | Missing actionBar/entity/cooldown related fields |
| GTB helper runtime | Enabled | UI-gated unavailable | Not started | Needs action bar + overlay parity |
| Aim Assist runtime | Enabled | UI-gated unavailable | Not started | Needs stable entity telemetry parity |
| Triggerbot runtime | Enabled | Intentionally unavailable | Omitted | Kept unavailable on 1.8.9 by design |
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

Status: In progress

Why second:

- required for AimAssist/GTB parity and protocol consistency
- prevents duplicate fallback logic in C#

Note:

- Triggerbot itself is omitted on 1.8.9, but these state fields remain useful for telemetry consistency and future-safe protocol parity.

Implementation tasks:

- [x] extend 1.8.9 `GameState` JSON payload with:
  - `actionBar`
  - `lookingAtEntity`
  - `lookingAtEntityLatched`
  - `breakingBlock`
  - `attackCooldown`
  - `attackCooldownPerTick`
  - `stateMs`
- [x] ensure safe defaults when world/player are unavailable
- [x] keep payload format compatible with current `GameState.cs`

Progress notes:

- 1.8.9 bridge now emits expanded state fields in outbound JSON.
- Added conservative 1.8.9 derivation for parity-only fields:
  - `lookingAtEntity` / `lookingAtEntityLatched` from `objectMouseOver`
  - `breakingBlock` from `lookingAtBlock + LMB held`
  - `attackCooldown=1.0` and `attackCooldownPerTick=0.08` fallback defaults
  - `stateMs` from `GetTickCount64()`

Remaining for P2 completion:

- [ ] runtime verification on real 1.8.9 session that all new fields deserialize and update as expected

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

- [x] parse and clamp in 1.8.9:
  - `gtbHint`, `gtbCount`, `gtbPreview`
  - `nametagMaxCount`, `chestEspMaxCount`
  - `reachMin`, `reachMax`, `reachChance`
  - `velocityHorizontal`, `velocityVertical`, `velocityChance`
- [ ] add any missing keybind fields if later needed for parity

Progress notes:

- 1.8.9 parser now accepts and clamps the extended parity fields.
- Runtime behavior for these fields is still phased:
  - parsed now for protocol parity and safe no-op consumption
  - behavioral wiring lands in later phases (P5+ / module phases)

Files expected:

- `McInjector/src/main/cpp/bridge.cpp`

Exit criteria:

- all settings sent from `ConfigSenderLoop` are either consumed in 1.8.9 or intentionally documented as unsupported

## P4 - Entity Telemetry Decoupling in 1.8.9

Status: In progress

Why fourth:

- currently 1.8.9 entity payload production is coupled to nametags path
- AimAssist should not require nametags module to be enabled

Implementation tasks:

- [x] generate entity telemetry whenever any consumer requires it (aim/trigger/closest/nametags)
- [x] keep CPU budget controlled (adaptive sleep/update interval)
- [ ] preserve current stability fixes for menu-injection transitions

Progress notes:

- Server loop now requests telemetry production when any current 1.8.9 consumer is active (`nametags`, `closest player`, `aim assist`) instead of tying pacing to nametags only.
- Telemetry polling sleep windows now adapt to telemetry demand rather than nametag-only demand.
- Entity JSON cache now resets to `[]` each telemetry pass to avoid stale entity reuse when projection/list fetch fails.

Remaining for P4 completion:

- [ ] runtime verification that `entities` keeps updating with nametags OFF and closest/aim ON
- [ ] validate no CPU regression when all telemetry consumers are OFF

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

- [x] add missing module list entries when enabled:
  - `Aim Assist`, `GTB Helper`, `Reach`, `Velocity`
- [x] add GTB info panel rendering using `gtbHint/gtbCount/gtbPreview`
- [x] enforce `nametagMaxCount` in nametag rendering
- [x] enforce `chestEspMaxCount` in chest ESP rendering

Progress notes:

- 1.8.9 HUD module list now includes missing parity entries (Triggerbot intentionally omitted).
- 1.8.9 HUD now renders GTB panel from bridge-config data (`gtbHint`, `gtbCount`, `gtbPreview`).
- 1.8.9 nametag and chest ESP render loops now honor configured max-count limits.

Remaining for P5 completion:

- [ ] runtime visual parity pass (layout/readability/overflow) vs 1.21 for all styles

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

## P8 - Triggerbot on 1.8.9 (Omitted)

Status: Done

Decision:

- Triggerbot is intentionally not being ported to 1.8.9.
- Rationale: it is tied to cooldown-era PvP behavior and does not map cleanly to legacy 1.8.9 combat expectations.

Implementation outcomes:

- keep Triggerbot unavailable on 1.8.9 via bridge capabilities
- keep UI explicit about unavailability reason
- keep input/keybind toggle blocked when unsupported

Files affected:

- `LegoClickerCS/Core/BridgeCapabilities.cs`
- `LegoClickerCS/MainWindow.xaml.cs`
- `LegoClickerCS/Core/InputHooks.cs`

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

- [ ] remove unavailable labels for modules once parity is done (except intentionally omitted Triggerbot on 1.8.9)
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
- Triggerbot is intentionally omitted on 1.8.9 due combat model mismatch
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

### 2026-03-27 (config regression fix)

- Fixed profile persistence for autoclicker armed state:
  - added `IsArmed` to profile schema
  - save/load now restores armed state between sessions

## Quick Triage Checklist (When New Bug Is Reported)

- [ ] classify as protocol, rendering, input, or module logic gap
- [ ] mark impacted phase (P1-P12)
- [ ] add reproduction steps and expected parity behavior
- [ ] add fix task under corresponding phase
- [ ] link verification scenario from matrix
