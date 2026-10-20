// Bridge to our Plugify admin_system's exported API (the fork's Metamod IAdminApi bridge).
// Bans are queued and executed outside any per-client hook: admin_system's AddPlayerPunishment
// calls DisconnectClient synchronously, and a detection may fire from inside a usercmd/cvar hook
// that is processing this very client.
//
// Both operations the fork needs are exported by plugins/admin_system:
//   AddPlayerPunishment        (online: DB insert + kick)
//   AddOfflinePlayerPunishment (DB only; the player is rejected on reconnect)

#pragma once

#include <cstdint>
#include <string>

namespace cs2ac::admin_bridge {

// True when admin_system is loaded and its core (DB, config) is ready.
bool IsAvailable();

// seconds == 0 means permanent. fallbackCommand runs via ServerCommand if admin_system disappears
// before the queue is processed (may be empty). Slot is the engine slot (0..63).
void QueueBan(int slot, std::uint64_t steamId64, const std::string& name, int seconds, const std::string& reason,
			  const std::string& fallbackCommand, bool fallbackRequiresSlot);

// Call once per server frame, outside any per-client hook.
void ProcessQueue();

// Map change / unload: never drop a pending ban. Bans are written as offline bans (DB only, no
// kick); admin_system rejects the player on reconnect.
void FlushOffline();

// State cleanup used by the plugin's unload path.
void Reset();

} // namespace cs2ac::admin_bridge
