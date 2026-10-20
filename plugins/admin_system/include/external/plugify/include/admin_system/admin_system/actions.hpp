#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: actions)

namespace admin_system {

  using _SendAction = void (*)(int32_t, const plg::string&, const plg::string&);
}
extern "C" PLUGIN_API admin_system::_SendAction __admin_system_SendAction;
namespace admin_system {
  /**
   * @brief Broadcasts a named action with one parameter to every OnAction listener - the generic hook the original added so modules could talk to each other (e.g. CheckCheats announcing a verdict, Admins asking for a menu).
   * @param slot (int32): Player the action is about, -1 when there is none.
   * @param action (string): Action name.
   * @param param (string): Action parameter (modules pass numbers or strings, e.g. '7 3600 reason').
   */
  inline void SendAction(int32_t slot, const plg::string& action, const plg::string& param) {
    return __admin_system_SendAction(slot, action, param);
  }

} // namespace admin_system
