#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: punishments)

namespace admin_system {

  using _IsPlayerPunished = bool (*)(int32_t, int32_t);
}
extern "C" PLUGIN_API admin_system::_IsPlayerPunished __admin_system_IsPlayerPunished;
namespace admin_system {
  /**
   * @brief True while the slot carries an active punishment of that type. Note that the original answered true before the punishments of a player had been loaded (the array started at 0); here every slot starts at 'none', which the ported modules (VoteBKM) rely on.
   * @param slot (int32): Player slot.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @return bool: Whether the punishment is active.
   */
  inline bool IsPlayerPunished(int32_t slot, int32_t type) {
    return __admin_system_IsPlayerPunished(slot, type);
  }

  using _GetPlayerPunishmentExpired = int64_t (*)(int32_t, int32_t);
}
extern "C" PLUGIN_API admin_system::_GetPlayerPunishmentExpired __admin_system_GetPlayerPunishmentExpired;
namespace admin_system {
  /**
   * @brief Expiry of the active punishment of a slot: absolute unix timestamp, 0 = permanent, -1 = no punishment. The original returned an int and stored the timestamp in a 32 bit array; the value is widened to int64 here so the 2038 wraparound of the original cannot happen.
   * @param slot (int32): Player slot.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @return int64: Expiry timestamp, 0 or -1.
   */
  inline int64_t GetPlayerPunishmentExpired(int32_t slot, int32_t type) {
    return __admin_system_GetPlayerPunishmentExpired(slot, type);
  }

  using _GetPlayerPunishmentReason = plg::string (*)(int32_t, int32_t);
}
extern "C" PLUGIN_API admin_system::_GetPlayerPunishmentReason __admin_system_GetPlayerPunishmentReason;
namespace admin_system {
  /**
   * @brief Reason of the active punishment of a slot, empty when there is none. Used for the 'you are muted for X (reason)' chat message.
   * @param slot (int32): Player slot.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @return string: Punishment reason.
   */
  inline plg::string GetPlayerPunishmentReason(int32_t slot, int32_t type) {
    return __admin_system_GetPlayerPunishmentReason(slot, type);
  }

  using _AddPlayerPunishment = void (*)(int32_t, int32_t, int32_t, const plg::string&, int32_t, bool, bool);
}
extern "C" PLUGIN_API admin_system::_AddPlayerPunishment __admin_system_AddPlayerPunishment;
namespace admin_system {
  /**
   * @brief Punishes a connected player: inserts the as_punishments row (name, steamid, ip when punish_ip is on, admin_id, created, expires, reason, server_id, punish_type), updates the in-memory state and notifies the OnPlayerPunish listeners. A ban additionally asks the core to disconnect the player after ban_delay seconds. time is a duration in seconds, 0 = permanent. adminID is the slot of the punishing admin, -1 = console.
   * @param slot (int32): Slot of the punished player.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @param time (int32): Duration in seconds, 0 = permanent.
   * @param reason (string): Punishment reason.
   * @param adminID (int32): Slot of the admin, -1 = console.
   * @param notify (bool): false = send no chat message (callers that print their own).
   * @param db (bool): false = punish in memory only.
   */
  inline void AddPlayerPunishment(int32_t slot, int32_t type, int32_t time, const plg::string& reason, int32_t adminID, bool notify, bool db) {
    return __admin_system_AddPlayerPunishment(slot, type, time, reason, adminID, notify, db);
  }

  using _AddOfflinePlayerPunishment = void (*)(const plg::string&, const plg::string&, int32_t, int32_t, const plg::string&, int32_t);
}
extern "C" PLUGIN_API admin_system::_AddOfflinePlayerPunishment __admin_system_AddOfflinePlayerPunishment;
namespace admin_system {
  /**
   * @brief Punishes a player that is not connected: inserts the as_punishments row (without an ip) and notifies the OnOfflinePlayerPunish listeners. Immunity is respected first - the target's immunity is read from the database, and when it is high enough (immunity_type 1/2/3) nothing happens.
   * @param steamid64 (string): SteamID64 of the target.
   * @param name (string): Last known name of the target.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @param time (int32): Duration in seconds, 0 = permanent.
   * @param reason (string): Punishment reason.
   * @param adminID (int32): Slot of the admin, -1 = console.
   */
  inline void AddOfflinePlayerPunishment(const plg::string& steamid64, const plg::string& name, int32_t type, int32_t time, const plg::string& reason, int32_t adminID) {
    return __admin_system_AddOfflinePlayerPunishment(steamid64, name, type, time, reason, adminID);
  }

  using _RemovePlayerPunishment = void (*)(int32_t, int32_t, int32_t, bool);
}
extern "C" PLUGIN_API admin_system::_RemovePlayerPunishment __admin_system_RemovePlayerPunishment;
namespace admin_system {
  /**
   * @brief Lifts the active punishment of a connected player: sets unpunish_admin_id on the matching as_punishments rows, clears the in-memory state and notifies the OnPlayerUnpunish listeners. With unpunish_type 0 an admin may only lift punishments they issued themselves, unless they have '@admin/unpunish_all' (or are the console).
   * @param slot (int32): Slot of the punished player.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @param adminID (int32): Slot of the admin, -1 = console.
   * @param notify (bool): false = send no chat message.
   */
  inline void RemovePlayerPunishment(int32_t slot, int32_t type, int32_t adminID, bool notify) {
    return __admin_system_RemovePlayerPunishment(slot, type, adminID, notify);
  }

  using _RemoveOfflinePlayerPunishment = void (*)(const plg::string&, int32_t, int32_t);
}
extern "C" PLUGIN_API admin_system::_RemoveOfflinePlayerPunishment __admin_system_RemoveOfflinePlayerPunishment;
namespace admin_system {
  /**
   * @brief Lifts the punishment of a player that is not connected. When unpunish_type is 0 and the caller is neither the console nor the holder of '@admin/unpunish_all', only rows whose admin_id is the caller are touched (the original added 'AND admin_id = <caller>').
   * @param steamid64 (string): SteamID64 of the target.
   * @param type (int32): 0 = ban, 1 = mute, 2 = gag, 3 = silence.
   * @param adminID (int32): Slot of the admin, -1 = console.
   */
  inline void RemoveOfflinePlayerPunishment(const plg::string& steamid64, int32_t type, int32_t adminID) {
    return __admin_system_RemoveOfflinePlayerPunishment(steamid64, type, adminID);
  }

} // namespace admin_system
