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

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cs2ac {

Plugin g_plugin;

namespace {

constexpr auto configLoadTimeout = std::chrono::seconds(5);

bool IsKickOnlyDetection(const char* detection)
{
	return EqualsIgnoreCase(detection, "DESUBTICKING") || EqualsIgnoreCase(detection, "NULLS") || EqualsIgnoreCase(detection, "SUBTICK SPAM");
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
		convarsRegistered = true;
		if (registered != settings::kConVarCount + settings::kDebugConVarCount) {
			LogWarning("Only %zu of %zu cs2ac convars could be registered; the missing ones keep their built-in defaults (run cs2ac_check_config).",
					   registered, settings::kConVarCount + settings::kDebugConVarCount);
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
	if (configReloadPending) {
		configReloadPending = false;
		configLoadFailed = true;
		ConsoleMessage("The configuration load was interrupted by a map reset. Run cs2ac_reload after the new map is ready.");
	}
	simulatingPhysics = false;
	announce::ResetDetectionAnnouncement();
	detectionSystem.Reset();
	punishmentLevels.fill(PunishmentLevel::None);
	joinWatermarks.fill({});
	lastCvarCycle = {};
}

void Plugin::CleanupRuntime()
{
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
		for (int index = 1; index <= MAXPLAYERS; ++index) {
			auto* player = g_pPlayerManager->ToPlayer(index);
			if (!player || !player->IsConnected() || !player->IsInGame() || player->IsFakeClient() || player->IsCSTV()) {
				continue;
			}
			auto& timeInServer = playerTime[index];
			const f64 delta = now - lastFrameTime;
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
	detectionSystem.OnClientReady(player);
	if (player && !player->IsFakeClient() && !player->IsCSTV() && !joinWatermarks[static_cast<size_t>(index)].shown
		&& !joinWatermarks[static_cast<size_t>(index)].pending) {
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
		settings::SetString(name.c_str(), value.c_str());
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
	auto finish = [&](announce::DetectionOutcome outcome) {
		announce::AnnounceDetection(detection, player->GetName(), outcome);
		if (webhook) {
			webhook->Report(detection, player->index, evidence.localized, outcome);
		}
	};
	if (steamId) {
		ConsoleMessage("Detected %s on %s (SteamID64 %llu).", detection, playerName.c_str(), static_cast<unsigned long long>(steamId));
	} else {
		ConsoleMessage("Detected %s on %s (SteamID64 unavailable).", detection, playerName.c_str());
	}
	if (!evidence.english.empty()) {
		ConsoleMessage("Evidence: %s", SanitizeConsoleText(evidence.english.c_str()).c_str());
	}
	if (networkVetoed) {
		finish(announce::DetectionOutcome::NetworkUnstable);
		ConsoleMessage("No punishment was sent because %s's connection exceeded the safe network limits.", playerName.c_str());
		return;
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

	const PunishmentLevel requested = kickOnly || IsKickOnlyDetection(detection) ? PunishmentLevel::Kick : PunishmentLevel::Ban;
	auto& issued = punishmentLevels[static_cast<size_t>(player->index)];
	if (issued >= requested) {
		finish(announce::DetectionOutcome::AlreadyPunished);
		ConsoleMessage("No duplicate punishment was sent for %s.", playerName.c_str());
		return;
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
	ReplaceAll(command, "{detection}", detection);
	// CCommand::MaxCommandLength() of the engine.
	constexpr std::size_t kMaxCommandLength = 512;
	if (command.size() >= kMaxCommandLength) {
		finish(announce::DetectionOutcome::CommandTooLong);
		ConsoleMessage("No punishment was sent because the configured command is too long.");
		return;
	}
	finish(announce::DetectionOutcome::PunishmentSent);
	command.push_back('\n');
	engine::ServerCommand(command.c_str());
	command.pop_back();
	ConsoleMessage("Sent: %s", SanitizeConsoleText(command.c_str()).c_str());
	issued = requested;
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
	findings += CheckCommandTemplate("cs2ac_kick_command", settings::GetKickCommand(), false);
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
	ConsoleMessage("Punishments: permanent ban %s, kick %s.",
				   settings::GetPunishmentCommand() && *settings::GetPunishmentCommand() ? "configured" : "disabled",
				   settings::GetKickCommand() && *settings::GetKickCommand() ? "configured" : "disabled");
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
