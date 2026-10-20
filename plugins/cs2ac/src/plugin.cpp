// cs2ac — the plugin entry point and the lifecycle/dispatch bodies that used to live in cs2ac.cpp
// (CS2ACPlugin::Load/Activate/OnGameFrame/HandleDetection/…).
//
// Rules this file follows (the porting guide's list):
//   * nothing queries a client before a map exists - engine::GameReady() gates every read and the
//     per-frame work only runs from OnPluginUpdate / OnGameFrame;
//   * the plugin never returns Failed from OnPluginStart (that would stop every Plugify plugin);
//     missing bindings, gamedata or hooks only disable the features that need them;
//   * no exception may escape into s2sdk/polyhook callbacks (every entry point is guarded);
//   * the game thread is never blocked: the webhook only enqueues and the config is read from disk
//     once at startup.

#include "cs2ac.hpp"
#include "admin_bridge.h"
#include "detection_log.h"

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cs2ac {

Plugin g_plugin;

namespace {

constexpr auto configLoadTimeout = std::chrono::seconds(5);

i64 NowSeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void Plugin::OnStart()
{
	try {
		SetConfigDirectory(std::string(plg::GetConfigsDir ? plg::GetConfigsDir().c_str() : "configs") + "/cs2ac");
		settings::Reset();
		// Console changes to the debug ConVars are seen immediately (the original read the CVars).
		settings::SetDebugResolver([](const char* name) {
			if (name && engine::ServerCvarExists(name)) {
				return engine::ServerCvarBool(name, settings::DebugFlagValue(name));
			}
			return settings::DebugFlagValue(name);
		});
		localization::SetWarningSink([](const std::string& message) { LogWarning("%s", message.c_str()); });
		localization::Reload(settings::GetLanguage(), ConfigDirectory() + "/translations");

		// The gamedata (and the schema/cvar plumbing) is resolved first: registration below uses the
		// s2sdk cvar bindings, and the original's ConVars are also what the cfg loader applies its
		// values through. Registering before this made s2sdk log
		// "Only 0 of 47 cs2ac convars could be registered" - the engine gate had not been opened yet.
		webhook = new WebhookService();

		hooks::Initialize();

		// The original registered its ConVars through ConVar_Register; the same 31 + 16 names are
		// registered here so console access (`cs2ac_aimbot_debug 1`, `cs2ac_enabled 0`) behaves the
		// same way, and the cfg loader below applies the file's values through them. The types match
		// the original's CConVar declarations (bool for the toggles, string for the rest).
		std::size_t registered = 0;
		for (const auto& spec : settings::kConVars) {
			// The webhook URL carries FCVAR_PROTECTED in the original (it must not be echoed by the
			// console or rcon); everything else is a plain server convar.
			const bool ok = spec.isString
								? engine::RegisterStringConVar(spec.name, spec.defaultValue, spec.description,
															   EqualsIgnoreCase(spec.name, "cs2ac_webhook_url"))
								: engine::RegisterConVar(spec.name, spec.defaultValue, spec.description);
			registered += ok ? 1 : 0;
		}
		for (const auto& spec : settings::kDebugConVars) {
			registered += engine::RegisterConVar(spec.name, "0", spec.description) ? 1 : 0;
		}
		for (const auto& spec : settings::kNumericConVars) {
			registered += spec.isFloat ? engine::RegisterFloatConVar(spec.name, std::strtof(spec.defaultValue, nullptr), spec.description)
									   : engine::RegisterIntConVar(spec.name, std::atoi(spec.defaultValue), spec.description)
							  ? 1
							  : 0;
		}
		convarsRegistered = true;
		const std::size_t convarTotal = settings::kConVarCount + settings::kDebugConVarCount + settings::kNumericConVarCount;
		if (registered != convarTotal) {
			LogWarning("Only %zu of %zu cs2ac convars could be registered; the missing ones keep their built-in defaults (run cs2ac_check_config).",
					   registered, convarTotal);
		}

		detectionSystem.Load([](const char* detection, PlayerRef* player, const localization::Text& evidence) {
			g_plugin.HandleDetection(detection, player, evidence);
		}, [](const char* detection, PlayerRef* player, const localization::Text& evidence) {
			g_plugin.HandleDetection(detection, player, evidence, false, true);
		});

		RegisterCommands();

		// The original exec'd cs2ac.cfg through the engine and waited for the trailing marker.
		configReloadPending = true;
		configLoadFailed = false;
		configLoadDeadline = std::chrono::steady_clock::now() + configLoadTimeout;
		LoadConfigFile();

		loaded = true;
		detection_log::Init();
		Log("CS2AC %s loaded successfully. Waiting for cs2ac.cfg to finish.", CS2AC_VERSION);
	} catch (const std::exception& error) {
		loaded = false;
		activationError = error.what();
		LogError("CS2AC could not start: %s", error.what());
	} catch (...) {
		loaded = false;
		activationError = "unknown error";
		LogError("CS2AC could not start (unknown error).");
	}
}

void Plugin::OnEnd()
{
	try {
		CleanupRuntime();
	} catch (...) {
	}
}

void Plugin::Activate()
{
	// Nothing deferred in the port: hooks and the detection system are ready in OnStart, so the
	// original's Activate()/activationPending dance collapses into OnStart.
}

namespace {
// The original's CON_COMMANDs, registered on the server console like every other Plugify plugin
// command. The table is shared with the removal path on plugin end - a command whose
// callback points into an unloaded module would crash the server on use.
struct CommandSpec {
	const char* name;
	const char* description;
};
constexpr CommandSpec kCommands[] = {
	{"cs2ac_status", "Show the current CS2AC status"},
	{"cs2ac_help", "Show the available CS2AC administrator commands"},
	{"cs2ac_reload", "Reload and validate cs2ac.cfg"},
	{"cs2ac_check_config", "Check the current CS2AC settings without changing them"},
	{"cs2ac_test_announcement", "Show a harmless CS2AC chat and center-screen test"},
	{"cs2ac_webhook_test", "Send a harmless Discord webhook test"},
	{"cs2ac_config_loaded", "Confirm that cs2ac.cfg finished loading"},
	{"cs2ac_hooks", "Show the cs2ac gamedata/hook readiness report"},
	{"cs2ac_usercmd_dump", "Dump the last parsed usercmd fields (diagnostic)"},
};
} // namespace

void Plugin::RegisterCommands()
{
	if (!__s2sdk_AddConsoleCommand) {
		LogWarning("the s2sdk command bindings are unavailable; the cs2ac_* commands are not registered.");
		return;
	}
	for (const auto& spec : kCommands) {
		// AddConsoleCommand attaches the listener even when the valve-command creation is refused
		// because the name already exists (s2sdk cannot destroy a console command, so our own
		// entry from a previous `plg reload` keeps its name); the command keeps working through
		// the listener, so a false return is a reuse - UnregisterCommands removes it either way.
		if (!s2sdk::AddConsoleCommand(spec.name, spec.description, s2sdk::ConVarFlag::None, &Plugin::OnCommand, s2sdk::HookMode::Pre)) {
			Log("the command '%s' already exists; reusing the entry (the listener is attached).", spec.name);
		}
	}
}

void Plugin::UnregisterCommands()
{
	if (!__s2sdk_RemoveCommand) {
		return;
	}
	for (const auto& spec : kCommands) {
		try {
			s2sdk::RemoveCommand(spec.name, &Plugin::OnCommand);
		} catch (...) {
		}
	}
}

s2sdk::ResultType Plugin::OnCommand(int32_t playerSlot, s2sdk::ConCommandContext context, const plg::vector<plg::string>& args)
{
	try {
		(void)context;
		// The original's CON_COMMANDs live in the server's command system: only the server console
		// and RCON (caller -1) could run them. s2sdk also feeds the chat triggers (! and /) of a
		// connected player into the same callbacks, so anything the game carries a player slot for
		// is refused before it can reach the administrator commands.
		if (playerSlot >= 0) {
			return s2sdk::ResultType::Handled;
		}
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view name(args[0].c_str());
		if (name == "cs2ac_status") {
			g_plugin.PrintStatus();
		} else if (name == "cs2ac_help") {
			g_plugin.PrintHelp();
		} else if (name == "cs2ac_reload") {
			g_plugin.ReloadConfig();
		} else if (name == "cs2ac_check_config") {
			g_plugin.CheckConfig();
		} else if (name == "cs2ac_test_announcement") {
			g_plugin.TestAnnouncement();
		} else if (name == "cs2ac_webhook_test") {
			g_plugin.TestWebhook();
		} else if (name == "cs2ac_config_loaded") {
			g_plugin.OnConfigLoaded();
		} else if (name == "cs2ac_hooks") {
			hooks::PrintReport();
		} else if (name == "cs2ac_usercmd_dump") {
			g_plugin.PrintUsercmdDump();
		}
	} catch (...) {
		// No exception may escape into the command dispatcher.
	}
	return s2sdk::ResultType::Handled;
}

void Plugin::OnMapInit()
{
	if (loaded) {
		ResetRuntime();
		Log("A new map is ready. Detection evidence started fresh.");
	}
}

void Plugin::OnMapShutdown()
{
	if (loaded) {
		ResetRuntime();
		Log("The map ended. Detection evidence was cleared.");
	}
}

void Plugin::ResetRuntime()
{
	// Never drop a pending ban: they are written as offline bans (DB only) at the map boundary.
	admin_bridge::FlushOffline();
	// A queued kick belongs to the map it was issued on; the next map's slots are different.
	if (!pendingServerCommands.empty()) {
		LogWarning("%zu queued punishment command(s) were dropped at the map boundary.", pendingServerCommands.size());
		pendingServerCommands.clear();
	}
	if (configReloadPending) {
		configReloadPending = false;
		configLoadFailed = true;
		ConsoleMessage("The configuration load was interrupted by a map reset. Run cs2ac_reload after the new map is ready.");
	}
	simulatingPhysics = false;
	announce::ResetDetectionAnnouncement();
	detectionSystem.Reset();
	punishmentLevels.fill(PunishmentLevel::None);
	for (auto& reports : streamReports) {
		reports.clear();
	}
	for (auto& kinds : reportedKinds) {
		kinds.clear();
	}
	// Repeat-ban records outlive the map on purpose; only expired ones go.
	PruneRepeatRecords(false);
	joinWatermarks.fill({});
	lastCvarCycle = {};
}

void Plugin::CleanupRuntime()
{
	admin_bridge::FlushOffline();
	loaded = false;
	UnregisterCommands();
	// The cs2ac_* ConVars are deliberately NOT removed: s2sdk 2.18.2 has no binding for removing a
	// convar (its ConVarManager::RemoveConVar is not exported), and the engine cvar system cannot
	// destroy one either. The next load therefore re-uses them (engine::CreateTypedConVar accepts
	// the existing handle) and re-applies cs2ac.cfg, which is what makes `plg reload` work.
	if (webhook) {
		webhook->Unload();
		delete webhook;
		webhook = nullptr;
	}
	detectionSystem.Unload();
	announce::ResetDetectionAnnouncement();
	hooks::Shutdown();
	localization::Shutdown();
	punishmentLevels.fill(PunishmentLevel::None);
	for (auto& reports : streamReports) {
		reports.clear();
	}
	for (auto& kinds : reportedKinds) {
		kinds.clear();
	}
	PruneRepeatRecords(true);
	massGuardHits.clear();
	massGuardTripped.clear();
	if (!pendingServerCommands.empty()) {
		LogWarning("%zu queued punishment command(s) were dropped because the plugin is unloading.", pendingServerCommands.size());
		pendingServerCommands.clear();
	}
	admin_bridge::Reset();
	detection_log::Shutdown();
	joinWatermarks.fill({});
	simulatingPhysics = false;
}

// ---------------------------------------------------------------------------------------------
// Dispatch (the original's OnProcessUsercmds/OnSetupMove/OnGameFrame/OnGameEvent/OnFireBullets)
// ---------------------------------------------------------------------------------------------

void Plugin::OnProcessUsercmds(int playerIndex, const std::vector<UsercmdView>& commands)
{
	if (!loaded || !settings::IsPluginEnabled() || playerIndex < 1 || playerIndex > MAXPLAYERS) {
		return;
	}
	auto* player = g_pPlayerManager->ToPlayer(playerIndex);
	if (!player) {
		return;
	}
	detectionSystem.OnProcessUsercmds(player, commands);
}

void Plugin::OnSetupMove(int playerIndex, const UsercmdView& command)
{
	if (!loaded || !settings::IsPluginEnabled() || playerIndex < 1 || playerIndex > MAXPLAYERS) {
		return;
	}
	auto* player = g_pPlayerManager->ToPlayer(playerIndex);
	if (!player) {
		return;
	}
	detectionSystem.OnSetupMove(player, command, engine::CurrentTick());
}

void Plugin::OnGameFrame(bool simulating)
{
	if (!loaded) {
		return;
	}
	// The per-frame player snapshot is live for exactly this call: the passes below (and every
	// detector they drive) read each player-state value from one record instead of calling s2sdk
	// again, and the hooks/events - which run outside this scope - keep reading the engine live.
	// The scope is RAII so an early return or an exception unwinding to the s2sdk listener turns it
	// off again.
	engine::FrameSnapshotScope frameSnapshot;
	simulatingPhysics = simulating;
	// One cvar read per frame per gate instead of one per call per player (the cache is
	// invalidated here, so a change is still seen within the same frame), and the client-cvar
	// replies queued by s2sdk's callback are handled on this normal game-thread path.
	engine::OnFrameRefresh();
	engine::DrainCvarReplies();
	const int currentTick = engine::CurrentTick();
	if (simulating) {
		const f64 now = engine::Realtime();
		// Pass 1: teleport/spawn sampling, so the position frames captured below see the current
		// flags (the original got this from its CBaseEntity::Teleport hook).
		for (int index = 1; index <= MAXPLAYERS; ++index) {
			auto* player = g_pPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || !player->IsInGame() || player->IsFakeClient() || player->IsCSTV()) {
				engine::ForgetTeleportTracking(index);
				continue;
			}
			engine::UpdateTeleportTrackingFor(index);
		}

		detectionSystem.OnGameFrame(currentTick);

		// Pass 2: the movement detectors' per-frame sampler (the port's replacement for the
		// original's movement-hook bookkeeping).
		const f64 delta = now - lastFrameTime;
		for (int index = 1; index <= MAXPLAYERS; ++index) {
			auto* player = g_pPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || !player->IsInGame() || player->IsFakeClient() || player->IsCSTV()) {
				continue;
			}
			auto& timeInServer = playerTime[index];
			if (delta > 0.0 && delta < 1.0) {
				timeInServer += delta;
			}
			detectionSystem.MovementOnGameFrame(player, timeInServer);
			// The Invalid CVar integrity cycle (cvars.cpp CheckClientCvars): the service spaces one
			// queried cvar per cvarCycleInterval / 10 and ignores the call until the next one is due.
			if (settings::IsDetectionEnabled(DetectionType::InvalidCvar)) {
				detectionSystem.MovementCvars(player);
			}
		}
		lastFrameTime = now;
	}
	announce::OnGameFrame();
	if (webhook) {
		webhook->OnGameFrame();
	}
	ProcessJoinWatermarks();
}

void Plugin::OnGameEvent(const detection::GameEventData& event, int playerIndex)
{
	if (!loaded || !settings::IsPluginEnabled()) {
		return;
	}
	PlayerRef* player = nullptr;
	if (playerIndex >= 1 && playerIndex <= MAXPLAYERS) {
		player = g_pPlayerManager->ToPlayer(playerIndex);
	} else if (event.userid >= 1 && event.userid <= MAXPLAYERS) {
		// event.userid is already the controller index (slot + 1): hooks.cpp resolves it with
		// s2sdk's GetEventPlayerSlot. Without this the player_spawn branch below never ran.
		player = g_pPlayerManager->ToPlayer(event.userid);
	}
	const int currentTick = engine::CurrentTick();
	detectionSystem.OnGameEvent(event, player, currentTick);
	if (event.name == "player_spawn" && player) {
		engine::MarkSpawned(player->index);
	}
}

void Plugin::OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField,
						   const std::string& missing)
{
	if (!loaded || !settings::IsPluginEnabled()) {
		return;
	}
	detectionSystem.OnFireBullets(playerIndex, currentTick, inaccuracy, spread, weaponId, missingField, missing);
}

// ---------------------------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------------------------

void Plugin::OnClientActive(int slot, u64 /*xuid*/)
{
	if (!loaded || slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	auto* player = g_pPlayerManager->ToPlayer(slot + 1);
	detectionSystem.OnClientReady(player);
	detectionSystem.MovementOnClientReady(player);
}

void Plugin::OnClientFullyConnect(int slot)
{
	if (!loaded || slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	const int index = slot + 1;
	auto* player = g_pPlayerManager->ToPlayer(index);
	// A reused player slot must never inherit the previous occupant's punishment state.
	punishmentLevels[static_cast<size_t>(index)] = PunishmentLevel::None;
	streamReports[static_cast<size_t>(index)].clear();
	reportedKinds[static_cast<size_t>(index)].clear();
	detectionSystem.OnClientReady(player);
	// Off by default: with players reconnecting for workshop addons the notice showed up on every
	// join and the owner found it spammy (fork v7 / cs2ac_join_watermark).
	if (settings::ShowJoinWatermark() && player && !player->IsFakeClient() && !player->IsCSTV()
		&& !joinWatermarks[static_cast<size_t>(index)].shown && !joinWatermarks[static_cast<size_t>(index)].pending) {
		joinWatermarks[static_cast<size_t>(index)].showAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		joinWatermarks[static_cast<size_t>(index)].pending = true;
	}
}

void Plugin::OnClientSettingsChanged(int slot)
{
	if (!loaded || slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	detectionSystem.OnClientSettingsChanged(g_pPlayerManager->ToPlayer(slot + 1));
}

void Plugin::OnClientDisconnect(int slot)
{
	if (slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	auto* player = g_pPlayerManager->ToPlayer(slot + 1);
	if (loaded) {
		detectionSystem.OnClientDisconnect(player);
	}
	const int index = slot + 1;
	joinWatermarks[static_cast<size_t>(index)] = {};
	playerTime[static_cast<size_t>(index)] = 0.0;
	engine::ForgetTeleportTracking(index);
	// The slot's next occupant must not inherit this player's user id.
	engine::ForgetClientUserId(index);
	if (player) {
		punishmentLevels[static_cast<size_t>(player->index)] = PunishmentLevel::None;
		streamReports[static_cast<size_t>(player->index)].clear();
		reportedKinds[static_cast<size_t>(player->index)].clear();
		// The player's name is not queried here: during a map shutdown (and for a client that is
		// already gone) s2sdk's GetClientName dereferences a null client entry. The slot is enough
		// to identify the log line.
		Log("Cleared the detection evidence of player slot %d because they disconnected.", slot);
	}
}

// ---------------------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------------------

void Plugin::LoadConfigFile()
{
	// A reload re-resolves every cs2ac_* cvar (they may have been (re)created since the last one).
	engine::InvalidateConVarCache();
	const std::string path = ConfigDirectory() + "/cs2ac.cfg";
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		LogWarning("cs2ac.cfg was not found at %s; the built-in defaults (the shipped file's values) are in effect.", path.c_str());
		configLoadFailed = true;
		configReloadPending = false;
		FinishConfigLoad();
		return;
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	const std::vector<ConfigLine> lines = ParseConfigFile(buffer.str());
	bool sawMarker = false;
	for (const ConfigLine& line : lines) {
		if (EqualsIgnoreCase(line.name, "cs2ac_config_loaded")) {
			sawMarker = true;
			continue;
		}
		ApplyConfigLine(line.name, line.value);
	}
	if (!sawMarker) {
		configLoadFailed = true;
		LogWarning("cs2ac.cfg did not end with the cs2ac_config_loaded marker.");
	}
	FinishConfigLoad();
}

// The original's settings were live ConVars: an admin could change any cs2ac_* value at any time
// and the change callbacks updated the masks immediately. The port keeps the same behaviour by
// re-reading the registered ConVars once per second and applying only what changed.
void Plugin::SyncSettingsFromConVars()
{
	for (const auto& spec : settings::kConVars) {
		const std::string current = engine::ServerCvarString(spec.name);
		if (current.empty() && spec.isString && std::string(spec.defaultValue).empty()) {
			continue; // an unset empty string stays as the file/default value
		}
		const std::string known = settings::RawValue(spec.name);
		if (spec.isString) {
			if (!current.empty() && current != known) {
				settings::SetString(spec.name, current.c_str());
			}
		} else if (!current.empty()) {
			const bool enabled = current == "1" || EqualsIgnoreCase(current, "true") || EqualsIgnoreCase(current, "yes");
			if (current != known) {
				settings::SetBool(spec.name, enabled);
			}
		}
	}
	for (const auto& spec : settings::kDebugConVars) {
		const std::string current = engine::ServerCvarString(spec.name);
		if (current.empty()) {
			continue;
		}
		const bool enabled = current == "1" || EqualsIgnoreCase(current, "true") || EqualsIgnoreCase(current, "yes");
		if (settings::DebugFlagValue(spec.name) != enabled) {
			settings::SetBool(spec.name, enabled);
		}
	}
	for (const auto& spec : settings::kNumericConVars) {
		const std::string current = engine::ServerCvarString(spec.name);
		if (current.empty() || current == settings::RawValue(spec.name)) {
			continue;
		}
		if (spec.isFloat) {
			settings::SetFloat(spec.name, std::strtof(current.c_str(), nullptr));
		} else {
			settings::SetInt(spec.name, std::atoi(current.c_str()));
		}
	}
}

void Plugin::ApplyConfigLine(const std::string& name, const std::string& value)
{
	// Booleans and strings go through the same setters the original's ConVar change callbacks used.
	bool isBoolean = false;
	for (const auto& spec : settings::kConVars) {
		if (EqualsIgnoreCase(name, spec.name)) {
			isBoolean = !spec.isString;
			break;
		}
	}
	if (!isBoolean) {
		for (const auto& spec : settings::kDebugConVars) {
			if (EqualsIgnoreCase(name, spec.name)) {
				isBoolean = true;
				break;
			}
		}
	}
	if (isBoolean) {
		const bool enabled = value == "1" || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "yes");
		settings::SetBool(name.c_str(), enabled);
	} else {
		bool numeric = false;
		for (const auto& spec : settings::kNumericConVars) {
			if (EqualsIgnoreCase(name, spec.name)) {
				if (spec.isFloat) {
					settings::SetFloat(spec.name, std::strtof(value.c_str(), nullptr));
				} else {
					settings::SetInt(spec.name, std::atoi(value.c_str()));
				}
				numeric = true;
				break;
			}
		}
		if (!numeric) {
			settings::SetString(name.c_str(), value.c_str());
		}
	}
	// Keep the registered ConVar in sync so `cs2ac_status`-style console reads show the file value.
	engine::SetCvarValue(name.c_str(), value.c_str());
}

void Plugin::FinishConfigLoad()
{
	// The original's OnConfigLoaded: "reloaded" reports the previous state, then the summary.
	const bool reloaded = configLoaded;
	configLoaded = true;
	configReloadPending = false;
	configLoadFailed = false;
	lastConfigLoad = std::chrono::steady_clock::now();
	settings::MarkConfigReloaded();
	localization::Reload(settings::GetLanguage(), ConfigDirectory() + "/translations");
	if (webhook) {
		webhook->Reload();
	}
	PrintConfigSummary(reloaded);
	CheckConfig();
}

void Plugin::OnConfigLoaded()
{
	FinishConfigLoad();
}

void Plugin::ReloadConfig()
{
	if (configReloadPending) {
		ConsoleMessage("A configuration load is already in progress. Please wait for it to finish.");
		return;
	}
	configReloadPending = true;
	configLoadFailed = false;
	configLoadDeadline = std::chrono::steady_clock::now() + configLoadTimeout;
	ConsoleMessage("Reloading cs2ac.cfg.");
	LoadConfigFile();
}

void Plugin::ReportConfigLoadTimeout()
{
	if (!configReloadPending) {
		return;
	}
	configReloadPending = false;
	configLoadFailed = true;
	ConsoleMessage("cs2ac.cfg did not finish loading. Make sure it exists and ends with cs2ac_config_loaded.");
	ConsoleMessage("Some settings may have changed before loading stopped. Run cs2ac_check_config before continuing.");
}

void Plugin::OnUpdate()
{
	try {
		if (!loaded) {
			return;
		}
		// Bans are executed here, outside every per-client hook: admin_system's AddPlayerPunishment
		// disconnects the client synchronously (fork admin_bridge).
		admin_bridge::ProcessQueue();
		ProcessServerCommands();
		if (configReloadPending && std::chrono::steady_clock::now() >= configLoadDeadline) {
			ReportConfigLoadTimeout();
		}

		// Once per second the plugin's settings are resynced from the registered ConVars, so
		// `cs2ac_bhop_enabled 0` and `cs2ac_aimbot_debug 1` typed at the console behave like the
		// original's ConVar callbacks. The Invalid CVar integrity cycle is paced by the movement
		// service itself and runs from the frame loop (Plugin::OnGameFrame).
		const auto now = std::chrono::steady_clock::now();
		if (now - lastCvarCycle >= std::chrono::seconds(1)) {
			lastCvarCycle = now;
			SyncSettingsFromConVars();
		}
	} catch (...) {
		// OnPluginUpdate is called from the module; nothing may unwind through it.
	}
}

// ---------------------------------------------------------------------------------------------
// Detection handling (cs2ac.cpp HandleDetection)
// ---------------------------------------------------------------------------------------------

void Plugin::HandleDetection(const char* detection, PlayerRef* player, const localization::Text& evidence, bool kickOnly, bool networkVetoed)
{
	if (!detection || !*detection || !player || player->index < 1 || player->index > MAXPLAYERS) {
		return;
	}

	const std::string playerName = SanitizeConsoleText(player->GetName());
	const u64 steamId = player->GetSteamId64(false);
	const char* loggedSeverity = "";
	auto finish = [&](announce::DetectionOutcome outcome) {
		detection_log::Write(detection, player, steamId, evidence.english, loggedSeverity, outcome);
		announce::AnnounceDetectionThrottled(steamId, detection, player->GetName(), outcome,
											 settings::IntSetting("cs2ac_announce_cooldown", 30));
		if (webhook) {
			webhook->Report(detection, player->index, evidence.localized, outcome);
		}
	};
	if (steamId) {
		ConsoleMessage("Detected %s on %s (SteamID64 %llu).", detection, playerName.c_str(), static_cast<unsigned long long>(steamId));
		NoteFinding(steamId, detection);
	} else {
		ConsoleMessage("Detected %s on %s (SteamID64 unavailable).", detection, playerName.c_str());
	}
	if (!evidence.english.empty()) {
		ConsoleMessage("Evidence: %s", SanitizeConsoleText(evidence.english.c_str()).c_str());
	}
	// The network check only ever stops a ban; a listed finding still kicks (v10).
	bool vetoedKick = false;
	if (networkVetoed) {
		const char* unstableList = settings::RawValue("cs2ac_unstable_kick_detections");
		if (!steamId || !detection::ListContains(unstableList, detection)) {
			finish(announce::DetectionOutcome::NetworkUnstable);
			ConsoleMessage("No punishment was sent because %s's connection exceeded the safe network limits.", playerName.c_str());
			return;
		}
		vetoedKick = true;
		ConsoleMessage("%s's connection exceeded the safe network limits, so %s kicks instead of banning.", playerName.c_str(), detection);
	}
	if (!steamId) {
		finish(announce::DetectionOutcome::IdentityUnavailable);
		ConsoleMessage("No punishment was sent because %s's SteamID64 is not ready yet.", playerName.c_str());
		return;
	}

	const bool whitelisted = settings::IsPlayerWhitelisted(steamId);
	if (whitelisted) {
		finish(announce::DetectionOutcome::Whitelisted);
		ConsoleMessage("No punishment was sent because %s is whitelisted.", playerName.c_str());
		return;
	}

	detection::DetectionSeverity severity =
		vetoedKick ? detection::DetectionSeverity::Kick
				   : detection::SeverityForDetection(detection, kickOnly, settings::RawValue("cs2ac_ban_detections"),
													 settings::RawValue("cs2ac_kick_detections"), settings::WeakSignalsKick());
	if (severity == detection::DetectionSeverity::Ban && MassGuardDemotes(detection, steamId)) {
		severity = detection::DetectionSeverity::Report;
		ConsoleMessage("%s is only reported: the mass guard for %s is active on this server.", playerName.c_str(), detection);
	}
	const bool reportOnlyDetection = detection::IsReportOnlyDetection(detection);
	if (severity == detection::DetectionSeverity::Report && !reportOnlyDetection
		&& StreamKickDue(player->index, detection, settings::RawValue("cs2ac_stream_kick_detections"),
						 settings::IntSetting("cs2ac_stream_kick_reports", 20), settings::IntSetting("cs2ac_stream_kick_seconds", 180),
						 "stream:")) {
		severity = detection::DetectionSeverity::Kick;
		ConsoleMessage("%s tripped %s %d times within %d s, so the finding is escalated to a kick.", playerName.c_str(), detection,
					   settings::IntSetting("cs2ac_stream_kick_reports", 20), settings::IntSetting("cs2ac_stream_kick_seconds", 180));
	}
	if (severity == detection::DetectionSeverity::Report && !reportOnlyDetection
		&& StreamKickDue(player->index, detection, settings::RawValue("cs2ac_burst_kick_detections"),
						 settings::IntSetting("cs2ac_burst_kick_reports", 3), settings::IntSetting("cs2ac_burst_kick_seconds", 60), "burst:")) {
		severity = detection::DetectionSeverity::Kick;
		ConsoleMessage("%s tripped %s %d times within %d s, so the finding is escalated to a kick.", playerName.c_str(), detection,
					   settings::IntSetting("cs2ac_burst_kick_reports", 3), settings::IntSetting("cs2ac_burst_kick_seconds", 60));
	}
	// The kick reason names the combination, not just the finding that completed it.
	const char* punishedAs = detection;
	if (severity == detection::DetectionSeverity::Report && !reportOnlyDetection && ComboKickDue(player->index, detection)) {
		severity = detection::DetectionSeverity::Kick;
		punishedAs = "MULTIPLE SIGNALS";
		ConsoleMessage("%s tripped %d different report-only detections in this connection (latest %s), so the finding is escalated to a kick.",
					   playerName.c_str(), settings::IntSetting("cs2ac_combo_kick_types", 3), detection);
	}
	PunishmentLevel requested = severity == detection::DetectionSeverity::Ban    ? PunishmentLevel::Ban
								: severity == detection::DetectionSeverity::Kick ? PunishmentLevel::Kick
																				 : PunishmentLevel::Report;
	// A second kick for the same finding within the window becomes a temporary ban. Only through
	// admin_system: the fallback ban command is permanent.
	int banSeconds = 0;
	if (requested == PunishmentLevel::Kick && punishmentLevels[static_cast<size_t>(player->index)] < PunishmentLevel::Kick
		&& admin_bridge::IsAvailable() && RepeatBanDue(steamId, detection)) {
		requested = PunishmentLevel::Ban;
		banSeconds = (std::max)(1, settings::IntSetting("cs2ac_repeat_ban_minutes", 1440)) * 60;
		ConsoleMessage("%s was already kicked for %s within %d h, so this kick becomes a %d-minute ban.", playerName.c_str(), detection,
					   settings::IntSetting("cs2ac_repeat_ban_hours", 24), banSeconds / 60);
	}
	const char* requestedName = requested == PunishmentLevel::Ban	  ? (banSeconds > 0 ? "tempban" : "ban")
								: requested == PunishmentLevel::Kick ? "kick"
																	 : "report";
	loggedSeverity = requestedName;
	auto& issued = punishmentLevels[static_cast<size_t>(player->index)];
	ConsoleMessage("Punishment decision for %s: %s has %s severity (previous level %d).", playerName.c_str(), detection, requestedName,
				   static_cast<int>(issued));
	if (issued >= requested) {
		const bool reportOnly = requested == PunishmentLevel::Report;
		finish(reportOnly ? announce::DetectionOutcome::ReportOnly : announce::DetectionOutcome::AlreadyPunished);
		ConsoleMessage(reportOnly ? "Repeated report-only finding for %s; no command was sent."
								  : "No duplicate punishment was sent for %s.",
					   playerName.c_str());
		return;
	}
	if (requested == PunishmentLevel::Report) {
		issued = PunishmentLevel::Report;
		finish(announce::DetectionOutcome::ReportOnly);
		ConsoleMessage("No command was sent: %s is a report-only finding. Evidence was logged and reported for later review.", detection);
		return;
	}

	// Ban: prefer admin_system's API. The command template is only the fallback.
	const char* banTemplate = settings::GetPunishmentCommand();
	if (requested == PunishmentLevel::Ban && banTemplate && *banTemplate && admin_bridge::IsAvailable()) {
		std::string fallback;
		if (banSeconds > 0) {
			// No fallback for a temporary ban: the configured command bans permanently.
		} else if (engine::BindingsAvailable()) {
			fallback = banTemplate;
			ReplaceAll(fallback, "{steamid64}", std::to_string(steamId));
			ReplaceAll(fallback, "{detection}", detection);
		} else {
			ConsoleMessage("The server command service is unavailable; no fallback command was prepared for %s.", playerName.c_str());
		}
		// Keep the reason short and without the player's name: it lands in the DB and in a chat
		// broadcast. The evidence itself goes to the webhook.
		const std::string reason = std::string("CS2AC: ") + detection;
		admin_bridge::QueueBan(player->GetPlayerSlot(), steamId, player->GetName() ? std::string(player->GetName()) : std::string(), banSeconds,
							   reason, fallback, std::string_view(banTemplate).find("{userid}") != std::string_view::npos);
		finish(announce::DetectionOutcome::PunishmentSent);
		issued = requested;
		if (banSeconds > 0) {
			NoteKick(steamId, detection);
		}
		ConsoleMessage("Ban queued through admin_system for %s (%s).", playerName.c_str(), detection);
		return;
	}
	if (requested == PunishmentLevel::Ban && banSeconds > 0) {
		// The repeat ban needs admin_system; without it the finding stays a kick.
		requested = PunishmentLevel::Kick;
		loggedSeverity = "kick";
	}

	const char* commandTemplate = requested == PunishmentLevel::Kick ? settings::GetKickCommand() : settings::GetPunishmentCommand();
	if (!commandTemplate || !*commandTemplate) {
		finish(announce::DetectionOutcome::PunishmentDisabled);
		ConsoleMessage("No punishment was sent because the matching command is empty.");
		return;
	}
	if (!engine::BindingsAvailable()) {
		finish(announce::DetectionOutcome::CommandServiceUnavailable);
		ConsoleMessage("No punishment was sent because the server command service is unavailable.");
		return;
	}

	const int userId = engine::PlayerUserId(player->index);
	std::string command = commandTemplate;
	if (userId < 0 && command.find("{userid}") != std::string::npos) {
		finish(announce::DetectionOutcome::IdentityUnavailable);
		ConsoleMessage("No punishment was sent because %s's user ID is not ready yet.", playerName.c_str());
		return;
	}
	ReplaceAll(command, "{steamid64}", std::to_string(steamId));
	ReplaceAll(command, "{userid}", std::to_string(userId));
	ReplaceAll(command, "{detection}", punishedAs);
	// CCommand::MaxCommandLength() of the engine.
	constexpr std::size_t kMaxCommandLength = 512;
	if (command.size() >= kMaxCommandLength) {
		finish(announce::DetectionOutcome::CommandTooLong);
		ConsoleMessage("No punishment was sent because the configured command is too long.");
		return;
	}
	// The command runs from OnPluginUpdate, never inside the hook that produced the finding; a
	// {userid} command is re-validated against the slot's SteamID64 when it runs.
	const bool requiresSlot = std::string_view(commandTemplate).find("{userid}") != std::string_view::npos;
	const std::string queuedCommand = command;
	// The template is queued unexpanded: {userid} is resolved again when the command runs so it
	// cannot belong to an earlier connection of the same slot.
	if (!QueueServerCommand(std::string(commandTemplate), player->GetPlayerSlot(), steamId, requiresSlot, punishedAs)) {
		finish(announce::DetectionOutcome::CommandServiceUnavailable);
		ConsoleMessage("No punishment was sent because the punishment queue is full.");
		return;
	}
	finish(announce::DetectionOutcome::PunishmentSent);
	ConsoleMessage("Queued: %s", SanitizeConsoleText(queuedCommand.c_str()).c_str());
	issued = requested;
	if (requested == PunishmentLevel::Kick) {
		NoteKick(steamId, detection);
	}
}

// ---------------------------------------------------------------------------------------------
// Fork v8-v11 punishment policy (cs2ac.cpp)
// ---------------------------------------------------------------------------------------------

bool Plugin::QueueServerCommand(std::string command, int slot, u64 steamId, bool requiresSlot, const std::string& detection)
{
	if (command.empty()) {
		return false;
	}
	constexpr std::size_t kMaxPendingCommands = 64;
	if (pendingServerCommands.size() >= kMaxPendingCommands) {
		LogWarning("The punishment queue is full; the queued command for %s was dropped.", detection.c_str());
		return false;
	}
	pendingServerCommands.push_back({std::move(command), detection, slot, steamId, requiresSlot});
	return true;
}

void Plugin::ProcessServerCommands()
{
	if (pendingServerCommands.empty()) {
		return;
	}
	// Swap first: a command can produce engine events that reach back into the plugin.
	std::deque<PendingServerCommand> batch;
	batch.swap(pendingServerCommands);
	for (const PendingServerCommand& pending : batch) {
		if (!engine::GameReady() || !engine::BindingsAvailable()) {
			LogWarning("Dropped the queued punishment for %s: the server command service or the map is unavailable.", pending.detection.c_str());
			continue;
		}
		// A {userid} command is slot-specific: the slot must still hold the same SteamID64 at the
		// moment it runs, otherwise the engine's userid could belong to the next occupant.
		if (pending.requiresSlot
			&& (pending.slot < 0 || pending.slot >= MAXPLAYERS || !engine::PlayerConnected(pending.slot + 1)
				|| engine::PlayerSteamId(pending.slot + 1) != pending.steamId)) {
			LogWarning("Dropped the queued punishment for %s: slot %d no longer holds SteamID64 %llu.", pending.detection.c_str(), pending.slot,
					   static_cast<unsigned long long>(pending.steamId));
			continue;
		}
		std::string command = pending.commandTemplate;
		if (pending.requiresSlot) {
			const int userId = engine::PlayerUserId(pending.slot + 1);
			if (userId < 0) {
				LogWarning("Dropped the queued punishment for %s: the player's userid is not ready.", pending.detection.c_str());
				continue;
			}
			ReplaceAll(command, "{userid}", std::to_string(userId));
		}
		ReplaceAll(command, "{steamid64}", std::to_string(pending.steamId));
		ReplaceAll(command, "{detection}", pending.detection);
		constexpr std::size_t kMaxCommandLength = 512;
		if (command.size() >= kMaxCommandLength) {
			LogWarning("Dropped the queued punishment for %s: the expanded command exceeds the engine limit.", pending.detection.c_str());
			continue;
		}
		command.push_back('\n');
		engine::ServerCommand(command.c_str());
		command.pop_back();
		ConsoleMessage("Sent: %s", SanitizeConsoleText(command.c_str()).c_str());
	}
}

bool Plugin::StreamKickDue(int index, const char* detection, const char* list, int threshold, int seconds, const char* prefix)
{
	if (index < 1 || index > MAXPLAYERS || threshold <= 0 || seconds <= 0 || !detection::ListContains(list, detection)) {
		return false;
	}
	const std::string key = std::string(prefix) + detection;
	return detection::WindowedCountReached(streamReports[static_cast<size_t>(index)][key], NowSeconds(), threshold, seconds);
}

bool Plugin::ComboKickDue(int index, const char* detection)
{
	const int needed = settings::IntSetting("cs2ac_combo_kick_types", 3);
	if (index < 1 || index > MAXPLAYERS) {
		return false;
	}
	return detection::ComboReached(reportedKinds[static_cast<size_t>(index)], detection,
								   settings::RawValue("cs2ac_combo_ignore_detections"), needed);
}

void Plugin::NoteFinding(u64 steamId, const char* detection)
{
	if (!settings::RawValue("cs2ac_repeat_ban_detections")[0]) {
		return;
	}
	if (repeatRecords.size() > 4096) {
		PruneRepeatRecords(false);
	}
	auto& record = repeatRecords[steamId];
	const i64 now = NowSeconds();
	record.lastSeen = now;
	record.findings[detection] = now;
}

void Plugin::NoteKick(u64 steamId, const char* detection)
{
	if (!detection::ListContains(settings::RawValue("cs2ac_repeat_ban_detections"), detection)) {
		return;
	}
	auto& record = repeatRecords[steamId];
	const i64 now = NowSeconds();
	record.lastSeen = now;
	record.kicks[detection].push_back(now);
}

bool Plugin::RepeatBanDue(u64 steamId, const char* detection)
{
	const int hours = settings::IntSetting("cs2ac_repeat_ban_hours", 24);
	if (!steamId || hours <= 0) {
		return false;
	}
	const auto found = repeatRecords.find(steamId);
	if (found == repeatRecords.end()) {
		return false;
	}
	auto& kicks = found->second.kicks[detection];
	return detection::RepeatBanReached(kicks, found->second.findings, detection, settings::RawValue("cs2ac_repeat_ban_detections"),
									   settings::RawValue("cs2ac_repeat_ban_needs_other"), NowSeconds(), hours * 3600);
}

void Plugin::PruneRepeatRecords(bool all)
{
	if (all) {
		repeatRecords.clear();
		return;
	}
	const i64 now = NowSeconds();
	const i64 window = static_cast<i64>((std::max)(1, settings::IntSetting("cs2ac_repeat_ban_hours", 24))) * 3600;
	for (auto it = repeatRecords.begin(); it != repeatRecords.end();) {
		it = now - it->second.lastSeen > window ? repeatRecords.erase(it) : std::next(it);
	}
}

bool Plugin::MassGuardDemotes(const char* detection, u64 steamId)
{
	const int limit = settings::IntSetting("cs2ac_mass_guard_players", 5);
	if (limit <= 0 || !detection::ListContains(settings::RawValue("cs2ac_mass_guard_detections"), detection)) {
		return false;
	}
	const std::string key(detection);
	if (massGuardTripped.count(key)) {
		return true;
	}
	const int windowSeconds = (std::max)(1, settings::IntSetting("cs2ac_mass_guard_minutes", 60)) * 60;
	auto& hits = massGuardHits[key];
	if (!detection::MassGuardReached(hits, NowSeconds(), windowSeconds, steamId, limit)) {
		return false;
	}
	massGuardTripped.insert(key);
	LogWarning("%d different players tripped %s within %d minutes. That looks like a game update, not cheaters, so %s only reports until "
			   "CS2AC reloads. Check recent %s bans.",
			   limit, detection, settings::IntSetting("cs2ac_mass_guard_minutes", 60), detection, detection);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Join watermarks (cs2ac.cpp ProcessJoinWatermarks)
// ---------------------------------------------------------------------------------------------

void Plugin::ProcessJoinWatermarks()
{
	const auto now = std::chrono::steady_clock::now();
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& state = joinWatermarks[static_cast<size_t>(index)];
		if (!state.pending && !state.centerPending && !state.centerActive) {
			continue;
		}
		auto* player = g_pPlayerManager->ToPlayer(index);
		if (!player || !player->IsConnected() || player->IsFakeClient() || player->IsCSTV()) {
			state = {};
			continue;
		}
		if (state.pending && now >= state.showAt && player->IsInGame()) {
			state.pending = false;
			state.shown = true;
			state.centerPending = true;
			announce::AnnounceWatermarkTo(player->GetPlayerSlot(), false);
		}
		if (state.centerPending && player->IsInGame() && !announce::IsDetectionAnnouncementActive()) {
			state.centerPending = false;
			announce::AnnounceWatermarkTo(player->GetPlayerSlot(), true);
			state.centerActive = true;
			state.expires = now + std::chrono::seconds(3);
			state.nextCenterSend = now + std::chrono::milliseconds(100);
			state.centerBroadcasts = 1;
		}
		if (!state.centerActive) {
			continue;
		}
		if (announce::IsDetectionAnnouncementActive() || !player->IsInGame()) {
			state.centerActive = false;
			continue;
		}
		if (now >= state.expires) {
			announce::ClearWatermarkFor(player->GetPlayerSlot());
			state.centerActive = false;
			continue;
		}
		if (now >= state.nextCenterSend && state.centerBroadcasts < 31) {
			announce::AnnounceWatermarkTo(player->GetPlayerSlot(), true);
			state.nextCenterSend = now + std::chrono::milliseconds(100);
			++state.centerBroadcasts;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Commands (cs2ac.cpp PrintStatus/PrintHelp/CheckConfig/…)
// ---------------------------------------------------------------------------------------------

void Plugin::PrintConfigSummary(bool reloaded)
{
	const std::size_t enabled = settings::GetEnabledDetectionCount();
	const std::size_t total = static_cast<std::size_t>(DetectionType::Count);
	ConsoleMessage("Configuration %s: %zu/%zu detectors enabled, %zu whitelisted SteamIDs, %zu rejected entries, %zu duplicates ignored.",
				   reloaded ? "reloaded" : "loaded", enabled, total, settings::GetWhitelistCount(), settings::GetRejectedWhitelistCount(),
				   settings::GetDuplicateWhitelistCount());
	ConsoleMessage("Public announcements: chat %s, center screen %s.", settings::ShowChatAnnouncements() ? "on" : "off",
				   settings::ShowCenterAnnouncements() ? "on" : "off");
	// The auto-updater is not ported (PORT_PLAN.md §6.1): the configured flags are reported as-is
	// so the admin sees the same summary, but deployment is the Plugify package's job.
	ConsoleMessage("Automatic updates: %s (not ported; deployment is handled outside the plugin).",
				   settings::AutomaticUpdatesEnabled() ? "on" : "off");
	ConsoleMessage("Punishments: permanent ban %s, kick %s.",
				   settings::GetPunishmentCommand() && *settings::GetPunishmentCommand() ? "configured" : "disabled",
				   settings::GetKickCommand() && *settings::GetKickCommand() ? "configured" : "disabled");
	ConsoleMessage("Ban detections: %s.", *settings::RawValue("cs2ac_ban_detections") ? settings::RawValue("cs2ac_ban_detections") : "none");
	ConsoleMessage("Permanent bans are limited to multi-episode evidence; other findings are %s.",
				   settings::WeakSignalsKick() ? "kick-only" : "report-only by default");
	if (!settings::AutomaticUpdatesEnabled()) {
		ConsoleMessage("Automatic updates are off. Set cs2ac_update_repository to this fork's owner/repo to enable them (the port does not "
					   "self-update; deployment owns the binary).");
	}
	ConsoleMessage("Discord webhook: %s.",
				   webhook && webhook->IsConfigured() ? (webhook->IsDisabled() ? "disabled after an error" : "configured") : "not configured");
}

void Plugin::PrintHelp()
{
	ConsoleMessage("Administrator commands:");
	ConsoleMessage("cs2ac_status - Show the plugin, player, detector, announcement, and punishment status.");
	ConsoleMessage("cs2ac_reload - Reload cs2ac.cfg and clear transient detector evidence when it finishes.");
	ConsoleMessage("cs2ac_check_config - Check the current settings without changing them.");
	ConsoleMessage("cs2ac_test_announcement - Show a harmless chat and center-screen test without punishing anyone.");
	ConsoleMessage("cs2ac_webhook_test - Send a harmless Discord test report.");
	ConsoleMessage("cs2ac_hooks - Show which gamedata signatures/hooks resolved on this server build.");
	ConsoleMessage("cs2ac_usercmd_dump - Show the fields of the usercmds parsed by the hook (diagnostic).");
}

void Plugin::CheckConfig()
{
	ConsoleMessage("Checking the current configuration. Nothing will be changed.");
	int findings = 0;
	if (configReloadPending) {
		ConsoleMessage("Review cs2ac.cfg: it is still loading, so this check may not reflect the whole file yet.");
		++findings;
	} else if (configLoadFailed) {
		ConsoleMessage("Review cs2ac.cfg: the last load did not reach its final cs2ac_config_loaded marker.");
		++findings;
	}
	if (!settings::IsPluginEnabled()) {
		ConsoleMessage("Review cs2ac_enabled: detection is currently disabled.");
		++findings;
	}
	if (settings::GetRejectedWhitelistCount()) {
		ConsoleMessage("Review cs2ac_whitelist: %zu entries were rejected because they are not valid SteamID64 values.",
					   settings::GetRejectedWhitelistCount());
		++findings;
	}
	if (settings::GetDuplicateWhitelistCount()) {
		ConsoleMessage("Review cs2ac_whitelist: %zu duplicate entries were ignored.", settings::GetDuplicateWhitelistCount());
		++findings;
	}
	findings += CheckCommandTemplate("cs2ac_punishment_command", settings::GetPunishmentCommand(), true);
	if (settings::WeakSignalsKick()) {
		findings += CheckCommandTemplate("cs2ac_kick_command", settings::GetKickCommand(), false);
	} else if (settings::GetKickCommand() && *settings::GetKickCommand()) {
		ConsoleMessage("Note: cs2ac_kick_command is configured, but weak findings are report-only. Set cs2ac_weak_signal_policy to kick to use it.");
	}
	if (!WebhookService::IsValidUrl(settings::GetWebhookUrl())) {
		ConsoleMessage("Review cs2ac_webhook_url: it is not a Discord webhook URL.");
		++findings;
	}
	if (!WebhookService::IsValidRoleId(settings::GetWebhookRoleId())) {
		ConsoleMessage("Review cs2ac_webhook_role_id: it must contain only numbers.");
		++findings;
	}
	if (!WebhookService::IsValidLogoUrl(settings::GetWebhookLogoUrl())) {
		ConsoleMessage("Review cs2ac_webhook_logo_url: it must be an HTTPS URL.");
		++findings;
	}
	if (!settings::ShowChatAnnouncements() && !settings::ShowCenterAnnouncements()) {
		ConsoleMessage("Review announcements: chat and center screen are both disabled. Detections will only appear in the server console.");
		++findings;
	}
	if (settings::AllowSvCheatsTesting()) {
		ConsoleMessage("Review sv_cheats testing: inherited movement detections are allowed while sv_cheats is on.");
		++findings;
	}
	// Informational: {userid} is filled from the engine userid the game events carry, and the
	// gamedata offset (when present and trusted) covers players that have not appeared in one yet.
	ConsoleMessage("The {userid} placeholder uses the engine user ids from game events; %s.", engine::ClientUserIdStatus().c_str());

	if (findings) {
		ConsoleMessage("Configuration check found %d %s to review.", findings, findings == 1 ? "thing" : "things");
	} else {
		ConsoleMessage("Configuration check passed. Everything is ready.");
	}
}

void Plugin::TestAnnouncement()
{
	ConsoleMessage("Running a harmless announcement test. No detection or punishment will be created.");
	announce::AnnounceTest();
}

void Plugin::TestWebhook()
{
	if (webhook) {
		webhook->Test();
	}
}

void Plugin::PrintStatus()
{
	int connected = 0;
	int humans = 0;
	int bots = 0;
	if (g_pPlayerManager && engine::GameReady()) {
		for (int index = 1; index <= MAXPLAYERS; ++index) {
			auto* player = g_pPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || player->IsCSTV()) {
				continue;
			}
			++connected;
			player->IsFakeClient() ? ++bots : ++humans;
		}
	}

	std::string enabledNames;
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index) {
		if (!settings::IsDetectionEnabled(static_cast<DetectionType>(index))) {
			continue;
		}
		if (!enabledNames.empty()) {
			enabledNames += ", ";
		}
		enabledNames += kDetectionNames[index];
	}

	ConsoleMessage("Status: %s, version %s.", loaded ? "running" : "not running", CS2AC_VERSION);
	if (configReloadPending) {
		ConsoleMessage("Configuration: waiting for cs2ac.cfg to finish.");
	} else if (configLoadFailed) {
		ConsoleMessage("Configuration: the last load did not finish. Run cs2ac_check_config after fixing the file.");
	} else if (!configLoaded) {
		ConsoleMessage("Configuration: not confirmed yet.");
	} else {
		const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - lastConfigLoad).count();
		ConsoleMessage("Configuration: loaded %lld seconds ago.", static_cast<long long>(age));
	}
	ConsoleMessage("Players: %d connected (%d human, %d bots).", connected, humans, bots);
	ConsoleMessage("Detectors: %zu/%zu enabled.", settings::GetEnabledDetectionCount(), static_cast<std::size_t>(DetectionType::Count));
	ConsoleMessage("Enabled detectors: %s.", enabledNames.empty() ? "none" : enabledNames.c_str());
	ConsoleMessage("Whitelist: %zu valid entries, %zu rejected, %zu duplicates ignored during the last update.", settings::GetWhitelistCount(),
				   settings::GetRejectedWhitelistCount(), settings::GetDuplicateWhitelistCount());
	ConsoleMessage("Announcements: chat %s, center screen %s.", settings::ShowChatAnnouncements() ? "on" : "off",
				   settings::ShowCenterAnnouncements() ? "on" : "off");
	ConsoleMessage("Punishments: permanent ban %s, kick %s, weak findings %s.",
				   settings::GetPunishmentCommand() && *settings::GetPunishmentCommand() ? "configured" : "disabled",
				   settings::GetKickCommand() && *settings::GetKickCommand() ? "configured" : "disabled",
				   settings::WeakSignalsKick() ? "kick-only" : "report-only");
	ConsoleMessage("Automatic updates: %s (repository: %s; not ported).", settings::AutomaticUpdatesEnabled() ? "on" : "off",
				   *settings::GetUpdateRepository() ? settings::GetUpdateRepository() : "not configured");
	const std::size_t webhookQueueSize = webhook ? webhook->QueueSize() : 0;
	ConsoleMessage("Discord webhook: %s, %zu queued report%s.",
				   webhook && webhook->IsConfigured() ? (webhook->IsDisabled() ? "disabled after an error" : "configured") : "not configured",
				   webhookQueueSize, webhookQueueSize == 1 ? "" : "s");
	ConsoleMessage("sv_cheats testing: %s.", settings::AllowSvCheatsTesting() ? "allowed" : "not allowed");
	hooks::PrintReport();
}

// Diagnostics: the parsed usercmd fields of the most recent commands (used to confirm the
// protobuf reader against the shipping build; see PORT_PLAN.md §7).
void Plugin::PrintUsercmdDump()
{
	if (!lastUsercmdValid) {
		ConsoleMessage("No usercmd has been parsed yet (is the ProcessUsercmds hook installed? see cs2ac_hooks).");
		return;
	}
	ConsoleMessage("Last parsed usercmd: cmd %d, client tick %d (seen: %s), view angles %.2f %.2f %.2f (seen: %s), buttons 0x%llx/0x%llx, "
				   "mousedx/y %d/%d, subtick moves %zu, input history %zu, attack1 index %d.",
				   lastUsercmd.commandNumber, lastUsercmd.clientTick, lastUsercmd.hasClientTick ? "yes" : "no",
				   static_cast<double>(lastUsercmd.viewAngles.x), static_cast<double>(lastUsercmd.viewAngles.y),
				   static_cast<double>(lastUsercmd.viewAngles.z), lastUsercmd.hasViewAngles ? "yes" : "no",
				   static_cast<unsigned long long>(lastUsercmd.buttonState1), static_cast<unsigned long long>(lastUsercmd.buttonState2),
				   lastUsercmd.mouseDx, lastUsercmd.mouseDy, lastUsercmd.subtickMoves.size(), lastUsercmd.inputHistory.size(),
				   lastUsercmd.attack1StartHistoryIndex);
	ConsoleMessage("Tick integrity: consumed angle changes %s%u, history[0] render tick %s%d frac %s%.3f, sv_interp0 %s(%d/%d/%.3f), "
				   "player tick %s%d.",
				   lastUsercmd.hasConsumedServerAngleChanges ? "" : "missing/", lastUsercmd.consumedServerAngleChanges,
				   lastUsercmd.inputHistory.empty() || !lastUsercmd.inputHistory[0].hasRenderTickCount ? "missing/" : "",
				   lastUsercmd.inputHistory.empty() ? 0 : lastUsercmd.inputHistory[0].renderTickCount,
				   lastUsercmd.inputHistory.empty() || !lastUsercmd.inputHistory[0].hasRenderTickFraction ? "missing/" : "",
				   lastUsercmd.inputHistory.empty() ? 0.0 : static_cast<double>(lastUsercmd.inputHistory[0].renderTickFraction),
				   lastUsercmd.inputHistory.empty() || !lastUsercmd.inputHistory[0].hasSvInterp0 ? "missing" : "",
				   lastUsercmd.inputHistory.empty() ? 0 : lastUsercmd.inputHistory[0].svInterp0.srcTick,
				   lastUsercmd.inputHistory.empty() || !lastUsercmd.inputHistory[0].svInterp0.hasDstTick
					   ? 0
					   : lastUsercmd.inputHistory[0].svInterp0.dstTick,
				   lastUsercmd.inputHistory.empty() ? 0.0 : static_cast<double>(lastUsercmd.inputHistory[0].svInterp0.frac),
				   lastUsercmd.inputHistory.empty() || !lastUsercmd.inputHistory[0].hasPlayerTickCount ? "missing/" : "",
				   lastUsercmd.inputHistory.empty() ? 0 : lastUsercmd.inputHistory[0].playerTickCount);
}

void Plugin::NoteUsercmd(const UsercmdView& view)
{
	lastUsercmd = view;
	lastUsercmdValid = true;
}

} // namespace cs2ac

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

namespace {
class Cs2acPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override
	{
		cs2ac::g_plugin.OnStart();
		// Never Failed: that stops every other Plugify plugin on the server.
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds /*dt*/) override
	{
		cs2ac::g_plugin.OnUpdate();
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		cs2ac::g_plugin.OnEnd();
		return {};
	}
};

Cs2acPlugin g_cs2acPlugin;
} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_cs2acPlugin)
