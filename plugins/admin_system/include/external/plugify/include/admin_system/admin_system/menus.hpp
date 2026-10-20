#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: menus)

namespace admin_system {

  using _ShowAdminMenu = void (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_ShowAdminMenu __admin_system_ShowAdminMenu;
namespace admin_system {
  /**
   * @brief Opens the main admin menu of a player (the original OnAdminMenu). Does nothing when the player is not an admin.
   * @param slot (int32): Player slot.
   */
  inline void ShowAdminMenu(int32_t slot) {
    return __admin_system_ShowAdminMenu(slot);
  }

  using _ShowAdminCategoryMenu = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_ShowAdminCategoryMenu __admin_system_ShowAdminCategoryMenu;
namespace admin_system {
  /**
   * @brief Opens one category of the admin menu and remembers it as the 'last category' so the back button works.
   * @param slot (int32): Player slot.
   * @param category (string): Category identity.
   */
  inline void ShowAdminCategoryMenu(int32_t slot, const plg::string& category) {
    return __admin_system_ShowAdminCategoryMenu(slot, category);
  }

  using _ShowAdminLastCategoryMenu = void (*)(int32_t);
}
extern "C" PLUGIN_API admin_system::_ShowAdminLastCategoryMenu __admin_system_ShowAdminLastCategoryMenu;
namespace admin_system {
  /**
   * @brief Re-opens the category the player looked at last - the call modules make after a submenu was closed.
   * @param slot (int32): Player slot.
   */
  inline void ShowAdminLastCategoryMenu(int32_t slot) {
    return __admin_system_ShowAdminLastCategoryMenu(slot);
  }

  using _ShowAdminItemMenu = void (*)(int32_t, const plg::string&, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_ShowAdminItemMenu __admin_system_ShowAdminItemMenu;
namespace admin_system {
  /**
   * @brief Invokes the select callback of one menu item, i.e. what the admin menu did when the item was picked.
   * @param slot (int32): Player slot.
   * @param category (string): Category identity.
   * @param identity (string): Item identity.
   */
  inline void ShowAdminItemMenu(int32_t slot, const plg::string& category, const plg::string& identity) {
    return __admin_system_ShowAdminItemMenu(slot, category, identity);
  }

} // namespace admin_system
