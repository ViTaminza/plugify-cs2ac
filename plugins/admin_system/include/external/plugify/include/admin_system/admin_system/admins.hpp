#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: admins)

namespace admin_system {

  using _HasPermission = bool (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_HasPermission __admin_system_HasPermission;
namespace admin_system {
  /**
   * @brief Permission check with the semantics of the original: slot -1 (console) is always true, permissions listed under 'default_permissions' in core.ini are true for everyone, '@admin/root' grants everything, and the flags loaded from the database are expanded through the 'flags' block of core.ini.
   * @param slot (int32): Player slot (-1 = console).
   * @param permission (string): Permission line, e.g. '@admin/ban'.
   * @return bool: Whether the admin has the permission.
   */
  inline bool HasPermission(int32_t slot, const plg::string& permission) {
    return __admin_system_HasPermission(slot, permission);
  }

  using _HasFlag = bool (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_HasFlag __admin_system_HasFlag;
namespace admin_system {
  /**
   * @brief Raw flag check (no core.ini expansion, no root): exactly g_pAdmins[slot].vFlags of the original.
   * @param slot (int32): Player slot (-1 = console).
   * @param flag (string): Single flag character, e.g. 'b'.
   * @return bool: Whether the admin carries the flag.
   */
  inline bool HasFlag(int32_t slot, const plg::string& flag) {
    return __admin_system_HasFlag(slot, flag);
  }

  using _IsAdmin = bool (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_IsAdmin __admin_system_IsAdmin;
namespace admin_system {
  /**
   * @brief True when the loaded admin record has at least one flag. A player whose admins_servers row has an empty flags column is not an admin - same as the original.
   * @param slot (int32): Player slot (-1 = console).
   * @return bool: Whether the slot is an admin.
   */
  inline bool IsAdmin(int32_t slot) {
    return __admin_system_IsAdmin(slot);
  }

  using _GetFlagName = plg::string (*)(const plg::string&);
}
extern "C" PLUGIN_API admin_system::_GetFlagName __admin_system_GetFlagName;
namespace admin_system {
  /**
   * @brief Human readable name of a flag from the 'flags' block of core.ini (its 'name' key). Empty string for an unknown flag.
   * @param flag (string): Single flag character.
   * @return string: Flag name or an empty string.
   */
  inline plg::string GetFlagName(const plg::string& flag) {
    return __admin_system_GetFlagName(flag);
  }

  using _GetPermissionsByFlag = plg::vector<plg::string> (*)(const plg::string&);
}
extern "C" PLUGIN_API admin_system::_GetPermissionsByFlag __admin_system_GetPermissionsByFlag;
namespace admin_system {
  /**
   * @brief Expands one flag into the permissions listed under its 'access' block in core.ini. Empty for an unknown flag.
   * @param flag (string): Single flag character.
   * @return string[]: Permissions granted by the flag.
   */
  inline plg::vector<plg::string> GetPermissionsByFlag(const plg::string& flag) {
    return __admin_system_GetPermissionsByFlag(flag);
  }

  using _GetAdminFlags = plg::vector<plg::string> (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminFlags __admin_system_GetAdminFlags;
namespace admin_system {
  /**
   * @brief Flags of a loaded admin (database flags plus group flags, deduplicated across servers in load order). Empty when the slot is not an admin.
   * @param slot (int32): Player slot.
   * @return string[]: Flag characters.
   */
  inline plg::vector<plg::string> GetAdminFlags(int32_t slot) {
    return __admin_system_GetAdminFlags(slot);
  }

  using _GetAdminPermissions = plg::vector<plg::string> (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminPermissions __admin_system_GetAdminPermissions;
namespace admin_system {
  /**
   * @brief Permissions of a loaded admin, i.e. the union of GetPermissionsByFlag() over its flags. '@admin/root' is included when the admin has the z flag.
   * @param slot (int32): Player slot.
   * @return string[]: Permission lines.
   */
  inline plg::vector<plg::string> GetAdminPermissions(int32_t slot) {
    return __admin_system_GetAdminPermissions(slot);
  }

  using _GetAdminImmunity = int32_t (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminImmunity __admin_system_GetAdminImmunity;
namespace admin_system {
  /**
   * @brief Immunity of a loaded admin: the maximum of the admins_servers.immunity and groups.immunity values over the servers that matched admin_server_id.
   * @param slot (int32): Player slot.
   * @return int32: Immunity value (0 when not an admin).
   */
  inline int32_t GetAdminImmunity(int32_t slot) {
    return __admin_system_GetAdminImmunity(slot);
  }

  using _GetAdminExpireTime = int64_t (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminExpireTime __admin_system_GetAdminExpireTime;
namespace admin_system {
  /**
   * @brief Absolute unix timestamp at which the admin loses the rights of this server, 0 = permanent, -1 = no row loaded. The original returned an int; the value is widened to int64 here because the timestamp is stored as a 32 bit unix time that overflows in 2038.
   * @param slot (int32): Player slot.
   * @return int64: Expiry timestamp, 0 (never) or -1 (none).
   */
  inline int64_t GetAdminExpireTime(int32_t slot) {
    return __admin_system_GetAdminExpireTime(slot);
  }

  using _GetAdminName = plg::string (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminName __admin_system_GetAdminName;
namespace admin_system {
  /**
   * @brief Name of a loaded admin: the name column of as_admins, or the player name when the column is NULL and static_name is disabled. 'Console' for slot -1, empty string for an invalid slot.
   * @param slot (int32): Player slot.
   * @return string: Admin name.
   */
  inline plg::string GetAdminName(int32_t slot) {
    return __admin_system_GetAdminName(slot);
  }

  using _GetAdminGroupID = int32_t (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminGroupID __admin_system_GetAdminGroupID;
namespace admin_system {
  /**
   * @brief Database id of the group of a loaded admin, -1 when the admin has no group or is not an admin.
   * @param slot (int32): Player slot.
   * @return int32: Group id or -1.
   */
  inline int32_t GetAdminGroupID(int32_t slot) {
    return __admin_system_GetAdminGroupID(slot);
  }

  using _GetAdminGroupName = plg::string (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_GetAdminGroupName __admin_system_GetAdminGroupName;
namespace admin_system {
  /**
   * @brief Name of the group of a loaded admin, empty when there is none.
   * @param slot (int32): Player slot.
   * @return string: Group name.
   */
  inline plg::string GetAdminGroupName(int32_t slot) {
    return __admin_system_GetAdminGroupName(slot);
  }

  using _GetImmunityType = int32_t (*)();
}
extern "C" PLUGIN_API admin_system::_GetImmunityType __admin_system_GetImmunityType;
namespace admin_system {
  /**
   * @brief Value of the 'immunity_type' key of core.ini (0 = immunity ignored, 1 = equal immunity may punish, 2 = strictly higher immunity needed, 3 = admins cannot punish each other). Kept for parity; the core applies the rules itself in TryAddPunishment.
   * @return int32: immunity_type from core.ini.
   */
  inline int32_t GetImmunityType() {
    return __admin_system_GetImmunityType();
  }

  using _AddPlayerAdmin = void (*)(const plg::string&, const plg::string&, const plg::string&, int32_t, int32_t, int32_t, const plg::string&, bool);
}
extern "C" PLUGIN_API admin_system::_AddPlayerAdmin __admin_system_AddPlayerAdmin;
namespace admin_system {
  /**
   * @brief Creates (or extends) an admin in as_admins / as_admins_servers for the current server, with the SQL of the original: an existing as_admins row is reused, the admins_servers row is INSERT IGNORE'd (so an existing row for this server is kept), and the rights of an online player are reloaded afterwards. expireTime is a duration in seconds from now, 0 = permanent. When db is false nothing is written and only the in-memory record of an online player changes.
   * @param name (string): Steam name stored in as_admins.name.
   * @param steamid64 (string): SteamID64 of the admin.
   * @param flags (string): Flag characters, e.g. 'abz'.
   * @param immunity (int32): Immunity of the admins_servers row.
   * @param expireTime (int32): Lifetime in seconds from now, 0 = permanent.
   * @param groupID (int32): as_groups.id, 0 = no group.
   * @param comment (string): Comment for a newly created as_admins row.
   * @param db (bool): false = change the in-memory record only.
   */
  inline void AddPlayerAdmin(const plg::string& name, const plg::string& steamid64, const plg::string& flags, int32_t immunity, int32_t expireTime, int32_t groupID, const plg::string& comment, bool db) {
    return __admin_system_AddPlayerAdmin(name, steamid64, flags, immunity, expireTime, groupID, comment, db);
  }

  using _RemovePlayerAdmin = void (*)(const plg::string&, bool);
}
extern "C" PLUGIN_API admin_system::_RemovePlayerAdmin __admin_system_RemovePlayerAdmin;
namespace admin_system {
  /**
   * @brief Removes the admin rights of a steamid: deletes the admins_servers row for this server and, when no other server uses the admin, the as_admins row as well. Online players with that steamid lose their flags immediately.
   * @param steamid64 (string): SteamID64 of the admin.
   * @param db (bool): false = change the in-memory record only.
   */
  inline void RemovePlayerAdmin(const plg::string& steamid64, bool db) {
    return __admin_system_RemovePlayerAdmin(steamid64, db);
  }

  using _AddPlayerLocalFlag = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_AddPlayerLocalFlag __admin_system_AddPlayerLocalFlag;
namespace admin_system {
  /**
   * @brief Appends a flag to the in-memory record of a slot without touching the database. The permission list is NOT recomputed (this is what the original did), so HasPermission() does not see the new flag until the next load - use AddPlayerLocalPermission for permissions. Kept for parity; VIP_Permissions is such a user.
   * @param slot (int32): Player slot.
   * @param flag (string): Flag character.
   */
  inline void AddPlayerLocalFlag(int32_t slot, const plg::string& flag) {
    return __admin_system_AddPlayerLocalFlag(slot, flag);
  }

  using _RemovePlayerLocalFlag = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_RemovePlayerLocalFlag __admin_system_RemovePlayerLocalFlag;
namespace admin_system {
  /**
   * @brief Removes every occurrence of a flag from the in-memory record of a slot.
   * @param slot (int32): Player slot.
   * @param flag (string): Flag character.
   */
  inline void RemovePlayerLocalFlag(int32_t slot, const plg::string& flag) {
    return __admin_system_RemovePlayerLocalFlag(slot, flag);
  }

  using _AddPlayerLocalPermission = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_AddPlayerLocalPermission __admin_system_AddPlayerLocalPermission;
namespace admin_system {
  /**
   * @brief Appends a permission line to the in-memory record of a slot without touching the database - the way VIP_Permissions makes a player an admin for one round.
   * @param slot (int32): Player slot.
   * @param permission (string): Permission line, e.g. '@admin/vip'.
   */
  inline void AddPlayerLocalPermission(int32_t slot, const plg::string& permission) {
    return __admin_system_AddPlayerLocalPermission(slot, permission);
  }

  using _RemovePlayerLocalPermission = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_RemovePlayerLocalPermission __admin_system_RemovePlayerLocalPermission;
namespace admin_system {
  /**
   * @brief Removes every occurrence of a permission line from the in-memory record of a slot.
   * @param slot (int32): Player slot.
   * @param permission (string): Permission line.
   */
  inline void RemovePlayerLocalPermission(int32_t slot, const plg::string& permission) {
    return __admin_system_RemovePlayerLocalPermission(slot, permission);
  }

  using _AddPlayerLocalImmunity = void (*)(int32_t, int32_t);
}
extern "C" PLUGIN_API admin_system::_AddPlayerLocalImmunity __admin_system_AddPlayerLocalImmunity;
namespace admin_system {
  /**
   * @brief Overwrites the in-memory immunity of a slot (the original assignment, not a maximum).
   * @param slot (int32): Player slot.
   * @param immunity (int32): New immunity value.
   */
  inline void AddPlayerLocalImmunity(int32_t slot, int32_t immunity) {
    return __admin_system_AddPlayerLocalImmunity(slot, immunity);
  }

  using _RemovePlayerLocalImmunity = void (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_RemovePlayerLocalImmunity __admin_system_RemovePlayerLocalImmunity;
namespace admin_system {
  /**
   * @brief Resets the in-memory immunity of a slot to 0.
   * @param slot (int32): Player slot.
   */
  inline void RemovePlayerLocalImmunity(int32_t slot) {
    return __admin_system_RemovePlayerLocalImmunity(slot);
  }

} // namespace admin_system
