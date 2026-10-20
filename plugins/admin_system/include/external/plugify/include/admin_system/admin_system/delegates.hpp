#pragma once

#include "enums.hpp"
#include "aliases.hpp"
// Generated from admin_system.pplugin

namespace admin_system {

  // Called when the category is about to be added to the admin menu. May rewrite name - the original passed the (already translated) label by reference so a module could e.g. append a player count.
  using CategoryDisplayCallback = void (*)(int32_t, const plg::string&, plg::string&);


  // Called while the item list is built; may rewrite name in place.
  using ItemDisplayCallback = void (*)(int32_t, const plg::string&, const plg::string&, plg::string&);


  // Called when the admin picks the item. The original passed the item label as a fourth argument in addition to the identity; both are given here.
  using ItemSelectCallback = void (*)(int32_t, const plg::string&, const plg::string&, const plg::string&);


  using AdminConnectedCallback = void (*)(int32_t);


  using CoreLoadedCallback = void (*)();


  using CategoryRegisterCallback = void (*)(const plg::string&);


  using ActionCallback = void (*)(int32_t, const plg::string&, const plg::string&);


  using PlayerPunishCallback = void (*)(int32_t, int32_t, int32_t, const plg::string&, int32_t);


  using OfflinePlayerPunishCallback = void (*)(const plg::string&, const plg::string&, int32_t, int32_t, const plg::string&, int32_t);


  using PlayerUnpunishCallback = void (*)(int32_t, int32_t, int32_t);


  using OfflinePlayerUnpunishCallback = void (*)(const plg::string&, int32_t, int32_t);


  using PlayerPunishPreCallback = bool (*)(int32_t, int32_t, int32_t, const plg::string&, int32_t);


  using OfflinePlayerPunishPreCallback = bool (*)(const plg::string&, const plg::string&, int32_t, int32_t, const plg::string&, int32_t);


} // namespace admin_system
