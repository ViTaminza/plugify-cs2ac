#include "admin_system.hpp"

admin_system::_GetPluginVersion __admin_system_GetPluginVersion = nullptr;

admin_system::_GetTranslation __admin_system_GetTranslation = nullptr;

admin_system::_GetDatabaseConfig __admin_system_GetDatabaseConfig = nullptr;

admin_system::_GetMessageType __admin_system_GetMessageType = nullptr;

admin_system::_IsCoreLoaded __admin_system_IsCoreLoaded = nullptr;

admin_system::_RegisterCategory __admin_system_RegisterCategory = nullptr;

admin_system::_RegisterItem __admin_system_RegisterItem = nullptr;

admin_system::_HasPermission __admin_system_HasPermission = nullptr;

admin_system::_HasFlag __admin_system_HasFlag = nullptr;

admin_system::_IsAdmin __admin_system_IsAdmin = nullptr;

admin_system::_GetFlagName __admin_system_GetFlagName = nullptr;

admin_system::_GetPermissionsByFlag __admin_system_GetPermissionsByFlag = nullptr;

admin_system::_GetAdminFlags __admin_system_GetAdminFlags = nullptr;

admin_system::_GetAdminPermissions __admin_system_GetAdminPermissions = nullptr;

admin_system::_GetAdminImmunity __admin_system_GetAdminImmunity = nullptr;

admin_system::_GetAdminExpireTime __admin_system_GetAdminExpireTime = nullptr;

admin_system::_GetAdminName __admin_system_GetAdminName = nullptr;

admin_system::_GetAdminGroupID __admin_system_GetAdminGroupID = nullptr;

admin_system::_GetAdminGroupName __admin_system_GetAdminGroupName = nullptr;

admin_system::_GetImmunityType __admin_system_GetImmunityType = nullptr;

admin_system::_AddPlayerAdmin __admin_system_AddPlayerAdmin = nullptr;

admin_system::_RemovePlayerAdmin __admin_system_RemovePlayerAdmin = nullptr;

admin_system::_AddPlayerLocalFlag __admin_system_AddPlayerLocalFlag = nullptr;

admin_system::_RemovePlayerLocalFlag __admin_system_RemovePlayerLocalFlag = nullptr;

admin_system::_AddPlayerLocalPermission __admin_system_AddPlayerLocalPermission = nullptr;

admin_system::_RemovePlayerLocalPermission __admin_system_RemovePlayerLocalPermission = nullptr;

admin_system::_AddPlayerLocalImmunity __admin_system_AddPlayerLocalImmunity = nullptr;

admin_system::_RemovePlayerLocalImmunity __admin_system_RemovePlayerLocalImmunity = nullptr;

admin_system::_IsPlayerPunished __admin_system_IsPlayerPunished = nullptr;

admin_system::_GetPlayerPunishmentExpired __admin_system_GetPlayerPunishmentExpired = nullptr;

admin_system::_GetPlayerPunishmentReason __admin_system_GetPlayerPunishmentReason = nullptr;

admin_system::_AddPlayerPunishment __admin_system_AddPlayerPunishment = nullptr;

admin_system::_AddOfflinePlayerPunishment __admin_system_AddOfflinePlayerPunishment = nullptr;

admin_system::_RemovePlayerPunishment __admin_system_RemovePlayerPunishment = nullptr;

admin_system::_RemoveOfflinePlayerPunishment __admin_system_RemoveOfflinePlayerPunishment = nullptr;

admin_system::_ShowAdminMenu __admin_system_ShowAdminMenu = nullptr;

admin_system::_ShowAdminCategoryMenu __admin_system_ShowAdminCategoryMenu = nullptr;

admin_system::_ShowAdminLastCategoryMenu __admin_system_ShowAdminLastCategoryMenu = nullptr;

admin_system::_ShowAdminItemMenu __admin_system_ShowAdminItemMenu = nullptr;

admin_system::_SendAction __admin_system_SendAction = nullptr;

admin_system::_OnAdminConnected_Register __admin_system_OnAdminConnected_Register = nullptr;

admin_system::_OnAdminConnected_Unregister __admin_system_OnAdminConnected_Unregister = nullptr;

admin_system::_OnCoreLoaded_Register __admin_system_OnCoreLoaded_Register = nullptr;

admin_system::_OnCoreLoaded_Unregister __admin_system_OnCoreLoaded_Unregister = nullptr;

admin_system::_OnCategoryRegister_Register __admin_system_OnCategoryRegister_Register = nullptr;

admin_system::_OnCategoryRegister_Unregister __admin_system_OnCategoryRegister_Unregister = nullptr;

admin_system::_OnAction_Register __admin_system_OnAction_Register = nullptr;

admin_system::_OnAction_Unregister __admin_system_OnAction_Unregister = nullptr;

admin_system::_OnPlayerPunish_Register __admin_system_OnPlayerPunish_Register = nullptr;

admin_system::_OnPlayerPunish_Unregister __admin_system_OnPlayerPunish_Unregister = nullptr;

admin_system::_OnOfflinePlayerPunish_Register __admin_system_OnOfflinePlayerPunish_Register = nullptr;

admin_system::_OnOfflinePlayerPunish_Unregister __admin_system_OnOfflinePlayerPunish_Unregister = nullptr;

admin_system::_OnPlayerUnpunish_Register __admin_system_OnPlayerUnpunish_Register = nullptr;

admin_system::_OnPlayerUnpunish_Unregister __admin_system_OnPlayerUnpunish_Unregister = nullptr;

admin_system::_OnOfflinePlayerUnpunish_Register __admin_system_OnOfflinePlayerUnpunish_Register = nullptr;

admin_system::_OnOfflinePlayerUnpunish_Unregister __admin_system_OnOfflinePlayerUnpunish_Unregister = nullptr;

admin_system::_OnPlayerPunishPre_Register __admin_system_OnPlayerPunishPre_Register = nullptr;

admin_system::_OnPlayerPunishPre_Unregister __admin_system_OnPlayerPunishPre_Unregister = nullptr;

admin_system::_OnOfflinePlayerPunishPre_Register __admin_system_OnOfflinePlayerPunishPre_Register = nullptr;

admin_system::_OnOfflinePlayerPunishPre_Unregister __admin_system_OnOfflinePlayerPunishPre_Unregister = nullptr;

