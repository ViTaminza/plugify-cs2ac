#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: categories)

namespace admin_system {

  using _RegisterCategory = void (*)(const plg::string&, const plg::string&, CategoryDisplayCallback);
}
extern "C" PLUGIN_API admin_system::_RegisterCategory __admin_system_RegisterCategory;
namespace admin_system {
  /**
   * @brief Registers (or replaces) an admin menu category. The original fired every OnCategoryRegister callback before inserting the category; that order is kept, so a listener may react to the registration. name is a phrase key (translated by GetTranslation); callback may be null.
   * @param identity (string): Category identity, e.g. 'players'.
   * @param name (string): Phrase key used as the menu label.
   * @param callback (function): Display callback, may be null.
   */
  inline void RegisterCategory(const plg::string& identity, const plg::string& name, CategoryDisplayCallback callback) {
    return __admin_system_RegisterCategory(identity, name, callback);
  }

  using _RegisterItem = void (*)(const plg::string&, const plg::string&, const plg::string&, const plg::string&, ItemDisplayCallback, ItemSelectCallback);
}
extern "C" PLUGIN_API admin_system::_RegisterItem __admin_system_RegisterItem;
namespace admin_system {
  /**
   * @brief Registers (or replaces) an item of a menu category. Registration is idempotent per (category, identity): registering the same identity twice replaces the previous callbacks, matching the std::map insert of the original. flags is a '|'-separated list of permissions (the original split szFlags on '|'); an empty or null string leaves the item reachable only for '@admin/root' (splitting '' produced one empty flag in the original, which only the root wildcard could match).
   * @param identity (string): Item identity, e.g. 'ban'.
   * @param name (string): Phrase key used as the item label.
   * @param category (string): Identity of the owning category.
   * @param flags (string): Permissions separated by '|'; the item is shown when the admin has any of them, or '@admin/root'.
   * @param display (function): Display callback, may be null.
   * @param select (function): Select callback, may be null.
   */
  inline void RegisterItem(const plg::string& identity, const plg::string& name, const plg::string& category, const plg::string& flags, ItemDisplayCallback display, ItemSelectCallback select) {
    return __admin_system_RegisterItem(identity, name, category, flags, display, select);
  }

} // namespace admin_system
