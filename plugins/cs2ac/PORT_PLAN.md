# cs2ac → Plugify port plan

Source: `ref/src/cs2ac-legacy` (CS2AC 1.0.14, Metamod:Source, AGPL-3.0, ~15.4k LOC non-vendor C++ across
67 files). Target: `plugins/cs2ac` on s2sdk 2.18.2 + polyhook 2.5.6, C++23/g++-14, Linux only.
Production reference: `ref/prod-27016/addons/metamod/cs2ac.vdf` (alias `cs2ac`, file
`addons/cs2ac/bin/linuxsteamrt64/cs2ac`); the plugin ships no configs directory in production —
its `cs2ac.cfg` lives in `cs2ac/cfg/cs2ac.cfg` and its translations in `cs2ac/translations/<lang>.txt`
(both carried in the source tree), so those two trees are the ones to ship.

## 1. What the original is built on, and what replaces it

| Original (Metamod/MSSDK) | Plugify replacement |
|---|---|
| `ISmmPlugin` entry, `Load`/`AllPluginsLoaded`, `PLUGIN_EXPOSE` | `PLUGIN_PLUGIN`-style `plg::Plugin` entry (`OnPluginStart`/`OnPluginUpdate`/`OnPluginEnd`), never `Failed` |
| SourceHook `SH_ADD_HOOK(IGameEventManager2, FireEvent, …)` (pre+post) | `s2sdk::HookEvent(name, cb, HookMode::Pre/Post)` (one hook per event name) |
| SourceHook `SH_ADD_HOOK(ISource2Server, GameFrame, …)` | `s2sdk::OnGameFrame_Register` (`bool,bool,bool` = simulating, first, last) |
| SourceHook `ISource2GameClients::ClientActive/ClientFullyConnect/ClientSettingsChanged/ClientDisconnect` | `s2sdk` listeners `OnClientActive`, `OnClientFullyConnect`, `OnClientSettingsChanged`, `OnClientDisconnect`\* (\*`OnClientPutInServer` when the original's `ClientActive` semantics are needed) |
| SourceHook `ISource2GameClients::ClientSvcUserMessage` / `IGameEventSystem::PostEventAbstract` for `GE_FireBulletsId` | `s2sdk::OnSendNetMessage_Register` (`int32_t slot, void* UserMessage`) filtered by `UserMessageGetMessageName`, fields read with `s2sdk::PbRead*` |
| `SH_ADD_MANUALHOOK(Teleport, playerPawn, offset 162/163)` | `s2sdk` entity/teleport data + per-frame position sampling; the raw vtable hook is **not** recreated (see §6) |
| Raw `CDetour` on 13 `libserver.so` functions (usercmds + movement) via `cs2ac.games.txt` signatures | polyhook `HookDetour(functionAddress, ret, args…)` + `AddCallback(Pre/Post)`, function addresses resolved with `s2sdk::LoadGameConfigFile`/`GetGameConfigSignature` from this plugin's own `assets/gamedata.jsonc` |
| Schema fields via `schema::GetOffset(class, field)` (`utils/schema.cpp`, `modules::schemasystem` scan) | `s2sdk::GetSchemaOffset(class, field)` + `GetEntSchema2/…Float2/Vector3D2/Ent2` (class/field names identical — the original's `AddSchemaRequirements` list is reused verbatim as the startup validation list) |
| `ICvar` lookups (`ConVarRefAbstract`, `ConVar_Register`) | `s2sdk::GetConVarFloat/Bool/Int/String`, `FindConVar`, `CreateConVar*` (same cvar names) |
| `g_pEngine->GetClientConVarValue(slot, name)` for `sensitivity`/`m_yaw`/userinfo | `s2sdk::GetClientInfo(slot, name)`-style helpers where s2sdk exposes them; otherwise `s2sdk::QueryClientConVar` (the server's `IClientCvarValue` service is what s2sdk wraps) |
| `g_ClientCvarValue` — cs2ac's own `IClientCvarValue001` implementation (client cvar polling: `clientcvar/*`, hooks `CEngineServer::ProcessRespondCvarValue`) | **dropped entirely**; the same queries go through `s2sdk::QueryClientConVar(slot, name, callback, data)`. The exported `ClientCvarValue001` interface had **no consumer in the production set** (checked: only cs2ac files reference it), so no Plugify method is exported |
| `g_pFullFileSystem` + `addons/cs2ac/gamedata/cs2ac.games.txt` | `s2sdk::LoadGameConfigFile("cs2ac")` reading `configs/cs2ac/gamedata.jsonc` (s2sdk gamedata format) |
| `g_pFullFileSystem` + `KeyValues("Phrases")` translations at `addons/cs2ac/translations/<lang>.txt` | own KV1 reader over `configs/cs2ac/translations/<lang>.txt` (same files, same format: `"Phrases" { "key" "value" }`) |
| `interfaces::pEngine->ServerCommand("exec cs2ac.cfg")` + ConVars | cfg file `configs/cs2ac/cs2ac.cfg` parsed directly (same `name value` lines, `//` comments), values applied to the registered ConVars via `s2sdk::SetConVar*`; the trailing `cs2ac_config_loaded` marker keeps its original meaning |
| `interfaces::pEngine->ServerCommand(ban/kick command)` | `s2sdk::ServerCommand(command)` (identical `{steamid64}`/`{userid}`/`{detection}` expansion, identical `CCommand::MaxCommandLength` guard) |
| `INetChannelInfo` (`GetEngineLatency`, `GetAvgLoss/Choke`, `GetRemoteFramerate`) | `s2sdk::GetClientLatency`; loss/choke/jitter/framerate read from the client's net channel through schema offsets in `gamedata.jsonc` (`INetChannel` layout is verified at startup and the network-safety monitor degrades to "veto" — its safe direction — if unavailable) |
| `INavPhysicsInterface::TraceLine` (movement trace for Triggerbot) | `s2sdk::TraceLine`/`TraceShape` with the same start/end/mask/ignore-entity parameters |
| `steam_gameserver.h` HTTP (Webhook avatar lookup) | HTTP through the same pipe the rest of the set uses (`plugins/as_blockdb/src/steam_http.cpp`); webhook POST is a plain HTTPS request with no engine dependency |
| `UpdaterService` (GitHub auto-update, `miniz`/`picosha2`) | **not ported** (see §6): Plugify deploys plugins, self-updating a shared server plugin is out of scope |
| Windows codepaths, `dynlibutils`, `module_windows.cpp`, `plat_win.cpp`, `.vscode`/AMBuild tooling | dropped (Linux only) |

Rules honoured from the porting guide: no Metamod/SourceHook/hl2sdk/SchemaEntity headers — every
engine access is s2sdk (data) or polyhook (code hooks); all engine data reads use s2sdk schema
accessors with the original class/field names; signatures live in this plugin's gamedata.

## 2. Engine data each module needs

The whole detector family hangs off one data pipeline; the modules themselves are pure logic over
these structures, which is why they port almost verbatim:

* **`PlayerCommand` (usercmd)** — read from the `ProcessUsercmds` detour argument and parsed with a
  small protobuf wire reader (no protobuf dependency): `cmdNum` (`CUserCmdBase::cmdNum`), base
  `command_number`(=cmdNum), `client_tick`, `viewangles`, `buttons_pb{buttonstate1..3}`,
  `mousedx/mousedy`, `subtick_moves[]{button,pressed,when,analog_forward_delta,analog_left_delta,
  pitch_delta,yaw_delta}`, `input_history[]{view_angles}`, `attack1_start_history_index`.
  Field numbers are the CS2 wire values (`CSGOUserCmdPB.base=1`, `input_history=2`,
  `attack1_start_history_index=6`; `CBaseUserCmdPB.client_tick=2`, `buttons_pb=3`, `viewangles=4`,
  `mousedx=11`, `mousedy=12`, `subtick_moves=18`; `CSubtickMoveStep.button=1`, `pressed=2`,
  `when=3`, `analog_forward_delta=4`, `analog_left_delta=5`, `pitch_delta=6`, `yaw_delta=7`;
  `CSGOInputHistoryEntryPB.view_angles=2`). The reader reports which fields it actually saw, and a
  `cs2ac_usercmd_dump` diagnostic command logs the parsed view of live commands so the reader can be
  confirmed on a live server (see §6 "needs a live-server check").
* **`CMoveData`** (SetupMove/ProcessMovement/…) — same POD layout as `sdk/datatypes.h`
  (`m_vecViewAngles`, `m_vecVelocity`, `m_vecAbsOrigin`, `m_outWishVel`, `m_SubtickMoves`,
  `m_AttackSubtickMoves`, `m_flSurfaceFriction`, …); 312 bytes on Linux, asserted.
* **Player/pawn state** — `m_fFlags`, `m_hGroundEntity`, `m_MoveType`, `m_nActualMoveType`,
  `m_bOnGroundLastTick`, `m_bIsScoped`, `m_vecAbsVelocity`, `m_CBodyComponent→m_pSceneNode→m_vecAbsOrigin`,
  `v_angle`, `m_angEyeAngles`, `m_hController`, `m_pMovementServices{ m_bDucked, m_ModernJump,
  m_vecLadderNormal, m_nButtons, m_vecLastMovementImpulses, m_flSurfaceFriction }`, `m_steamID`,
  `m_iszPlayerName`, weapon services `m_hActiveWeapon` — all through `GetEntSchema*2`.
* **Events** — `weapon_fire`, `player_hurt`, `player_death`, `player_spawn`, `smokegrenade_detonate`
  through `HookEvent` (pre and post, exactly where the original hooked `FireEvent` before/after).
* **Net message** — `CMsgTEFireBullets` (`player`, `inaccuracy`, `spread`, `weapon_id`) through
  `OnSendNetMessage`; used only by Silentaim.
* **Client cvars** — `s2sdk::QueryClientConVar` for the `m_yaw`, `fps_max`, `sv_cheats`,
  `sensitivity`, `cl_showpos`, `cam_showangles`, `cl_drawhud`, `cl_pitchdown`, `cl_pitchup`,
  `cl_yawspeed`, `fov_cs_debug` integrity cycle (Invalid CVar), plus userinfo reads for
  `sensitivity`.
* **Server cvars** — `sv_cheats`, `mp_teammates_are_enemies`, `sv_air_max_wishspeed`,
  `sv_autobunnyhopping`, `sv_jump_spam_penalty_time`, `sv_legacy_jump`, `sv_bhop_time_window`.
* **Network quality** — sampled once per 100 ms per player for the five-second window used by
  Doubletap / Triggerbot / AntiAim (ping, jitter, in/out loss, in/out choke, command gaps).

## 3. Detection modules (all 18), data → hook equivalent

| # | Module | Original input | Port input (s2sdk/polyhook) |
|---|---|---|---|
| 1 | AIMBOT | `ProcessUsercmds` (base + attack history angles) + `SetupMove` (eye, airborne, scoped) + `weapon_fire`/`player_hurt` + per-tick position frames | identical; usercmds from the polyhook detour, positions sampled each `OnGameFrame` |
| 2 | AIMLOCK | `SetupMove` view angles + position frames + `GetEngineLatency`/`cl_interp_ratio` | identical; latency `s2sdk::GetClientLatency`, interpolation from `QueryClientConVar("cl_interp_ratio")` |
| 3 | ANTIAIM | `ProcessUsercmds` (angles, mouse, subtick deltas, history), `SetupMove`, `weapon_fire`, `player_spawn`, `m_yaw` | identical |
| 4 | AUTOSTRAFE | jump analysis (AirAccelerate/TryPlayerMove/OnJumpModern data) + yaw-accel optimizer over usercmds | usercmds for the optimizer; jump statistics from the movement detours (§4) |
| 5 | BHOP/HYPERSCROLL | landings (`CategorizePosition`/`RegisterLanding`), jump presses (subtick), frame-perfect detection via `m_outWishVel` change in OnJump\* | `CategorizePosition` + `OnJumpLegacy/Modern` detours recording `m_outWishVel` before/after, plus `AirMove` for air time |
| 6 | DLL INJECTION | `GetLegacyGameEventListener` signature + `IGameEventManager2::FindListener` for 117 blacklisted events | **degraded**: same gamedata signature, but `FindListener` is a vtable call whose index must be verified on the live binary — the index is a gamedata entry; without a verified index the module logs once and stays off (§6) |
| 7 | DESUBTICKING | usercmd subtick `when` distribution | identical |
| 8 | DOUBLETAP | two `weapon_fire` in the same/next tick per weapon | identical (events) |
| 9 | HYPERSCROLL | landing/jump-press pattern scoring | identical (§4 landings) |
| 10 | INHUMAN ACCURACY | shots + position frames + `player_hurt` | identical |
| 11 | INVALID CVAR | client cvar poll cycle + sv_cheats watcher | `s2sdk::QueryClientConVar` + `s2sdk::GetConVarBool("sv_cheats")` polled once per second (replaces the cvar change callback) |
| 12 | INVALID INPUT | usercmd buttons vs subtick moves (`VerifyCommand`) | identical |
| 13 | IRREGULAR BEHAVIOR | `weapon_fire`/`player_death` (airborne, noscope, distance, penetrated, headshot) | identical (events) |
| 14 | NAMECHANGER | `ClientSettingsChanged` + client name | `OnClientSettingsChanged` + `s2sdk::GetClientName` |
| 15 | NULLS | usercmd subtick button events + `m_vecLastMovementImpulses` + `GetRemoteFramerate` + air speed | identical; air speed from `CMoveData` post-velocity in the `AirMove`/`ProcessMovement` detours, remote framerate from the net-channel schema read |
| 16 | SILENTAIM | attack-history angles vs `CMsgTEFireBullets` inaccuracy/spread + hit/death outcomes | identical |
| 17 | SUBTICK SPAM | usercmd subtick button/timing aliases with angle deltas | identical |
| 18 | TRIGGERBOT | `SetupMove` crosshair trace + reaction ticks + `player_hurt` + smoke safety volumes | identical; trace through `s2sdk::TraceLine` |

Shared infrastructure ported as-is: `ShotCorrelator` (command↔shot correlation, position frames),
`NetworkSafetyMonitor`, `localization` (phrases + watermark), `settings` (ConVars, whitelist,
detection mask), announcement code (chat, center HTML, punishment command), `PlayerManager`.

## 4. Hook inventory — what is actually installed

Two polyhook detours are installed (`hooks.cpp`), each only when its gamedata signature resolves:

| Gamedata signature | Purpose | Failure mode |
|---|---|---|
| `ProcessUsercmds` | the usercmd stream every detector consumes (arg: `CSource2Server*this`, controller, commands, count, paused, margin) | `cs2ac_hooks`/log report it; all usercmd-based detections stay off |
| `SetupMove` | the authoritative per-tick command for Aimlock/Aimbot/Triggerbot/ShotCorrelator | those detections degrade, triggerbot/aimlock log once |

The other eleven signatures of cs2ac.games.txt are still resolved and reported by `cs2ac_hooks`,
but their data is **observed once per server frame** instead of hooked
(`DetectionSystem::MovementOnGameFrame`, fed from s2sdk/schema reads):

| Original hook | Port equivalent |
|---|---|
| `CategorizePosition` | ground-flag transitions sampled per frame (`PlayerOnGround` / `m_bOnGroundLastTick`) drive the same landing gate (`OnGroundFrame` → `CreateLandEvent`) |
| `AirMove` | air time accumulates once per frame while airborne and `MOVETYPE_WALK` (`OnAirMoveFrame`) |
| `ProcessMovement[Post]` | the "air-moved this frame" bookkeeping runs at the same point of the sampler |
| `OnJumpLegacy`/`OnJumpModern` (+`m_outWishVel` delta) | `engine::PlayerFramePerfectJump` reads `CCSPlayer_MovementServices::m_ModernJump` (`m_nLastUsableJumpPressTick`+frac vs `m_nLastLandedTick`+frac — the original's own modern-perf test) and `OnServerJump` |
| `PhysicsSimulate` | `s2sdk::GetGameTickCount()` / `OnGameFrame` |
| `Duck`, `LadderMove`, `WalkMove`, `TryPlayerMove`, `AirAccelerate` | the fields those hooks fed (ducking, ladder time, walk flag, collision-gain, AA calls) are read from the schema / the usercmd data at the same points; the parts that need the exact per-call CMoveData (air-accel call accounting for the jump statistics) are not carried over — see §6.4 |
| `GameEventManager`, `GetLegacyGameEventListener` | DLL Injection only; the listener pointer resolves, the `FindListener` vtable index does not (see §3 #6) |

Rationale for the split: the CMoveData / movement-services object layouts those hooks receive cannot
be verified against the live binary from this session, and a wrong layout misreads the engine's
structs (false detections) instead of failing. The frame sampler uses only named schema fields and
s2sdk getters, each of which fails soft (warn once, treat as absent). Every entry is visible in
`cs2ac_hooks` so a live session can confirm what resolved.

Gamedata offsets: only `ClientUserIdOffset` is read (`engine::RegisterGamedataOffset`); it is
deliberately absent from the shipped gamedata (§7.3). The original's remaining offsets
(GameEntitySystem, Teleport, IsEntityPawn, IsEntityController, ClientOffset,
ProcessRespondCvarValue, ClientSlotOffset) belong to the Metamod engine/schema layer and are not
carried over — s2sdk answers the same questions.

## 5. Config, files, commands

* `configs/cs2ac/cs2ac.cfg` — the original file byte-for-byte (`cs2ac_enabled`, 18 per-detector
  switches, 17 debug switches, announcements, language, punishment/kick commands, webhook settings,
  `cs2ac_allow_sv_cheats_testing`, trailing `cs2ac_config_loaded`).
* `configs/cs2ac/translations/<lang>.txt` — all 38 original files.
* `configs/cs2ac/gamedata.jsonc` — this plugin's signatures/offsets (s2sdk format, see §4).
* Commands (identical names and output): `cs2ac_status`, `cs2ac_help`, `cs2ac_reload`,
  `cs2ac_check_config`, `cs2ac_test_announcement`, `cs2ac_webhook_test`, `cs2ac_config_loaded`,
  plus `cs2ac_usercmd_dump` (new, diagnostic only) and `cs2ac_hooks` (new, prints the hook/gamedata
  readiness report).
* ConVars: all 40 original names with identical defaults/descriptions.

## 6. Deliberately not ported (explicit)

1. **UpdaterService** (`updater.cpp`, 721 LOC + `miniz`/`picosha2`): GitHub auto-update, backup and
   next-restart replacement of the plugin binary. Plugify owns deployment; a shared server plugin
   overwriting its own binary is out of scope. `cs2ac_auto_update` stays registered and only
   reports "managed by the deployment" in `cs2ac_status`.
2. **IClientCvarValue001 export** (`clientcvar/*`, ~830 LOC including `dynlibutils`): s2sdk already
   implements the client-cvar query service; the exported interface had no consumer in the
   production plugin set, so nothing is exported.
3. **Custom `RegisterLanding`/`RegisterTakeoff` movement-event bookkeeping** (`mv_player.cpp`):
   replaced by direct reads of the same schema fields at the same hook points; the ladder/teleport
   helper semantics (`JustTeleported(2.0f)` grace windows) are preserved with timestamps recorded
   from the `Teleport`-equivalent data (position discontinuity detection in the frame sampler).
4. **Jump analysis full statistics** (`jump_analysis.cpp`, 627 LOC: air-accel call accounting,
   distbug fixes, invalid-gain detection): only the parts the ported detectors consume are ported —
   airtime, strafe count/duration, gain efficiency, jump type used by Autostrafe. The unused
   classification helpers are not carried over.
5. **Windows support, `plat_win.cpp`, `module_windows.cpp`, `dynlibutils`, AMBuild/`.devcontainer`,
   `.clang-format` cosmetics.**
6. **Panorama/HTML chat niceties** are unchanged: chat uses s2sdk's colored chat (the same
   `{red}/{default}` → control-byte conversion), center text uses `show_survival_respawn_status`
   (s2sdk `PrintCentreHtml`), exactly like the original.

### 6b. Deviations that had to be made (each one deliberate and visible)

| # | Original behaviour | Port behaviour | Why |
|---|---|---|---|
| 1 | Nulls reset all evidence when `INetChannelInfo::GetRemoteFramerate` was unavailable | events carry an unknown frame rate (-1); the requirement falls back to the original's own unknown-rate value (640) | s2sdk does not expose the net channel; the strict reset would disable Nulls entirely, the fallback is the most conservative setting the original itself had |
| 2 | Network safety vetoed when loss/choke could not be read | ping and jitter still veto (and the sample-availability gate); loss/choke are treated as unknown | same reason; vetoing every detection would disable Doubletap/Triggerbot/AntiAim-mismatch outright, while ping+jitter are the gates that actually move with a bad connection |
| 3 | Discord reports included the server's public IP:port from `ISteamGameServer::GetPublicIP` | `cs2ac_webhook_server_address` is used when set, otherwise the field is omitted (logged once) | the accessor's ABI could not be verified from this session |
| 4 | `{userid}` came from `IEngineServer::GetPlayerUserId` | taken from the optional `ClientUserIdOffset` gamedata entry; absent → the punishment reports IdentityUnavailable (never a guessed id) | wrong id in a kick command would punish the wrong player; `cs2ac_check_config` reports the missing offset |
| 5 | Jump statistics counted air-acceleration calls (per-call CMoveData data) | airtime/strafe/gain come from the frame sampler and the usercmd stream | those need the movement-service object layout (see §4); the thresholds are unchanged |

## 7. What the unit tests cover / what needs a live-server check

`tests/cs2ac_tests.cpp` (run by `./test.sh`) covers everything that can be checked without an
engine: the angle math (`NormalizeDeg`, `GetAngleDifference`, `AimForward`, `AngularDistance`,
`YawDelta`), the weapon tables and opponent rule, every compile-time rule the original asserted
(`ScoreReactions`, `MeetsSmoothThresholds`, `ShouldVeto`, `MeetsCoverage`) as runtime checks, the
Nulls scaling (unknown/64/512 FPS, `fps_max` clamp, underlap forgiveness), the cs2ac.cfg parser
(comments, quotes, the `cs2ac_config_loaded` marker, the shipped production file), the settings
state machine (mask, whitelist parsing/rejects/duplicates), `CheckCommandTemplate`, the phrase
loader plus all 38 shipped language files and the watermark, and the usercmd protobuf reader
(real wire bytes for a full command, unknown-field skipping, truncated/padded/zero buffers).

## 7b. Needs a live-server check (cannot be validated in this session)

1. **Protobuf field numbers of usercmd messages** — the reader is tolerant and self-reports; run
   `cs2ac_usercmd_dump` on a live server and confirm `viewangles`/`subtick_moves`/`input_history`
   are non-empty for a moving player.
2. **Movement signatures in `gamedata.jsonc`** — must be re-verified against the current
   `libserver.so` (the shipped patterns are cs2ac 1.0.14's linux patterns). `cs2ac_hooks` prints
   which resolved.
3. **`IGameEventManager2::FindListener` vtable index** (DLL Injection) and the net-channel offsets
   used for loss/choke/framerate.
4. **Ban/kick command templates** (`css_addban`/`css_kick`) — depend on the installed admin plugin.

## 8. Crash-fix round (post-port audit against the live server crash loop)

| # | Finding (file) | Class | Fix |
|---|---|---|---|
| 1 | both polyhook detours declared an extra leading `this` slot, so every captured argument was one register off (`hooks.cpp`, the `kProcessUsercmds*` / `kSetupMove*` indices and the `HookDetour` argument vectors) | crash | For `SetupMove` the shift handed a `PlayerCommand*` to `s2sdk::PlayerServicesToPlayerSlot()`, which dereferences it (`service->GetPawn()`) — a wild call inside the engine's movement processing. The lists now mirror the original's own detour signatures (first real parameter in RDI, `controller`/`services` at index 0); verified against s2sdk's own `HookDetour` handlers and the live-tested `plugins/RestrictedWeapons` hook (same convention). |
| 2 | `s2sdk::FindConVar` returns the cvar system's invalid handle (0xFFFFFFFF, seen in the live log as `Invalid ConVar handle: 4294967295`) for a name it cannot resolve, and every guard tested only `!handle` (`engine.cpp` cvar helpers) | spam (heavy) | All cvar access goes through a resolving cache: `cvar::IsValidHandle` (core.cpp, unit-tested) rejects 0, 0xFFFFFFFF and its sign-extended spellings; a miss is logged once per name and retried every 60 s; `ServerCvarExists` no longer reports missing cvars as present and `SetCvarValue` no longer "succeeds" silently. Registration failures are counted and reported. |
| 3 | the Invalid-CVar reply handler evaluated the reply, fed the latches and could send a punishment *inside* s2sdk's cvar dispatch (`engine.cpp` `QueryClientCvar`) | hang/re-entrancy | Replies are queued (bounded, 64, drops counted) and drained from the frame pump; no s2sdk call happens inside the cvar callback. |
| 4 | the centre message was re-sent on every frame (`announce.cpp`) instead of the original's 100 ms timer | spam (network) | Throttled to 100 ms; the writes are additionally gated on a live map (map start/shutdown no longer post usermessages). |
| 5 | the usercmd array read used the assumed stride over the whole remaining array (`hooks.cpp` `ParseCommands`) | crash-risk | Read bounded to the object's own slot, plus plausibility gates (`usercmd::IsPlausibleCommandNumber`, `IsStrictlyIncreasing` — unit-tested) that drop a batch with out-of-range or non-increasing command numbers instead of feeding it to the detectors. |
| 6 | detours were left installed on plugin end; the pending Steam HTTP call result was not unregistered; console commands stayed registered (`hooks.cpp`, `webhook.cpp`, `plugin.cpp`) | crash on unload | `polyhook::UnhookDetour` per hook, `SteamAPI_UnregisterCallResult` before the webhook object dies, `s2sdk::RemoveCommand` for the 9 commands. |
| 7 | `NormalizeDeg` used subtract-in-a-loop (`core.cpp`) | hang | `fmod` (O(1)); non-finite input is returned unchanged. Unit-tested with 1e30 and ±inf. |
| 8 | two full 64-slot frame passes, per-call `sv_cheats`/`mp_teammates_are_enemies` lookups, 13 event-field reads per event (`plugin.cpp`, `engine.cpp`, `hooks.cpp`) | cost | One tracked-player pass for teleport sampling + movement sampler; the two gate cvars are read once per frame; each event type reads only the fields its detectors use. |
| 9 | `OnClientActive` queried the SteamID of a client that may not be authenticated yet (`hooks.cpp`) | cost/robustness | Dropped the query (nothing consumed it). |

### 8b. Map-shutdown crash (live, 4 dumps: `cs2ac_160404/160410/160756/160804`)

Stack (all four identically): `s2sdk MapEnd listener` → `hooks::OnMapEndListener` → `Plugin::OnMapShutdown`
→ `ResetRuntime` → `DetectionSystem::Reset` → `NameChangerModule::Reset` → `NameChangerModule::OnClientReady`
→ `detection::IsEligibleHuman` → `s2sdk Player::IsFakeClient` → `utils::GetClientBySlot` → SIGSEGV at 0.

Cause: the client list is already emptied at map shutdown, so *any* client query there dereferences a
null entry. The original's `NameChangerModule::Reset` re-baselined every player by asking the engine
for the current name — harmless in the Metamod original (whose client list was still alive), fatal here.

Fixes (two independent layers, so the invariant does not depend on listener ordering):
1. `hooks.cpp` `OnMapEndListener` now calls `engine::OnMapEnd()` **before** `Plugin::OnMapShutdown()`
   (and `OnMapStart()` before `OnMapInit()`), so the engine gate is closed for the whole shutdown path;
   `engine::PlayerFakeClient`/`PlayerCSTV` return "true" while the gate is closed, which makes every
   `IsEligibleHuman` check fail *before* any s2sdk call. `engine::OnMapEnd` also drops the sampled
   teleport state now (stale origins would have marked everyone as teleported on the next map).
2. `NameChangerModule::Reset` is state-only: no client loop, no `OnClientReady`. The name baseline is
   re-established lazily from the client events that already existed (`OnClientReady` on join/activate
   and the `!initialized` branch of `OnClientSettingsChanged`), which is the required "per-slot
   re-initialisation happens from a client event with `IsClientInGame`, never from the map-end path".
   Also removed the `GetClientName` call from `Plugin::OnClientDisconnect` (the disconnect hook can run
   for a slot whose client is already gone; the log line now names the slot).

Audit of every other reset path (all now state-only): `TriggerbotModule::Reset`, `DoubletapModule::Reset`,
`SilentAimModule::Reset`, `AimbotModule::Reset`, `AimlockModule::Reset`, `AntiAimModule::Reset`,
`IrregularBehaviorModule::Reset`, `InhumanAccuracyModule::Reset`, `DllInjectionModule::Reset`,
`NetworkSafetyMonitor::Reset`, `ShotCorrelator::Reset`, `MovementDetectionService::Reset` /
`ResetTransientDetectionState`, and each module's `Unload()`/`OnClientDisconnect()` — verified to contain
no `engine::`/`s2sdk::`/`PlayerRef` accessor calls. `Plugin::ResetRuntime` additionally only sends the
centre-screen clear when `engine::GameReady()` (which is false in this window). Every client-querying
`engine::` getter is gated on `g_gameReady`; there is no direct s2sdk client call outside `engine.cpp`
(only the entity-pointer helpers in `hooks.cpp`, which do not touch the client list).

No entity-handle/index misuse exists in this plugin: it never calls the s2sdk entity functions that take
handles with an index (the only handle traffic is `PawnHandle`/`GetEntData*` on pawn handles,
`PlayerSlotToEntHandle` for the trace ignore list and `TraceLine`'s handle → player conversion, all
with the `>= 0` invalid checks - see 8c for the reader-layer rules). No timers, no
`FindEntityByClassname` loops, no per-frame O(entities) work; every loop is bounded by its own
deque/byte budget.

### 8c. Audit follow-up: schema accessors and polyhook teardown (`doc_audit.md` 1.3/1.4/1.5/1.7)

**1. `m_pMovementServices` was read with a CHandle accessor (movement detectors silently dead).**
`engine.cpp` read the *pointer* field through `GetEntSchemaEnt2`, which s2sdk only serves for `m_h*`
fields: it printed "is not a entity handle" and returned the invalid sentinel, `MovementServices()`
returned nullptr and every jump/BHOP/null-subtick gate that needs it did nothing (no symptom besides
console spam, one warning per player per frame).

The first fix attempt (`GetEntDataPtr(handle, offset)`) was itself wrong: that function returns the
**address of the field**, not the value it stores ("a pointer to the data at the given memory
location" - `ref/s2sdk-src/src/export/schema.cpp:418`). The final reader (`engine::ReadPointerField`)
resolves the offset once and reads the pointer-width *value* with `GetEntData(handle, offset,
sizeof(void*))` - handle-based, offset-only, and it never dereferences the cached `CSchemaType*`
that crash signature D (`tools/mdump/REPORT.txt`) pins as the -1 sentinel. Verified against the live
schema dump shipped with the server (`backup-27035-*/addons/swiftlys2/gamedata/cs2/sdk.json`):
`m_pMovementServices` is `kind: ptr`, size 8; `m_bDucked`/`m_bOnGroundLastTick`/`m_bIsScoped` are
1-byte bools; `m_nActualMoveType` (MoveType_t) is 1 byte; `v_angle` and `m_vecLastMovementImpulses`
are 12 bytes; `m_ModernJump` is an embedded 56-byte `CCSPlayerModernJump` whose four tick/frac
fields are 4 bytes each - every width the readers use matches.

Observability: `engine::SchemaStatistics()` counts movement-services resolutions/misses, offset
misses and jump-data reads; `cs2ac_hooks` prints them and warns when the services pointer never
resolved. The pure part of the frame-perfect-jump comparison moved to
`movement::IsFramePerfectJump` (`core.cpp`) and is unit tested (7 checks), so the semantics cannot
regress unnoticed.

**2. Entities are read through handles, components through pointers.** All `ReadNumber`/`ReadVector`
call sites now pass a valid entity handle (`PawnHandle`, derived from the pawn pointer s2sdk already
validated) or `-1` with a *component* pointer (movement services) that was resolved from the owning
pawn's handle in the same tick; the pointer-"2" variants are reachable only for those components.
Reads are additionally refused while `g_gameReady` is false (map load), so the shared s2sdk schema
table can never be built in the loading window (signature C is exactly that crash).

**3. polyhook teardown** (`hooks.cpp:717-749`): each detour drops *our* callback first
(`RemoveCallback(hook, Pre, handler)`), then checks `AreCallbacksRegistered(hook)` and only calls
`UnhookDetour(TARGET address)` when no callback is left; the address stored at hook time
(`HookState::address`, `hooks.cpp:507`) is the hook key, never `GetOriginalAddr`. When another
plugin still owns a callback the detour stays installed and `state.hooked` is cleared.

### 8d. Live-log functional fixes: gamedata entries and ConVar registration (sweep round)

**1. `WalkMove` signature no longer matched the live build.** The live server logged
`Could not get signature: WalkMove - Pattern not found: 48 B8 ... 4C 8D B5 ... 41 55 41 BD` twice per
boot: the CS2AC 1.0.14 linux pattern (and the win64 one) describe the old prologue. Replaced with the
SwiftlyS2 pattern for the same build
(`48 B8 00 00 00 00 00 00 80 3F 55 66 0F EF C0 48 89 E5 41 57 41 56 4C 8D BD ? ? ? ? 41 55 49 89 F5`),
verified here: exactly one match in the live `libserver.so` (file offset 22900848 = VA 0x15D8070),
where the disassembly is the WalkMove prologue (`movabs rax, 0x3F80000000000000`; push rbp; pxor
xmm0,xmm0; lea -0x218(%rbp),%r15; mov %rsi,%r13), and the sibling entries of the same source resolve
in the same binary exactly once (`CCSPlayer_MovementServices::FullWalkMove` at 22907056, i.e. 0x1840
bytes later - the two functions are adjacent, as in the game - and `::Friction` at 22599616). All 15
other cs2ac patterns were re-checked against the same binary: exactly one match each, which also
confirms the copy is the build the live server runs (the live log only ever complained about
WalkMove). No ported detector depends on it: the port resolves it for the `cs2ac_hooks` report and
samples the movement data per frame (see the header comment of hooks.cpp / §4).

**2. `ClientUserIdOffset` was missing, so `{userid}` never resolved.** The original asked
`IVEngineServer2::GetPlayerUserId`; s2sdk exposes no such call, and the port's first version asked
the gamedata for a name that was deliberately absent, which logged `Could not get offset:
ClientUserIdOffset` twice per load. Now:
* the entry exists (`168`, documented in `assets/gamedata.jsonc`) - the `CServerSideClientBase::
  m_UserID` offset for this build (SwiftlyS2's offsets for the same version, cross-checked against
  the Metamod header annotations in `ref/pisex/SchemaEntity/serversideclient.h`);
* because a wrong id would substitute the WRONG PLAYER into a kick command, the offset is never
  trusted blindly: `engine::RememberClientUserId` records the engine userid every consumed game
  event reports for a slot (`hooks.cpp` ReadEvent - the events always carry the engine's own userid,
  which is exactly what GetPlayerUserId returned), and `engine::PlayerUserId` uses the offset only
  after it agreed with such an event userid; a disagreement is logged once and rejects the offset
  (`engine::ClientUserIdStatus`, printed by `cs2ac_hooks` and `cs2ac_check_config`). Until a userid is
  known the punishment path reports IdentityUnavailable - the same refusal the original used, never a
  guessed id. `engine::PlausibleUserId` (unit tested) bounds both sources.

**3. All 47 ConVars failed to register - the live server's config never reached the detectors.** The
live log: `Only 0 of 47 cs2ac convars could be registered` (both boots) plus `Failed to find
'cs2ac_*' convar` for every setting, so every detector ran on built-in defaults. Two root causes:
* registration ran before `hooks::Initialize()`, the only place that sets the engine gate the old
  code checked (`g_bindingsAvailable`) - so every `RegisterConVar` returned false without ever
  calling `CreateConVar`;
* the old code created *string* cvars for everything (s2sdk's `CreateConVarString`, which no other
  plugin in this tree uses), while the original declared `CConVar<bool>` for the toggles and s2sdk's
  typed readers (`GetConVarBool`) reject a cvar whose engine type is not bool ("Invalid cvar type
  ...") - even a successfully created `cs2ac_enabled` string cvar would have read back as false.
Fixes: `Plugin::OnStart` now runs `hooks::Initialize()` first and then registers, with the types and
the descriptions matching the original (`engine::RegisterConVar` = bool via `CreateConVarBool`,
`engine::RegisterStringConVar` = string via `CreateConVarString`; the 16 `_debug` switches carry the
descriptions their detection modules declare in ref/src; `cs2ac_webhook_url` keeps
FCVAR_PROTECTED); the registration path checks the specific s2sdk bindings instead of the gate, so it
no longer depends on lifecycle order. Reads go through the returned handles (the cache in
`engine.cpp`), and the cfg loader (`configs/cs2ac/cs2ac.cfg`, byte-identical to the original's
`cfg/cs2ac.cfg`) applies every value through them. Unit tests (`TestConVarTables`) pin the 31 + 16
names, their types, their bool defaults and their uniqueness.
`plg reload` support: s2sdk 2.18.2 has no binding to remove a ConVar, so a reload finds the cvars
already in the engine; `engine::CreateTypedConVar` accepts the existing handle (falling back to
`FindConVar` when s2sdk's "already exists" path returns an unusable value) and re-applies the cfg
instead of silently keeping defaults. If an existing cvar has the wrong engine type (the old build
may have left string-typed `cs2ac_*` cvars behind), it is detected once and read through the
type-agnostic path, with a warning telling the operator that a restart gives the proper type.

## 9. Non-negotiables carried over

* No exceptions escape into s2sdk/polyhook callbacks (every callback body is guarded).
* No blocking DB/network work on the game thread; the webhook queue is pumped from `OnPluginUpdate`.
* Never `Failed` from `OnPluginStart`; missing bindings/signatures → log + degrade.
* All thresholds, windows and weights are byte-compatible with the original (static_asserts from
  the original are kept as unit tests, see `tests/`).
