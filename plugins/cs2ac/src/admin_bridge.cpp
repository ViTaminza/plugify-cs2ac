// admin_bridge.cpp — the fork's admin_bridge on the Plugify admin_system API.
//
// The fork resolved IAdminApi through MetaFactory on every use because admin_system can be
// reloaded independently. The Plugify binding slots behave the same way (the module fills them
// when admin_system is loaded), so the wrapper is asked on every use and never cached.

#include "admin_bridge.h"

#include "cs2ac.hpp"

#include <admin_api.hpp>

#include <string>
#include <utility>
#include <vector>

namespace cs2ac::admin_bridge {

namespace {

struct PendingBan {
	int slot {-1};
	std::uint64_t steamId {};
	std::string name;
	int seconds {};
	std::string reason;
	std::string fallbackCommand;
	bool fallbackRequiresSlot {};
};

std::vector<PendingBan> g_pending;

admin_system::AdminApi* Api()
{
	auto& api = admin_system::AdminApi::Get();
	return api.IsCoreLoaded() ? &api : nullptr;
}

bool SameClientStillOnline(const PendingBan& ban)
{
	if (!engine::GameReady() || ban.slot < 0 || ban.slot >= MAXPLAYERS) {
		return false;
	}
	const int index = ban.slot + 1;
	return engine::PlayerConnected(index) && engine::PlayerSteamId(index) == ban.steamId;
}

void RunFallback(const PendingBan& ban)
{
	if (ban.fallbackCommand.empty() || !engine::BindingsAvailable()) {
		LogWarning("admin_system is unavailable and no fallback command is set; ban for %llu was NOT issued.",
				   static_cast<unsigned long long>(ban.steamId));
		return;
	}
	if (ban.fallbackRequiresSlot && !SameClientStillOnline(ban)) {
		LogWarning("admin_system is unavailable; dropped fallback ban for %llu because the slot was reused.",
				   static_cast<unsigned long long>(ban.steamId));
		return;
	}
	std::string command = ban.fallbackCommand;
	if (ban.fallbackRequiresSlot) {
		const int userId = engine::PlayerUserId(ban.slot + 1);
		if (userId < 0) {
			LogWarning("admin_system is unavailable; dropped fallback ban for %llu because the userid is not ready.",
					   static_cast<unsigned long long>(ban.steamId));
			return;
		}
		ReplaceAll(command, "{userid}", std::to_string(userId));
	}
	ReplaceAll(command, "{steamid64}", std::to_string(ban.steamId));
	command += "\n";
	engine::ServerCommand(command.c_str());
	Log("admin_system unavailable, fallback command sent for %llu.", static_cast<unsigned long long>(ban.steamId));
}

void Execute(admin_system::AdminApi* api, const PendingBan& ban, bool forceOffline)
{
	if (!api) {
		RunFallback(ban);
		return;
	}
	// iAdminID = -1 means "Console". AddPlayerPunishment goes straight to the DB insert and kick;
	// it does not run admin_system's immunity check, so the CS2AC whitelist is the only exemption.
	if (!forceOffline && SameClientStillOnline(ban)) {
		api->AddPlayerPunishment(ban.slot, admin_system::RT_BAN, ban.seconds, ban.reason.c_str(), -1, true, true);
		Log("Ban sent to admin_system for slot %d (%llu).", ban.slot, static_cast<unsigned long long>(ban.steamId));
	} else {
		const std::string steamId = std::to_string(ban.steamId);
		api->AddOfflinePlayerPunishment(steamId.c_str(), ban.name.c_str(), admin_system::RT_BAN, ban.seconds, ban.reason.c_str(), -1);
		Log("Offline ban sent to admin_system for %s.", steamId.c_str());
	}
}

} // namespace

bool IsAvailable()
{
	return Api() != nullptr;
}

void QueueBan(int slot, std::uint64_t steamId64, const std::string& name, int seconds, const std::string& reason,
			  const std::string& fallbackCommand, bool fallbackRequiresSlot)
{
	if (!steamId64 || slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	for (const PendingBan& pending : g_pending) {
		if (pending.steamId == steamId64) {
			return; // one ban per player is enough
		}
	}
	g_pending.push_back({slot, steamId64, name, seconds < 0 ? 0 : seconds, reason, fallbackCommand, fallbackRequiresSlot});
}

void ProcessQueue()
{
	if (g_pending.empty()) {
		return;
	}
	// Swap first: AddPlayerPunishment fires disconnect callbacks that may reach back into CS2AC and
	// queue something new.
	std::vector<PendingBan> batch;
	batch.swap(g_pending);
	admin_system::AdminApi* api = Api();
	for (const PendingBan& ban : batch) {
		Execute(api, ban, false);
	}
}

void FlushOffline()
{
	if (g_pending.empty()) {
		return;
	}
	std::vector<PendingBan> batch;
	batch.swap(g_pending);
	admin_system::AdminApi* api = Api();
	for (const PendingBan& ban : batch) {
		Execute(api, ban, true);
	}
}

void Reset()
{
	g_pending.clear();
}

} // namespace cs2ac::admin_bridge
