#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: core)

namespace admin_system {

  using _GetPluginVersion = float (*)();
}
extern "C" PLUGIN_API admin_system::_GetPluginVersion __admin_system_GetPluginVersion;
namespace admin_system {
  /**
   * @brief Version of the admin_system plugin as a number (1.0.0 -> 1.0). The original returned the Metamod version string parsed with atof; modules only use it for display, so the same value is kept.
   * @return float: Plugin version.
   */
  inline float GetPluginVersion() {
    return __admin_system_GetPluginVersion();
  }

  using _GetTranslation = plg::string (*)(const plg::string&);
}
extern "C" PLUGIN_API admin_system::_GetTranslation __admin_system_GetTranslation;
namespace admin_system {
  /**
   * @brief Phrase from admin_system.phrases.txt (the file the original loaded into its g_vecPhrases map). When the key is unknown the key itself is returned, exactly like the original. Translations are reloaded by the config reload command, so a plugify plugin should not cache the value across a reload.
   * @param key (string): Phrase key.
   * @return string: Translated phrase, or the key when it is missing.
   */
  inline plg::string GetTranslation(const plg::string& key) {
    return __admin_system_GetTranslation(key);
  }

  using _GetDatabaseConfig = plg::vector<plg::string> (*)();
}
extern "C" PLUGIN_API admin_system::_GetDatabaseConfig __admin_system_GetDatabaseConfig;
namespace admin_system {
  /**
   * @brief Replacement for the original GetMySQLConnection(): instead of handing out a connection object the connection settings are exported so every plugin opens its own plgdb connection. Returns exactly 7 strings in this order: 0 driver ('mysql' or 'sqlite'), 1 host, 2 port, 3 user, 4 password, 5 database, 6 table prefix. Password and user are empty for SQLite; for SQLite entry 1 (host) carries the database file path. Use admin_system::AdminApi::GetDatabaseConfig() from admin_api.hpp to get them as a named struct.
   * @return string[]: driver, host, port, user, password, database, prefix.
   */
  inline plg::vector<plg::string> GetDatabaseConfig() {
    return __admin_system_GetDatabaseConfig();
  }

  using _GetMessageType = int32_t (*)();
}
extern "C" PLUGIN_API admin_system::_GetMessageType __admin_system_GetMessageType;
namespace admin_system {
  /**
   * @brief Value of the 'message_type' key of core.ini (0 = no action messages, 1 = without the admin name, 2 = with the admin name). Modules use it to decide how loud their own action messages are.
   * @return int32: message_type from core.ini.
   */
  inline int32_t GetMessageType() {
    return __admin_system_GetMessageType();
  }

  using _IsCoreLoaded = bool (*)();
}
extern "C" PLUGIN_API admin_system::_IsCoreLoaded __admin_system_IsCoreLoaded;
namespace admin_system {
  /**
   * @brief True once the database connection is up, the tables exist and the configuration has been read - i.e. after OnCoreLoaded fired. Modules that were loaded late poll this instead of waiting for a callback.
   * @return bool: Whether the admin core finished loading.
   */
  inline bool IsCoreLoaded() {
    return __admin_system_IsCoreLoaded();
  }

} // namespace admin_system
