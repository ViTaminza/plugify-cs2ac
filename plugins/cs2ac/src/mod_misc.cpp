// cs2ac — the remaining detectors: Namechanger and DLL Injection.
//
// Ported from ref/src/cs2ac-legacy/src/detection/{namechanger,dll_injection}_module.cpp.
//
// DLL Injection check: the original resolved IGameEventManager2 through a mov signature and asked
// FindListener() for each of 117 blacklisted events on the player's legacy listener. The listener
// pointer is still resolvable here (the GetLegacyGameEventListener signature is in gamedata), but
// the FindListener vtable index can only be verified against the live libserver.so, so the detector
// stays off until engine::LegacyListenerSubscribes() reports data (PORT_PLAN.md §3 #6 and §7.3).

#include "cs2ac.hpp"

namespace cs2ac {
namespace detection {

namespace {
bool NameChangerDebug() { return settings::DebugFlag("cs2ac_namechanger_debug"); }
bool DllInjectionDebug() { return settings::DebugFlag("cs2ac_dll_injection_debug"); }
} // namespace

// ---------------------------------------------------------------------------------------------
// Namechanger (namechanger_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr size_t nameChangerDetectionThreshold = 5;
constexpr auto nameChangerEvidenceWindow = std::chrono::seconds(60);
} // namespace

void NameChangerModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
	Reset();
}

void NameChangerModule::Unload()
{
	playerData = {};
	announce = nullptr;
}

void NameChangerModule::Reset()
{
	// State only. The original re-baselined every player's name here by asking the engine for each
	// player's current name, but this runs from OnMapEnd/OnMapStart - and at map shutdown s2sdk's
	// client list is already empty, so any client query (IsFakeClient/GetClientName/...) dereferences
	// a null entry (live crash: Reset -> OnClientReady -> IsEligibleHuman -> IsFakeClient ->
	// GetClientBySlot -> SIGSEGV at 0).
	//
	// Losing the eager baseline is harmless: NameChangerPlayerData::initialized is false after this
	// reset, and both entry points that follow re-baseline without counting a change -
	// OnClientReady (client join/activate) and the `!initialized` branch of OnClientSettingsChanged
	// (the first settings change after a map change). Deferring to those callbacks is exactly the
	// "re-initialise per slot from a client event, not from the map-end path" rule.
	playerData = {};
}

void NameChangerModule::OnClientReady(PlayerRef* player)
{
	if (!IsEligibleHuman(player) || !player->IsInGame()) {
		return;
	}
	auto& data = playerData[static_cast<size_t>(player->index)];
	data = {};
	const char* name = engine::PlayerName(player->index);
	if (name) {
		data.lastName = name;
		data.initialized = true;
		if (NameChangerDebug()) {
			ConsoleMessage("Namechanger: %s baseline stored for player slot %d.", name, player->index);
		}
	}
}

void NameChangerModule::OnClientSettingsChanged(PlayerRef* player)
{
	if (!IsEligibleHuman(player) || !player->IsInGame()) {
		return;
	}
	const char* currentName = engine::PlayerName(player->index);
	if (!currentName) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	if (!data.initialized) {
		data.lastName = currentName;
		data.initialized = true;
		return;
	}
	if (data.lastName == currentName) {
		return;
	}
	data.lastName = currentName;

	const auto now = Clock::now();
	size_t expired = 0;
	while (!data.changes.empty() && now - data.changes.front() >= nameChangerEvidenceWindow) {
		data.changes.pop_front();
		++expired;
	}
	data.changes.push_back(now);
	if (NameChangerDebug()) {
		ConsoleMessage("Namechanger: %s changed names: %zu of %zu changes remain in the rolling minute%s.", currentName, data.changes.size(),
					   nameChangerDetectionThreshold, expired ? FormatString("; %zu expired", expired).c_str() : "");
	}
	if (data.changes.size() >= nameChangerDetectionThreshold) {
		if (announce) {
			announce("NAMECHANGER", player,
					 localization::Format("evidence.namechanger", "The player changed their visible name {changes} times within one minute.",
										  {{"changes", FormatString("%zu", data.changes.size())}}));
		}
		data.changes.clear();
	}
}

void NameChangerModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
	}
}

// ---------------------------------------------------------------------------------------------
// DLL Injection (dll_injection_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr auto initialScanDelay = std::chrono::seconds(10);
constexpr auto scanInterval = std::chrono::minutes(2);

// Keep normal client and game-instructor subscriptions out of this list.
constexpr std::array<const char*, 117> blacklistedEvents = {
	"gameui_hidden",
	"player_chat",
	"player_score",
	"player_shoot",
	"game_init",
	"game_start",
	"game_end",
	"ugc_map_info_received",
	"ugc_map_unsubscribed",
	"ugc_map_download_error",
	"ugc_file_download_finished",
	"ugc_file_download_start",
	"dm_bonus_weapon_start",
	"survival_announce_phase",
	"reset_game_titledata",
	"vote_ended",
	"vote_started",
	"vote_options",
	"endmatch_mapvote_selecting_map",
	"endmatch_cmm_start_reveal_items",
	"inventory_updated",
	"client_loadout_changed",
	"add_player_sonar_icon",
	"door_open",
	"door_closed",
	"door_break",
	"bullet_damage",
	"hostage_call_for_help",
	"vip_escaped",
	"vip_killed",
	"silencer_detach",
	"inspect_weapon",
	"player_spawned",
	"item_pickup_slerp",
	"item_pickup_failed",
	"enter_rescue_zone",
	"exit_rescue_zone",
	"silencer_off",
	"silencer_on",
	"round_poststart",
	"tagrenade_detonate",
	"inferno_extinguish",
	"mb_input_lock_success",
	"mb_input_lock_cancel",
	"nav_generate",
	"achievement_info_loaded",
	"hltv_changed_mode",
	"show_deathpanel",
	"hide_deathpanel",
	"player_avenged_teammate",
	"achievement_earned_local",
	"repost_xbox_achievements",
	"match_end_conditions",
	"write_profile_data",
	"trial_time_expired",
	"update_matchmaking_stats",
	"enable_restart_voting",
	"sfuievent",
	"start_vote",
	"teamchange_pending",
	"material_default_complete",
	"cs_prev_next_spectator",
	"tournament_reward",
	"start_halftime",
	"ammo_refill",
	"parachute_pickup",
	"parachute_deploy",
	"dronegun_attack",
	"drone_dispatched",
	"loot_crate_visible",
	"loot_crate_opened",
	"open_crate_instr",
	"smoke_beacon_paradrop",
	"drone_cargo_detached",
	"drone_above_roof",
	"dz_item_interaction",
	"survival_teammate_respawn",
	"guardian_wave_restart",
	"bullet_flight_resolution",
	"server_message",
	"player_full_update",
	"local_player_controller_team",
	"local_player_pawn_changed",
	"ragdoll_dissolved",
	"team_info",
	"team_score",
	"hltv_rank_camera",
	"hltv_rank_entity",
	"demo_stop",
	"map_shutdown",
	"hostname_changed",
	"game_message",
	"round_start_pre_entity",
	"round_start_post_nav",
	"player_hintmessage",
	"broken_breakable",
	"door_close",
	"vote_failed",
	"vote_passed",
	"vote_cast_yes",
	"vote_cast_no",
	"achievement_event",
	"achievement_write_failed",
	"bonus_updated",
	"flare_ignite_npc",
	"helicopter_grenade_punt_miss",
	"physgun_pickup",
	"cart_updated",
	"store_pricesheet_updated",
	"item_schema_initialized",
	"drop_rate_modified",
	"event_ticket_modified",
	"gc_connected",
	"instructor_start_lesson",
	"instructor_close_lesson",
	"clientside_lesson_closed",
	"dynamic_shadow_light_changed",
};

static_assert(blacklistedEvents.size() == 117, "The DLL Injection event list must contain 117 entries.");
} // namespace

void DllInjectionModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
	Reset();
}

void DllInjectionModule::Unload()
{
	nextScans = {};
	announce = nullptr;
}

void DllInjectionModule::Reset()
{
	nextScans = {};
}

void DllInjectionModule::OnGameFrame()
{
	auto now = std::chrono::steady_clock::now();
	if (!announce || !g_pPlayerManager) {
		return;
	}

	bool scannedPlayerThisFrame = false;
	for (u32 index = 1; index <= MAXPLAYERS; ++index) {
		auto* player = g_pPlayerManager->ToPlayer(static_cast<i32>(index));
		if (!player || !player->IsConnected() || !player->IsInGame() || player->IsFakeClient() || player->IsCSTV()) {
			continue;
		}

		const int slot = player->GetPlayerSlot();
		if (slot < 0 || slot >= MAXPLAYERS) {
			continue;
		}

		auto& nextScan = nextScans[static_cast<size_t>(slot)];
		if (nextScan == Clock::time_point {}) {
			nextScan = now + initialScanDelay;
			if (DllInjectionDebug()) {
				ConsoleMessage("DLL Injection: %s will receive the first subscription scan in 10 seconds.", player->GetName());
			}
			continue;
		}
		if (now < nextScan) {
			continue;
		}
		if (scannedPlayerThisFrame) {
			continue;
		}
		scannedPlayerThisFrame = true;
		nextScan = now + scanInterval;

		std::array<bool, blacklistedEvents.size()> current {};
		size_t found = 0;
		bool available = true;
		for (size_t eventIndex = 0; eventIndex < blacklistedEvents.size(); ++eventIndex) {
			bool subscribed = false;
			if (!engine::LegacyListenerSubscribes(player->index, blacklistedEvents[eventIndex], subscribed)) {
				available = false;
				break;
			}
			current[eventIndex] = subscribed;
			found += subscribed ? 1 : 0;
		}
		if (!available) {
			// The vtable index is not verified for this build (see the file header): stay off, once.
			static bool noted = false;
			if (!noted) {
				noted = true;
				LogWarning("DLL Injection is disabled: the IGameEventManager2::FindListener vtable index is not verified in gamedata for this "
						   "server build (see PORT_PLAN.md).");
			}
			return;
		}
		if (DllInjectionDebug()) {
			ConsoleMessage("DLL Injection: %s scan completed: %zu of %zu blacklisted subscriptions found; next scan in 2 minutes.", player->GetName(),
						   found, blacklistedEvents.size());
		}

		if (found == 0) {
			continue;
		}

		std::string matches;
		for (size_t eventIndex = 0; eventIndex < blacklistedEvents.size(); ++eventIndex) {
			if (!current[eventIndex]) {
				continue;
			}
			if (!matches.empty()) {
				matches += ", ";
			}
			matches += blacklistedEvents[eventIndex];
			if (matches.size() > 700) {
				matches += ", ...";
				break;
			}
		}
		announce("DLL INJECTION", player,
				 localization::Format(found == 1 ? "evidence.dll_injection.one" : "evidence.dll_injection.many",
									  found == 1 ? "The player's game was listening for {count} event associated with injected cheats: {events}. This "
												   "detector checks visible event subscriptions only; it does not scan the player's memory or prove "
												   "every possible DLL injection."
												 : "The player's game was listening for {count} events associated with injected cheats: {events}. This "
												   "detector checks visible event subscriptions only; it does not scan the player's memory or prove "
												   "every possible DLL injection.",
									  {{"count", FormatString("%zu", found)}, {"events", matches}}));
	}
}

void DllInjectionModule::OnClientDisconnect(PlayerRef* player)
{
	if (!player) {
		return;
	}
	const int slot = player->GetPlayerSlot();
	if (slot >= 0 && slot < MAXPLAYERS) {
		nextScans[static_cast<size_t>(slot)] = {};
	}
}

} // namespace detection
} // namespace cs2ac
