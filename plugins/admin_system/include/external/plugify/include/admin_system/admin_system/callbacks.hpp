#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from admin_system.pplugin (group: callbacks)

namespace admin_system {

  using _OnAdminConnected_Register = void (*)(AdminConnectedCallback);
}
extern "C" PLUGIN_API admin_system::_OnAdminConnected_Register __admin_system_OnAdminConnected_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) callback of the calling plugin for the OnAdminConnected event. The original keyed listeners by PluginId, so one plugin had exactly one listener; registering again replaces it. The callback fires once per successful admin load (a player was found in as_admins), not for ordinary players.
   * @param callback (function): Function to invoke.
   */
  inline void OnAdminConnected_Register(AdminConnectedCallback callback) {
    return __admin_system_OnAdminConnected_Register(callback);
  }

  using _OnAdminConnected_Unregister = void (*)(AdminConnectedCallback);
}
extern "C" PLUGIN_API admin_system::_OnAdminConnected_Unregister __admin_system_OnAdminConnected_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnAdminConnected listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnAdminConnected_Unregister(AdminConnectedCallback callback) {
    return __admin_system_OnAdminConnected_Unregister(callback);
  }

  using _OnCoreLoaded_Register = void (*)(CoreLoadedCallback);
}
extern "C" PLUGIN_API admin_system::_OnCoreLoaded_Register __admin_system_OnCoreLoaded_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnCoreLoaded listener of the calling plugin. It fires as soon as the database is up and the tables exist; a plugin that registers later and sees IsCoreLoaded() == true must run its start logic immediately, because the event will not fire twice.
   * @param callback (function): Function to invoke.
   */
  inline void OnCoreLoaded_Register(CoreLoadedCallback callback) {
    return __admin_system_OnCoreLoaded_Register(callback);
  }

  using _OnCoreLoaded_Unregister = void (*)(CoreLoadedCallback);
}
extern "C" PLUGIN_API admin_system::_OnCoreLoaded_Unregister __admin_system_OnCoreLoaded_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnCoreLoaded listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnCoreLoaded_Unregister(CoreLoadedCallback callback) {
    return __admin_system_OnCoreLoaded_Unregister(callback);
  }

  using _OnCategoryRegister_Register = void (*)(CategoryRegisterCallback);
}
extern "C" PLUGIN_API admin_system::_OnCategoryRegister_Register __admin_system_OnCategoryRegister_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnCategoryRegister listener of the calling plugin. It fires for every RegisterCategory call, including the ones of the admin core itself, so it can be used to filter or re-order categories as they appear.
   * @param callback (function): Function to invoke.
   */
  inline void OnCategoryRegister_Register(CategoryRegisterCallback callback) {
    return __admin_system_OnCategoryRegister_Register(callback);
  }

  using _OnCategoryRegister_Unregister = void (*)(CategoryRegisterCallback);
}
extern "C" PLUGIN_API admin_system::_OnCategoryRegister_Unregister __admin_system_OnCategoryRegister_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnCategoryRegister listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnCategoryRegister_Unregister(CategoryRegisterCallback callback) {
    return __admin_system_OnCategoryRegister_Unregister(callback);
  }

  using _OnAction_Register = void (*)(ActionCallback);
}
extern "C" PLUGIN_API admin_system::_OnAction_Register __admin_system_OnAction_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnAction listener of the calling plugin, i.e. the receiving end of SendAction.
   * @param callback (function): Function to invoke.
   */
  inline void OnAction_Register(ActionCallback callback) {
    return __admin_system_OnAction_Register(callback);
  }

  using _OnAction_Unregister = void (*)(ActionCallback);
}
extern "C" PLUGIN_API admin_system::_OnAction_Unregister __admin_system_OnAction_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnAction listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnAction_Unregister(ActionCallback callback) {
    return __admin_system_OnAction_Unregister(callback);
  }

  using _OnPlayerPunish_Register = void (*)(PlayerPunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerPunish_Register __admin_system_OnPlayerPunish_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnPlayerPunish listener of the calling plugin. Fires after a connected player got punished, and when the punishments of a player are force-reloaded (mm_as_reload_punish).
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerPunish_Register(PlayerPunishCallback callback) {
    return __admin_system_OnPlayerPunish_Register(callback);
  }

  using _OnPlayerPunish_Unregister = void (*)(PlayerPunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerPunish_Unregister __admin_system_OnPlayerPunish_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnPlayerPunish listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerPunish_Unregister(PlayerPunishCallback callback) {
    return __admin_system_OnPlayerPunish_Unregister(callback);
  }

  using _OnOfflinePlayerPunish_Register = void (*)(OfflinePlayerPunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerPunish_Register __admin_system_OnOfflinePlayerPunish_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnOfflinePlayerPunish listener of the calling plugin. Fires after a player that is not connected got punished.
   * @param callback (function): Function to invoke.
   */
  inline void OnOfflinePlayerPunish_Register(OfflinePlayerPunishCallback callback) {
    return __admin_system_OnOfflinePlayerPunish_Register(callback);
  }

  using _OnOfflinePlayerPunish_Unregister = void (*)(OfflinePlayerPunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerPunish_Unregister __admin_system_OnOfflinePlayerPunish_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnOfflinePlayerPunish listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnOfflinePlayerPunish_Unregister(OfflinePlayerPunishCallback callback) {
    return __admin_system_OnOfflinePlayerPunish_Unregister(callback);
  }

  using _OnPlayerUnpunish_Register = void (*)(PlayerUnpunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerUnpunish_Register __admin_system_OnPlayerUnpunish_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnPlayerUnpunish listener of the calling plugin. Fires after the punishment of a connected player was lifted.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerUnpunish_Register(PlayerUnpunishCallback callback) {
    return __admin_system_OnPlayerUnpunish_Register(callback);
  }

  using _OnPlayerUnpunish_Unregister = void (*)(PlayerUnpunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerUnpunish_Unregister __admin_system_OnPlayerUnpunish_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnPlayerUnpunish listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerUnpunish_Unregister(PlayerUnpunishCallback callback) {
    return __admin_system_OnPlayerUnpunish_Unregister(callback);
  }

  using _OnOfflinePlayerUnpunish_Register = void (*)(OfflinePlayerUnpunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerUnpunish_Register __admin_system_OnOfflinePlayerUnpunish_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnOfflinePlayerUnpunish listener of the calling plugin. Fires after the punishment of a player that is not connected was lifted.
   * @param callback (function): Function to invoke.
   */
  inline void OnOfflinePlayerUnpunish_Register(OfflinePlayerUnpunishCallback callback) {
    return __admin_system_OnOfflinePlayerUnpunish_Register(callback);
  }

  using _OnOfflinePlayerUnpunish_Unregister = void (*)(OfflinePlayerUnpunishCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerUnpunish_Unregister __admin_system_OnOfflinePlayerUnpunish_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnOfflinePlayerUnpunish listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnOfflinePlayerUnpunish_Unregister(OfflinePlayerUnpunishCallback callback) {
    return __admin_system_OnOfflinePlayerUnpunish_Unregister(callback);
  }

  using _OnPlayerPunishPre_Register = void (*)(PlayerPunishPreCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerPunishPre_Register __admin_system_OnPlayerPunishPre_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnPlayerPunishPre listener of the calling plugin. It runs before a connected player is punished and can veto the punishment by returning true - exactly the 'true means block' convention of the original.
   * @param callback (function): Function to invoke.
   */
  inline void OnPlayerPunishPre_Register(PlayerPunishPreCallback callback) {
    return __admin_system_OnPlayerPunishPre_Register(callback);
  }

  using _OnPlayerPunishPre_Unregister = void (*)(PlayerPunishPreCallback);
}
extern "C" PLUGIN_API admin_system::_OnPlayerPunishPre_Unregister __admin_system_OnPlayerPunishPre_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnPlayerPunishPre listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnPlayerPunishPre_Unregister(PlayerPunishPreCallback callback) {
    return __admin_system_OnPlayerPunishPre_Unregister(callback);
  }

  using _OnOfflinePlayerPunishPre_Register = void (*)(OfflinePlayerPunishPreCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerPunishPre_Register __admin_system_OnOfflinePlayerPunishPre_Register;
namespace admin_system {
  /**
   * @brief Registers the (single) OnOfflinePlayerPunishPre listener of the calling plugin. It runs before a player that is not connected is punished and can veto the punishment by returning true.
   * @param callback (function): Function to invoke.
   */
  inline void OnOfflinePlayerPunishPre_Register(OfflinePlayerPunishPreCallback callback) {
    return __admin_system_OnOfflinePlayerPunishPre_Register(callback);
  }

  using _OnOfflinePlayerPunishPre_Unregister = void (*)(OfflinePlayerPunishPreCallback);
}
extern "C" PLUGIN_API admin_system::_OnOfflinePlayerPunishPre_Unregister __admin_system_OnOfflinePlayerPunishPre_Unregister;
namespace admin_system {
  /**
   * @brief Removes the OnOfflinePlayerPunishPre listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void OnOfflinePlayerPunishPre_Unregister(OfflinePlayerPunishPreCallback callback) {
    return __admin_system_OnOfflinePlayerPunishPre_Unregister(callback);
  }

} // namespace admin_system
