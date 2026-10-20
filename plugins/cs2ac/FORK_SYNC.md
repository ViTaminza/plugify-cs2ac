# CS2AC fork sync (v3 → v11) — change map and port status

Source of truth: the Metamod fork `ezekutor/cs2ac`, first commit `59e58ae` ("Import CS2AC fork as
deployed on 2026-09-29", already contains v3+v4 and the pre-import v2 changes) through HEAD
`9ff13f7` (v11). Our port was made from vanilla CS2AC 1.0.14, so the work is the diff
`cs2ac-legacy → fork HEAD`, filtered through `PORT_PLAN.md`'s mapping.

Reference copies used while porting (gitignored): `.scratch/ref-cs2ac/{legacy,fork-import-59e58ae,
fork-head,patches,CHANGELOG}`.

Status: **done** = ported, built and covered by tests; **n/a** = deliberately not ported (reason);
**gap** = cannot be completed in this stack/sandbox (documented, visible at runtime).

## How the fork's primitives map into the port

| Fork primitive | Port location |
|---|---|
| `settings::Configuration` CConVars | `src/config.cpp` `settings::kConVars` / `kNumericConVars` |
| `CS2ACPlugin::HandleDetection` | `src/plugin.cpp` `Plugin::HandleDetection` |
| `utils::AnnounceDetectionThrottled` | `src/announce.cpp` + `Plugin::HandleDetection` |
| `detection_log::{Write,WriteTelemetry,JsonString}` | `src/detection_log.cpp` (same JSONL shape) |
| `admin_bridge` (`IAdminApi`) | `src/admin_bridge.cpp` on `admin_system::AdminApi` |
| `MovementPlayer`, `PlayerCommand` | `PlayerRef`, `UsercmdView` (`core.hpp`) |
| `schema::HasField` / typed schema reads | `engine::` offset-cached raw reads (`GetEntData*`) |
| `g_pCS2ACUtils->GetServerGlobals()` | `engine::{CurrentTick,CurTime,Realtime}` |
| `engine->GetPlayerNetInfo` (detection log) | `engine::ClientNetStats` |
| `IMetamodListener` map hooks | existing s2sdk MapStart/MapEnd listeners (`hooks.cpp`) |
| fork pure decisions | `src/decisions.cpp` (engine-free, unit-tested) |

## Package v3 + v4 + rcs_spray JSON hotfix (fork import `59e58ae`)

### v3 — false-positive fixes from the first live day
| # | Behaviour | Fork file | Port location | Status |
|---|---|---|---|---|
| 3.1 | TICK MANIPULATION reports only `player_tick_count == 0` shots (3 within 10 s); far ticks are telemetry only (`far_ticks`, min/max delta) | `tick_integrity_module.cpp` | `mod_integrity.cpp` | done |
| 3.2 | BACKTRACK ignores a dip when the client clock jumped more than `cs2ac_backtrack_max_gap` ticks around the shot; counted as `hitch_skips` | `tick_integrity_module.cpp` | `mod_integrity.cpp`, `decisions.cpp` | done |
| 3.3 | TRIGGER PATTERN reports instant taps only when ≥ 90% of instant taps at a nearby visible enemy started on that enemy (≥ 10 samples) | `trigger_pattern_module.cpp` | `mod_pattern.cpp` | done |

### v4 — RCS on the new aim-punch component
| # | Behaviour | Fork file | Port location | Status |
|---|---|---|---|---|
| 4.1 | Aim punch moved to `CCSPlayer_AimPunchServices`; RCS reads `m_predictableBaseAngle`, `m_unpredictableBaseAngle` (+ base ticks) through `CCSPlayerPawn::m_pAimPunchServices` | `recoil_control_module.cpp`, `sdk/{entity/ccsplayerpawn,services}.h` | `mod_recoil.cpp` + `engine::ReadRecoilData` | done |
| 4.2 | All required schema fields checked; RCS stays off (no dereference) if any is missing, logging which group failed | `RecoilControlModule::Load` | lazy probe in `engine::RecoilSchemaAvailable` (after a map exists — `GetSchemaOffset` must not run in `OnPluginStart`, PORT_PLAN §8c) | done |
| 4.3 | RCS reports OFF by default (`cs2ac_rcs_report 0`); telemetry always written, raw per-tick series for the first 3 sprays/player/map | `recoil_control_module.cpp` | `mod_recoil.cpp`, `detection_log.cpp` | done |
| 4.4 | New convar `cs2ac_backtrack_max_gap 4` | `tick_integrity_module.cpp` | `config.cpp` (`kNumericConVars`) | done |
| 4.5 | JSON hotfix: capture the base rcs_spray object before reusing `buffer` for random lags | `recoil_control_module.cpp` | `mod_recoil.cpp` (`const std::string base`) | done |

### Earlier fork changes that came in the same import (not in CHANGELOG's v3/v4 sections)
| # | Behaviour | Fork file | Port location | Status |
|---|---|---|---|---|
| I.1 | NEW MODULES: `TickIntegrityModule`, `TriggerPatternModule`, `RecoilControlModule` | `detection/{tick_integrity,trigger_pattern,recoil_control}_module.cpp` | `mod_integrity.cpp`, `mod_pattern.cpp`, `mod_recoil.cpp` | done |
| I.2 | `detection_log` — append-only JSONL detections + telemetry | `detection_log.cpp/.h` | `detection_log.cpp/.h`, `Plugin::OnStart/OnEnd` | done |
| I.3 | `admin_bridge` — queued bans through `IAdminApi`, executed outside hooks; offline bans flushed on map change/unload | `admin_bridge.cpp/.h` | `admin_bridge.cpp/.h` on `admin_system::AdminApi` (`AddPlayerPunishment` + `AddOfflinePlayerPunishment`, no API gap) | done |
| I.4 | Punishment model `DetectionSeverity {Report,Kick,Ban}`, ban/kick lists, weak-signal policy, report-only failsafe (`ReportOnly` outcome), REPORT level | `cs2ac.cpp`, `settings.cpp`, `utils/utils_print.cpp`, `webhook.cpp` | `plugin.cpp`, `config.cpp`, `announce.cpp`, `decisions.cpp` | done |
| I.5 | `cs2ac_announce_cooldown 30` (same steamid+detection+outcome throttled) | `cs2ac.cpp`, `utils/utils_print.cpp` | `announce.cpp` | done |
| I.6 | `cs2ac_update_repository ""`, `cs2ac_auto_update` default 0; `GetUpdateRepository` | `settings.cpp/.h` | `config.cpp` (updater itself stays unported) | done |
| I.7 | `IsReportOnlyDetection` (import: TRIGGER PATTERN/RCS; v9 leaves only RCS) | `cs2ac.cpp` | `decisions.cpp` | done |
| I.8 | `CheckCommandTemplate`: `mm_ban`/`!ban` + `{userid}` warning | `cs2ac.cpp` | `config.cpp` | done |
| I.9 | OnClientFullyConnect resets the slot's punishment state | `cs2ac.cpp` | `plugin.cpp` | done |
| I.10 | cfg: auto-update defaults/update repository | `cfg/cs2ac.cfg` | `assets/cs2ac.cfg` | done |
| I.11 | TRIGGERBOT reaction measured in server ticks (`contactStartTick`) | `triggerbot_module.cpp` | `mod_shots.cpp` | done |
| I.12 | TRIGGERBOT forwards each command to TriggerPattern (no extra traces) | `triggerbot_module.cpp`, `trigger_pattern_module.cpp` | `mod_shots.cpp`, `mod_pattern.cpp` | done |
| I.13 | Aimbot pending-shot queue (earlier hurt cannot be overwritten) | `aimbot_module.cpp` | `mod_aim.cpp`, `cs2ac.hpp` | done |
| I.14 | Aimbot close-range rules: minimum distance 64, close-range convergence/snap-return thresholds, discontinuous command pairs skipped instead of aborting, smooth rule disabled on gaps | `aimbot_module.cpp` | `mod_aim.cpp` | done |
| I.15 | SILENTAIM subtick-travel plausibility (30° margin, 90° fallback) | `silent_aim_module.cpp` | `mod_shots.cpp` | done |
| I.16 | ANTIAIM continuous spin/jitter needs horizontal speed ≥ 8 | `antiaim_module.cpp` | `mod_antiaim.cpp` | done |
| I.17 | INVALID INPUT: each cmdNum once (`lastSubtickCmdNum`); gaps clear the four evidence buffers | `subtick.cpp`, `movement_detection.{h,cpp}` | `mod_movement.cpp`, `cs2ac.hpp` | done |
| I.18 | AUTOSTRAFE optimizer only airborne with horizontal speed ≥ 50 | `strafe_optimizer.cpp` | `mod_movement.cpp` | done |
| I.19 | Shot correlation: 2-tick match / 4-tick lifetime; several compatible candidates resolve to the closest (server tick, then command/id) | `detection_system.cpp` | `detection.cpp` | done |
| I.20 | SILENTAIM per-hit / per-2-minute telemetry; `smokeClearedByHe`, mouse dx/dy on `ShotRecord` | `detection_system.{h,cpp}`, `silent_aim_module.cpp` | `mod_shots.cpp`, `core.hpp`, `detection_log.cpp` | done |
| I.21 | `SmokeClearTracker` (smokes/HE blasts, `smokegrenade_expired`, `round_start`) | `detection_system.{h,cpp}` | `detection.cpp` + new consumed events in `hooks.cpp` | done |

## v5 — BACKTRACK fingerprint, INTERP BACKTRACK telemetry, 20 ms taps, angle-input telemetry
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 5.1 | BACKTRACK counts a dip only with the rewrite fingerprint and fractional neighbours; `cs2ac_backtrack_require_forged 1` restores the v3 rule with 0 | `mod_integrity.cpp`, `decisions.cpp` | done |
| 5.2 | Negative clock gap = stall, like `> cs2ac_backtrack_max_gap` | `decisions.cpp` | done |
| 5.3 | Every dip written as `backtrack_shot` telemetry with `forged`; `unforged_dips` in `tick_stats` | `mod_integrity.cpp` | done |
| 5.4 | INTERP BACKTRACK: render−sv_interp0 offset histogram, `interp_shot` rows (10/player), report off by default | `mod_integrity.cpp`, `config.cpp` | done |
| 5.5 | View angle vs mouse input telemetry and yaw-sign agreement | `mod_integrity.cpp` | done |
| 5.6 | TRIGGER PATTERN fast-tap limit configurable (`cs2ac_trigger_pattern_fast_tap_ms 20`), evidence takes `{limit}` | `mod_pattern.cpp`, `config.cpp` | done |
| 5.7 | 37 translation files updated | `translations/*.txt` (fork HEAD) | done |

## v6 — SILENTAIM telemetry
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 6.1 | `silentaim_hit` rows (≤60/player/map) | `mod_shots.cpp` | done |
| 6.2 | `silentaim_stats` every 2 min with excess and camera histograms | `mod_shots.cpp` | done |
| 6.3 | `ShotRecord` carries `silentMouseDx/Dy` | `core.hpp`, `detection.cpp` | done |

## v7 — SILENTAIM weighs the camera
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 7.1 | Still camera (<0.5°) adds +1 point | `decisions.cpp` (`SilentAimPoints`) | done |
| 7.2 | Turning mismatch < 2° counts half, rounded up | `decisions.cpp` | done |
| 7.3 | Two new evidence sentences in all 37 translations | `translations/*.txt` | done |

## Join notice off by default
| # | Behaviour | Port location | Status |
|---|---|---|---|
| J.1 | `cs2ac_join_watermark` (bool, default 0); notice only when on | `config.cpp`, `plugin.cpp` | done |

## Gamedata fix (23 Sep CS2 update)
| # | Behaviour | Port location | Status |
|---|---|---|---|
| G.1 | `OnJumpLegacy` / `AirMove` / `WalkMove` signatures re-pointed; `IsEntityPawn` 170, `IsEntityController` 171, `Teleport` 164, `ClientOffset` 616 | `assets/gamedata.jsonc` | **gap/low impact**: the port hooks only `ProcessUsercmds`/`SetupMove`; `WalkMove` already carries the SwiftlyS2 pattern from the same fix (PORT_PLAN §8d). The fork's `OnJumpLegacy`/`AirMove` patterns cannot be uniqueness-verified from this sandbox (`/srv` is unreadable), so our live-verified patterns stay; they are report-only in `cs2ac_hooks`. |

## v8 — kick for INVALID INPUT streams
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 8.1 | `cs2ac_stream_kick_{detections,reports,seconds}` (20/180 s), escalation report → kick via `cs2ac_kick_command` | `plugin.cpp`, `decisions.cpp`, `config.cpp`, cfg | done |
| 8.2 | Per-slot/per-detection report times cleared on (re)connect/disconnect/map reset | `plugin.cpp` | done |

## v8.1 — SILENTAIM < 0.25°, BACKTRACK bans
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 8.1.1 | Excess < 0.25° scores 0 (still telemetry) | `decisions.cpp` | done |
| 8.1.2 | BACKTRACK in the default ban list | `config.cpp` | done |
| 8.1.3 | BACKTRACK neighbours must also carry fractional render times | `decisions.cpp` | done |

## v9 — four kicks for cheats CS2AC missed
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 9.1 | MOUSE MISMATCH: <90% of ≥100 turns; kicks, never bans | `mod_integrity.cpp`, `decisions.cpp`, `config.cpp` | done |
| 9.2 | MULTIPLE SIGNALS: 3 kinds, BHOP/NULLS ignored | `plugin.cpp`, `decisions.cpp` | done |
| 9.3 | `cs2ac_kick_detections "TRIGGER PATTERN,MOUSE MISMATCH,ANTIAIM"` | `config.cpp` | done |
| 9.4 | TRIGGER PATTERN left the report-only list; NAMECHANGER joins the stream kick | `decisions.cpp`, `config.cpp` | done |

## v9.1 — kill telemetry through smoke and walls / HE-cleared smoke
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 9.1.1 | `silentaim_stats` counts all enemy kills (kills, hs, smoke, smoke_hs, smoke_he, wall, wall_hs, blind, noscope); suicides/teamkills excluded | `mod_shots.cpp` + `attackerblind` in `hooks.cpp` | done |
| 9.1.2 | HE clears a smoke: blast within 250 units in the last 3.0 s → no smoke bonus, `smoke_he` flag | `detection.cpp` (`SmokeClearTracker`), `mod_shots.cpp` | done |
| 9.1.3 | `smokeClearedByHe` on `ShotRecord`; `silentaim_hit` gets `smoke_he` | `core.hpp`, `mod_shots.cpp` | done |
| 9.1.4 | New events consumed: `hegrenade_detonate`, `smokegrenade_expired`, `round_start` | `hooks.cpp` | done |

## v10 — kicks and day bans where the evidence is reliable
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 10.1 | ANTIAIM in `cs2ac_kick_detections` | `config.cpp` | done |
| 10.2 | `cs2ac_unstable_kick_detections "ANTIAIM"`: network-vetoed listed findings kick | `plugin.cpp` | done |
| 10.3 | DESUBTICKING burst kick (3 / 60 s) | `plugin.cpp`, `config.cpp` | done |
| 10.4 | MULTIPLE SIGNALS needs 3 kinds; ignore BHOP/NULLS | `decisions.cpp` | done |
| 10.5 | MOUSE MISMATCH threshold 0.9 | `config.cpp` | done |
| 10.6 | Repeat ban (second kick within 24 h → 1440-min ban through admin_system only; `needs_other` for INVALID INPUT; records per SteamID across reconnects/maps; severity `tempban`) | `plugin.cpp`, `admin_bridge.cpp`, `decisions.cpp` | done |
| 10.7 | `CheckCommandTemplate` review changes (`weak_signal_policy` note) | `plugin.cpp` | done |

## v11 — DLL INJECTION bans + mass guard
| # | Behaviour | Port location | Status |
|---|---|---|---|
| 11.1 | DLL INJECTION in the default ban list (permanent ban) | `config.cpp` | done |
| 11.2 | Mass guard (5 distinct SteamIDs within 60 min → reports until reload + warning) | `plugin.cpp`, `decisions.cpp`, `config.cpp`, cfg | done |

## Cross-cutting: translations, cfg, version, tests
| # | Item | Status |
|---|---|---|
| T.1 | Fork HEAD `translations/*.txt` copied over the package (37 files) | done |
| T.2 | `assets/cs2ac.cfg` replaced with the fork cfg | done |
| T.3 | Version 1.1.0; manifest description names the fork v11 and the new modules | done |
| T.4 | 49 string/bool + 19 debug + 22 int/float convars registered with the fork's names/defaults/descriptions | done |
| T.5 | Unit tests: list/severity policy, stream/combo/mass-guard/repeat-ban windows, JSON escaping, tick render/interp/dip/fingerprint/buckets, SILENTAIM 0.25°/camera weights/buckets, trigger-pattern buckets/median, RCS correlation/qualification/share, usercmd tick-integrity wire fields; `cs2ac_tests` 936 checks, 0 failures | done |

## Known gaps / deliberate deviations

1. **Gamedata `OnJumpLegacy`/`AirMove` patterns** — cannot be uniqueness-verified against the live
   `libserver.so` from this sandbox; not adopted (G.1). Neither function is detoured by the port, so
   the only consequence is the readiness line in `cs2ac_hooks`.
2. **admin_bridge API** — our `admin_system` exports both `AddPlayerPunishment` (online) and
   `AddOfflinePlayerPunishment`, so the fork's bridge semantics (DB insert, offline flush) are fully
   available; no fallback-command gap. The manifest declares it `optional`.
3. **Detection log directory** — the fork writes `csgo/addons/cs2ac/logs`; the port writes
   `<plugify logs>/cs2ac/*.jsonl` (`plg::GetLogsDir()`), same file names and JSON schema. Writes are
   skipped outside a live map so no client query can run in the loading window.
4. **RCS schema fields** (`CCSPlayer_AimPunchServices`) are probed lazily after a map exists; if
   this server's schema lacks them the module disables itself exactly like the fork.
5. **Webhook report-only text** reuses the announcement sentence (`, but this finding is
   report-only.`) instead of the fork's separate `webhook.outcome.report_only` wording; the outcome
   value and the reports themselves are identical.
6. **Usercmd tick-integrity field numbers** (`render_tick_count=3`, `render_tick_fraction=4`,
   `sv_interp0=5`, `sv_interp1=6`, `cl_interp=7`, `player_tick_count=8`,
   `consumed_server_angle_changes=13`) come from the CS2 protobuf layout; the reader is tolerant and
   `cs2ac_usercmd_dump` now prints each field plus its "missing" marker, so they can be confirmed on
   a live server. A wrong number degrades that signal only (no crash).
7. **Auto-updater** remains unported (PORT_PLAN §6.1); `cs2ac_auto_update`/`cs2ac_update_repository`
   are registered and reported, and `AutomaticUpdatesEnabled()` now requires both like the fork.
8. **Kick/fallback commands are queued** and executed from `OnPluginUpdate`
   (`Plugin::ProcessServerCommands`), as the task requires: never inside the per-client hook that
   produced the finding. A command carrying `{userid}` is re-validated (slot still connected, same
   SteamID64) at execution time, so a reused slot cannot kick the next occupant; queued commands
   are dropped with a log line at a map boundary. Bans through `admin_system` are queued by
   `admin_bridge` exactly like the fork.
9. **Stream-kick / burst-kick / combo / repeat-ban state** uses `steady_clock` seconds instead of
   the fork's `steady_clock::time_point` deques so the window rules live in engine-free
   `decisions.cpp` and are unit-tested; the semantics (windows, thresholds, dedupe, ignore lists)
   are the fork's.
