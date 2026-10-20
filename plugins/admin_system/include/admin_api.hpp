#pragma once
// Consumer-side wrapper for the admin_system plugin API.
//
// The generated headers (admin_system/admin_system.hpp and friends, produced by
// bin/plugify-gen from admin_system.pplugin) expose the raw boundary: plain C function
// pointers, plg::string / plg::vector<plg::string> values and no captures. This header
// restores the shape the Pisex modules were written against:
//
//   * std::function callbacks with captures (the IAdminApi typedefs), dispatched from static
//     trampolines that are keyed by category/identity (menu callbacks) or per plugin (events,
//     which the original keyed by PluginId - one listener per plugin, re-registering replaces);
//   * std::string / std::vector<std::string> instead of plg types;
//   * the default arguments of the original (adminID = -1, notify = true, ...);
//   * const char* returns that stay valid as long as the original's did.
//
// Porting a module is then mostly mechanical: keep `g_pAdminApi` and make it point at
// admin_system::AdminApi::Get(), drop the SourceMM::PluginId argument of the Register calls,
// everything else compiles as it did before.
//
//   #include <admin_api.hpp>                       // -I plugins/admin_system/include
//   #include <admin_system/admin_system.hpp>       // -I .../include/external/plugify/include
//
//   admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();
//
//   g_pAdminApi->RegisterItem("ban", "Item_Ban", "players", "@admin/ban", nullptr, OnItemSelect);
//   if (g_pAdminApi->HasPermission(iSlot, "@admin/ban")) { ... }
//
// Lifetime notes:
//   * GetTranslation()/GetAdminName()/GetPlayerPunishmentReason() return pointers into a cache
//     owned by this plugin, exactly like the g_vecPhrases / g_pAdmins maps of the original:
//     valid until the same key is queried again (translation pointers survive a config reload,
//     because the original's did not either).
//   * The category/identity pointers handed to the menu callbacks point into the registration
//     map and stay valid until the same item/category is registered again - the original
//     handed out its Item::szIdentity the same way.
//   * Every trampoline swallows exceptions (the boundary must not unwind into the engine) and
//     logs them through plg::Log.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <admin_system/admin_system.hpp>

#if defined(__has_include)
#	if __has_include(<plgdb/db.hpp>)
#		include <plgdb/db.hpp>
#		define ADMIN_SYSTEM_API_HAS_PLGDB 1
#	endif
#endif

namespace admin_system {

// --- punishment types (the RT_* macros of the original admin.h) ---------------
inline constexpr int32_t RT_BAN = 0;
inline constexpr int32_t RT_MUTE = 1;
inline constexpr int32_t RT_GAG = 2;
inline constexpr int32_t RT_SILENCE = 3;
inline constexpr int32_t kPunishmentTypes = 4;

// --- callback types (IAdminApi typedefs, captured std::function) --------------
using OnCategoryDisplayCallback = std::function<void(int iSlot, const char* szIdentity, std::string& szCategory)>;
using OnItemDisplayCallback = std::function<void(int iSlot, const char* szCategory, const char* szIdentity, std::string& szItem)>;
using OnItemSelectCallback = std::function<void(int iSlot, const char* szCategory, const char* szIdentity, const char* szItem)>;
using OnAdminConnectedCallback = std::function<void(int iSlot)>;
using OnPlayerPunishCallback = std::function<void(int iSlot, int iType, int iTime, const char* szReason, int iAdminID)>;
using OnOfflinePlayerPunishCallback = std::function<void(const char* szSteamID64, const char* szName, int iType, int iTime, const char* szReason, int iAdminID)>;
using OnPlayerUnpunishCallback = std::function<void(int iSlot, int iType, int iAdminID)>;
using OnOfflinePlayerUnpunishCallback = std::function<void(const char* szSteamID64, int iType, int iAdminID)>;
using OnCoreLoadedCallback = std::function<void()>;
using OnCategoryRegisterCallback = std::function<void(const char* szCategory)>;
using OnActionCallback = std::function<void(int iSlot, const char* szAction, const char* szParam)>;
using OnPlayerPunishPreCallback = std::function<bool(int iSlot, int iType, int iTime, const char* szReason, int iAdminID)>;
using OnOfflinePlayerPunishPreCallback = std::function<bool(const char* szSteamID64, const char* szName, int iType, int iTime, const char* szReason, int iAdminID)>;

// --- database settings --------------------------------------------------------
// The values the original handed out through GetMySQLConnection(), plus the prefix: a module
// that used to run its own queries (CheckCheats writes checkcheats_stats) opens its own plgdb
// connection with these and never touches the core's one.
struct DatabaseConfig {
	std::string driver = "mysql";      // "mysql" or "sqlite"
	std::string host = "127.0.0.1";    // sqlite: path of the database file
	uint16_t port = 3306;
	std::string user;
	std::string password;
	std::string database;
	std::string prefix = "as_";

	bool IsSqlite() const noexcept { return driver == "sqlite"; }
};

namespace detail {

inline std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

inline plg::string ToPlg(const std::string& value) {
	return plg::string(std::string_view(value));
}

// One non-empty plg::Log call; the consumer may run with logging disabled.
inline void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// True when the plugify module filled in the binding slots, i.e. when admin_system is loaded.
inline bool Available() {
	return __admin_system_GetPluginVersion != nullptr;
}

struct ItemCallbacks {
	std::string name;          // phrase key, handed out as the 4th argument of the select callback
	OnItemDisplayCallback display;
	OnItemSelectCallback select;
};

// Everything this plugin registered, plus the string caches that back the const char*
// returns. One instance per plugin (the header is included by exactly one translation unit
// of each module, but the accessor is a function-local static so several TUs share it).
struct State {
	std::map<std::string, OnCategoryDisplayCallback, std::less<>> categories;
	std::map<std::pair<std::string, std::string>, ItemCallbacks> items;

	OnAdminConnectedCallback adminConnected;
	OnCoreLoadedCallback coreLoaded;
	OnCategoryRegisterCallback categoryRegister;
	OnActionCallback action;
	OnPlayerPunishCallback playerPunish;
	OnOfflinePlayerPunishCallback offlinePlayerPunish;
	OnPlayerUnpunishCallback playerUnpunish;
	OnOfflinePlayerUnpunishCallback offlinePlayerUnpunish;
	OnPlayerPunishPreCallback playerPunishPre;
	OnOfflinePlayerPunishPreCallback offlinePlayerPunishPre;

	// Stable addresses: node based containers, so a returned c_str() survives later queries.
	std::map<std::string, std::string, std::less<>> translations;
	std::vector<std::string> names;   // index slot + 1, [0] is the console
	std::vector<std::string> groups;  // index slot
	std::vector<std::string> reasons; // index slot * 4 + type

	State() : names(65), groups(64), reasons(64 * kPunishmentTypes) {}
};

inline State& S() {
	static State state;
	return state;
}

// --- trampolines ---------------------------------------------------------------
// All of them are plain function pointers, so they can be handed to the admin_system plugin;
// the captures live in State. Exceptions never cross the boundary.

inline void CategoryDisplayTrampoline(int32_t slot, const plg::string& identity, plg::string& name) {
	try {
		State& state = S();
		const auto it = state.categories.find(ToStd(identity));
		if (it == state.categories.end() || !it->second) {
			return;
		}
		std::string label = ToStd(name);
		it->second(slot, it->first.c_str(), label);
		name = ToPlg(label);
	} catch (...) {
		LogError("admin_system: category display callback threw");
	}
}

inline void ItemDisplayTrampoline(int32_t slot, const plg::string& category, const plg::string& identity, plg::string& name) {
	try {
		State& state = S();
		const auto it = state.items.find({ToStd(category), ToStd(identity)});
		if (it == state.items.end() || !it->second.display) {
			return;
		}
		std::string label = ToStd(name);
		it->second.display(slot, it->first.first.c_str(), it->first.second.c_str(), label);
		name = ToPlg(label);
	} catch (...) {
		LogError("admin_system: item display callback threw");
	}
}

inline void ItemSelectTrampoline(int32_t slot, const plg::string& category, const plg::string& identity, const plg::string& name) {
	try {
		State& state = S();
		const auto it = state.items.find({ToStd(category), ToStd(identity)});
		if (it == state.items.end() || !it->second.select) {
			return;
		}
		it->second.select(slot, it->first.first.c_str(), it->first.second.c_str(), it->second.name.c_str());
		(void)name;
	} catch (...) {
		LogError("admin_system: item select callback threw");
	}
}

inline void AdminConnectedTrampoline(int32_t slot) {
	try {
		if (const auto& callback = S().adminConnected) {
			callback(slot);
		}
	} catch (...) {
		LogError("admin_system: OnAdminConnected callback threw");
	}
}

inline void CoreLoadedTrampoline() {
	try {
		if (const auto& callback = S().coreLoaded) {
			callback();
		}
	} catch (...) {
		LogError("admin_system: OnCoreLoaded callback threw");
	}
}

inline void CategoryRegisterTrampoline(const plg::string& identity) {
	try {
		State& state = S();
		// The identity comes from the registering plugin, not from our map, so it is copied
		// into a local that lives for the call (the original passed Category::szIdentity).
		const std::string copy = ToStd(identity);
		if (const auto& callback = state.categoryRegister) {
			callback(copy.c_str());
		}
	} catch (...) {
		LogError("admin_system: OnCategoryRegister callback threw");
	}
}

inline void ActionTrampoline(int32_t slot, const plg::string& action, const plg::string& param) {
	try {
		State& state = S();
		const std::string actionCopy = ToStd(action);
		const std::string paramCopy = ToStd(param);
		if (const auto& callback = state.action) {
			callback(slot, actionCopy.c_str(), paramCopy.c_str());
		}
	} catch (...) {
		LogError("admin_system: OnAction callback threw");
	}
}

inline void PlayerPunishTrampoline(int32_t slot, int32_t type, int32_t time, const plg::string& reason, int32_t adminID) {
	try {
		State& state = S();
		const std::string reasonCopy = ToStd(reason);
		if (const auto& callback = state.playerPunish) {
			callback(slot, type, time, reasonCopy.c_str(), adminID);
		}
	} catch (...) {
		LogError("admin_system: OnPlayerPunish callback threw");
	}
}

inline void OfflinePlayerPunishTrampoline(const plg::string& steamid64, const plg::string& name, int32_t type, int32_t time,
										  const plg::string& reason, int32_t adminID) {
	try {
		State& state = S();
		const std::string steamidCopy = ToStd(steamid64);
		const std::string nameCopy = ToStd(name);
		const std::string reasonCopy = ToStd(reason);
		if (const auto& callback = state.offlinePlayerPunish) {
			callback(steamidCopy.c_str(), nameCopy.c_str(), type, time, reasonCopy.c_str(), adminID);
		}
	} catch (...) {
		LogError("admin_system: OnOfflinePlayerPunish callback threw");
	}
}

inline void PlayerUnpunishTrampoline(int32_t slot, int32_t type, int32_t adminID) {
	try {
		if (const auto& callback = S().playerUnpunish) {
			callback(slot, type, adminID);
		}
	} catch (...) {
		LogError("admin_system: OnPlayerUnpunish callback threw");
	}
}

inline void OfflinePlayerUnpunishTrampoline(const plg::string& steamid64, int32_t type, int32_t adminID) {
	try {
		State& state = S();
		const std::string steamidCopy = ToStd(steamid64);
		if (const auto& callback = state.offlinePlayerUnpunish) {
			callback(steamidCopy.c_str(), type, adminID);
		}
	} catch (...) {
		LogError("admin_system: OnOfflinePlayerUnpunish callback threw");
	}
}

inline bool PlayerPunishPreTrampoline(int32_t slot, int32_t type, int32_t time, const plg::string& reason, int32_t adminID) {
	try {
		State& state = S();
		if (!state.playerPunishPre) {
			return false;
		}
		const std::string reasonCopy = ToStd(reason);
		return state.playerPunishPre(slot, type, time, reasonCopy.c_str(), adminID);
	} catch (...) {
		LogError("admin_system: OnPlayerPunishPre callback threw");
		return false;
	}
}

inline bool OfflinePlayerPunishPreTrampoline(const plg::string& steamid64, const plg::string& name, int32_t type, int32_t time,
											 const plg::string& reason, int32_t adminID) {
	try {
		State& state = S();
		if (!state.offlinePlayerPunishPre) {
			return false;
		}
		const std::string steamidCopy = ToStd(steamid64);
		const std::string nameCopy = ToStd(name);
		const std::string reasonCopy = ToStd(reason);
		return state.offlinePlayerPunishPre(steamidCopy.c_str(), nameCopy.c_str(), type, time, reasonCopy.c_str(), adminID);
	} catch (...) {
		LogError("admin_system: OnOfflinePlayerPunishPre callback threw");
		return false;
	}
}

inline std::vector<std::string> ToStdVector(const plg::vector<plg::string>& values) {
	std::vector<std::string> result;
	result.reserve(values.size());
	for (const plg::string& value : values) {
		result.push_back(ToStd(value));
	}
	return result;
}

} // namespace detail

// --- the API -------------------------------------------------------------------
// One instance per plugin; methods mirror IAdminApi one to one (same names, same argument
// order, same defaults), so ported modules only have to change the type of g_pAdminApi.
class AdminApi {
public:
	static AdminApi& Get() {
		static AdminApi api;
		return api;
	}

	AdminApi(const AdminApi&) = delete;
	AdminApi& operator=(const AdminApi&) = delete;

	// --- core ------------------------------------------------------------------
	float GetPluginVersion() const { return detail::Available() ? ::admin_system::GetPluginVersion() : 0.0f; }
	int GetMessageType() const { return detail::Available() ? static_cast<int>(::admin_system::GetMessageType()) : 0; }
	bool IsCoreLoaded() const { return detail::Available() && ::admin_system::IsCoreLoaded(); }
	int GetImmunityType() const { return detail::Available() ? static_cast<int>(::admin_system::GetImmunityType()) : 0; }

	// Translated phrase, cached per key; the key itself when it is missing (like the
	// g_vecPhrases lookup of the original).
	const char* GetTranslation(const char* key) {
		if (!key || !*key) {
			return "";
		}
		detail::State& state = detail::S();
		const auto it = state.translations.find(key);
		if (it != state.translations.end()) {
			return it->second.c_str();
		}
		std::string translated = key;
		if (detail::Available()) {
			translated = detail::ToStd(::admin_system::GetTranslation(detail::ToPlg(key)));
		}
		return state.translations.emplace(key, std::move(translated)).first->second.c_str();
	}

	// Drops the phrase cache (the original reloaded g_vecPhrases on mm_as_reload_config).
	void ReloadTranslations() { detail::S().translations.clear(); }

	DatabaseConfig GetDatabaseConfig() const {
		DatabaseConfig config;
		if (!detail::Available()) {
			return config;
		}
		const plg::vector<plg::string> raw = ::admin_system::GetDatabaseConfig();
		auto at = [&raw](size_t index) -> std::string {
			return index < raw.size() ? detail::ToStd(raw[index]) : std::string();
		};
		if (const std::string driver = at(0); !driver.empty()) {
			config.driver = driver;
		}
		if (const std::string host = at(1); !host.empty()) {
			config.host = host;
		}
		if (const std::string port = at(2); !port.empty()) {
			try {
				const int value = std::stoi(port);
				if (value > 0 && value < 65536) {
					config.port = static_cast<uint16_t>(value);
				}
			} catch (const std::exception&) {
				// keep the default port
			}
		}
		config.user = at(3);
		config.password = at(4);
		if (const std::string database = at(5); !database.empty()) {
			config.database = database;
		}
		if (const std::string prefix = at(6); !prefix.empty()) {
			config.prefix = prefix;
		}
		return config;
	}

	// --- menu registration -------------------------------------------------------
	// Pisex fired every OnCategoryRegister listener before inserting the category; the core
	// does the same, so a module may react to its own registration.
	void RegisterCategory(const char* identity, const char* name, OnCategoryDisplayCallback callback) {
		if (!identity || !detail::Available()) {
			return;
		}
		detail::S().categories[identity] = std::move(callback);
		::admin_system::RegisterCategory(detail::ToPlg(identity), detail::ToPlg(name ? name : ""), &detail::CategoryDisplayTrampoline);
	}

	// flags is a '|' separated permission list; an empty string leaves the item for '@admin/root'.
	void RegisterItem(const char* identity, const char* name, const char* category, const char* flags,
					  OnItemDisplayCallback callbackDisplay, OnItemSelectCallback callbackSelect) {
		if (!identity || !category || !detail::Available()) {
			return;
		}
		detail::ItemCallbacks callbacks;
		callbacks.name = name ? name : "";
		callbacks.display = std::move(callbackDisplay);
		callbacks.select = std::move(callbackSelect);
		detail::S().items[std::make_pair(std::string(category), std::string(identity))] = std::move(callbacks);
		::admin_system::RegisterItem(detail::ToPlg(identity), detail::ToPlg(name ? name : ""), detail::ToPlg(category),
									 detail::ToPlg(flags ? flags : ""), &detail::ItemDisplayTrampoline, &detail::ItemSelectTrampoline);
	}

	// --- admins -------------------------------------------------------------------
	bool HasPermission(int iSlot, const char* szPermission) const {
		return detail::Available() && szPermission && ::admin_system::HasPermission(iSlot, detail::ToPlg(szPermission));
	}

	bool HasFlag(int iSlot, const char* szFlag) const {
		return detail::Available() && szFlag && ::admin_system::HasFlag(iSlot, detail::ToPlg(szFlag));
	}

	bool IsAdmin(int iSlot) const { return detail::Available() && ::admin_system::IsAdmin(iSlot); }

	const char* GetFlagName(const char* szFlag) {
		if (!szFlag || !*szFlag) {
			return "";
		}
		detail::State& state = detail::S();
		const std::string key = std::string("flag:") + szFlag; // own namespace inside the cache
		const auto it = state.translations.find(key);
		if (it != state.translations.end()) {
			return it->second.c_str();
		}
		std::string name;
		if (detail::Available()) {
			name = detail::ToStd(::admin_system::GetFlagName(detail::ToPlg(szFlag)));
		}
		return state.translations.emplace(key, std::move(name)).first->second.c_str();
	}

	std::vector<std::string> GetPermissionsByFlag(const char* szFlag) const {
		if (!detail::Available() || !szFlag) {
			return {};
		}
		return detail::ToStdVector(::admin_system::GetPermissionsByFlag(detail::ToPlg(szFlag)));
	}

	std::vector<std::string> GetAdminFlags(int iSlot) const {
		if (!detail::Available()) {
			return {};
		}
		return detail::ToStdVector(::admin_system::GetAdminFlags(iSlot));
	}

	std::vector<std::string> GetAdminPermissions(int iSlot) const {
		if (!detail::Available()) {
			return {};
		}
		return detail::ToStdVector(::admin_system::GetAdminPermissions(iSlot));
	}

	int GetAdminImmunity(int iSlot) const {
		return detail::Available() ? static_cast<int>(::admin_system::GetAdminImmunity(iSlot)) : 0;
	}

	int64_t GetAdminExpireTime(int iSlot) const {
		return detail::Available() ? ::admin_system::GetAdminExpireTime(iSlot) : -1;
	}

	int GetAdminGroupID(int iSlot) const {
		return detail::Available() ? static_cast<int>(::admin_system::GetAdminGroupID(iSlot)) : -1;
	}

	// Name cached per slot (index 0 is the console, like the original's g_pAdmins[slot].szName).
	const char* GetAdminName(int iSlot) {
		detail::State& state = detail::S();
		if (iSlot < -1 || iSlot >= 64) {
			return "";
		}
		std::string name;
		if (detail::Available()) {
			name = detail::ToStd(::admin_system::GetAdminName(iSlot));
		}
		std::string& slot = state.names[static_cast<size_t>(iSlot + 1)];
		slot = std::move(name);
		return slot.c_str();
	}

	const char* GetAdminGroupName(int iSlot) {
		detail::State& state = detail::S();
		if (iSlot < 0 || iSlot >= 64) {
			return "";
		}
		std::string name;
		if (detail::Available()) {
			name = detail::ToStd(::admin_system::GetAdminGroupName(iSlot));
		}
		std::string& slot = state.groups[static_cast<size_t>(iSlot)];
		slot = std::move(name);
		return slot.c_str();
	}

	void AddPlayerAdmin(const char* szName, const char* szSteamID64, const char* szFlags, int iImmunity = 0,
						int iExpireTime = 0, int iGroupID = 0, const char* szComment = "", bool bDB = true) const {
		if (!detail::Available() || !szSteamID64) {
			return;
		}
		::admin_system::AddPlayerAdmin(detail::ToPlg(szName ? szName : ""), detail::ToPlg(szSteamID64),
									   detail::ToPlg(szFlags ? szFlags : ""), iImmunity, iExpireTime, iGroupID,
									   detail::ToPlg(szComment ? szComment : ""), bDB);
	}

	void RemovePlayerAdmin(const char* szSteamID64, bool bDB = true) const {
		if (!detail::Available() || !szSteamID64) {
			return;
		}
		::admin_system::RemovePlayerAdmin(detail::ToPlg(szSteamID64), bDB);
	}

	void AddPlayerLocalFlag(int iSlot, const char* szFlag) const {
		if (detail::Available() && szFlag) {
			::admin_system::AddPlayerLocalFlag(iSlot, detail::ToPlg(szFlag));
		}
	}

	void RemovePlayerLocalFlag(int iSlot, const char* szFlag) const {
		if (detail::Available() && szFlag) {
			::admin_system::RemovePlayerLocalFlag(iSlot, detail::ToPlg(szFlag));
		}
	}

	void AddPlayerLocalPermission(int iSlot, const char* szPermission) const {
		if (detail::Available() && szPermission) {
			::admin_system::AddPlayerLocalPermission(iSlot, detail::ToPlg(szPermission));
		}
	}

	void RemovePlayerLocalPermission(int iSlot, const char* szPermission) const {
		if (detail::Available() && szPermission) {
			::admin_system::RemovePlayerLocalPermission(iSlot, detail::ToPlg(szPermission));
		}
	}

	void AddPlayerLocalImmunity(int iSlot, int iImmunity) const {
		if (detail::Available()) {
			::admin_system::AddPlayerLocalImmunity(iSlot, iImmunity);
		}
	}

	void RemovePlayerLocalImmunity(int iSlot) const {
		if (detail::Available()) {
			::admin_system::RemovePlayerLocalImmunity(iSlot);
		}
	}

	// --- punishments ---------------------------------------------------------------
	bool IsPlayerPunished(int iSlot, int iType) const {
		return detail::Available() && ::admin_system::IsPlayerPunished(iSlot, iType);
	}

	int64_t GetPlayerPunishmentExpired(int iSlot, int iType) const {
		return detail::Available() ? ::admin_system::GetPlayerPunishmentExpired(iSlot, iType) : -1;
	}

	const char* GetPlayerPunishmentReason(int iSlot, int iType) {
		if (iSlot < 0 || iSlot >= 64 || iType < 0 || iType >= kPunishmentTypes) {
			return "";
		}
		std::string reason;
		if (detail::Available()) {
			reason = detail::ToStd(::admin_system::GetPlayerPunishmentReason(iSlot, iType));
		}
		std::string& slot = detail::S().reasons[static_cast<size_t>(iSlot) * kPunishmentTypes + static_cast<size_t>(iType)];
		slot = std::move(reason);
		return slot.c_str();
	}

	void AddPlayerPunishment(int iSlot, int iType, int iTime, const char* szReason, int iAdminID = -1,
							 bool bNotify = true, bool bDB = true) const {
		if (detail::Available()) {
			::admin_system::AddPlayerPunishment(iSlot, iType, iTime, detail::ToPlg(szReason ? szReason : ""), iAdminID, bNotify, bDB);
		}
	}

	void AddOfflinePlayerPunishment(const char* szSteamID64, const char* szName, int iType, int iTime, const char* szReason,
									int iAdminID = -1) const {
		if (!detail::Available() || !szSteamID64) {
			return;
		}
		::admin_system::AddOfflinePlayerPunishment(detail::ToPlg(szSteamID64), detail::ToPlg(szName ? szName : ""), iType, iTime,
												   detail::ToPlg(szReason ? szReason : ""), iAdminID);
	}

	void RemovePlayerPunishment(int iSlot, int iType, int iAdminID = -1, bool bNotify = true) const {
		if (detail::Available()) {
			::admin_system::RemovePlayerPunishment(iSlot, iType, iAdminID, bNotify);
		}
	}

	void RemoveOfflinePlayerPunishment(const char* szSteamID64, int iType, int iAdminID = -1) const {
		if (!detail::Available() || !szSteamID64) {
			return;
		}
		::admin_system::RemoveOfflinePlayerPunishment(detail::ToPlg(szSteamID64), iType, iAdminID);
	}

	// --- menus ----------------------------------------------------------------------
	void ShowAdminMenu(int iSlot) const {
		if (detail::Available()) {
			::admin_system::ShowAdminMenu(iSlot);
		}
	}

	void ShowAdminCategoryMenu(int iSlot, const char* szCategory) const {
		if (detail::Available() && szCategory) {
			::admin_system::ShowAdminCategoryMenu(iSlot, detail::ToPlg(szCategory));
		}
	}

	void ShowAdminLastCategoryMenu(int iSlot) const {
		if (detail::Available()) {
			::admin_system::ShowAdminLastCategoryMenu(iSlot);
		}
	}

	void ShowAdminItemMenu(int iSlot, const char* szCategory, const char* szIdentity) const {
		if (detail::Available() && szCategory && szIdentity) {
			::admin_system::ShowAdminItemMenu(iSlot, detail::ToPlg(szCategory), detail::ToPlg(szIdentity));
		}
	}

	// --- actions ---------------------------------------------------------------------
	void SendAction(int iSlot, const char* szAction, const char* szParam) const {
		if (detail::Available() && szAction) {
			::admin_system::SendAction(iSlot, detail::ToPlg(szAction), detail::ToPlg(szParam ? szParam : ""));
		}
	}

	// --- callbacks --------------------------------------------------------------------
	// Every event keeps the semantics of the original: one listener per plugin, registering
	// again replaces the previous one, and Unregister removes it. The server deduplicates by
	// the trampoline pointer, which is constant per event and plugin.
	void OnAdminConnected(OnAdminConnectedCallback callback) {
		detail::S().adminConnected = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnAdminConnected_Register(&detail::AdminConnectedTrampoline);
		}
	}
	void OnAdminConnectedUnregister() {
		detail::S().adminConnected = nullptr;
		if (detail::Available()) {
			::admin_system::OnAdminConnected_Unregister(&detail::AdminConnectedTrampoline);
		}
	}

	void OnCoreLoaded(OnCoreLoadedCallback callback) {
		detail::S().coreLoaded = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnCoreLoaded_Register(&detail::CoreLoadedTrampoline);
		}
	}
	void OnCoreLoadedUnregister() {
		detail::S().coreLoaded = nullptr;
		if (detail::Available()) {
			::admin_system::OnCoreLoaded_Unregister(&detail::CoreLoadedTrampoline);
		}
	}

	void OnCategoryRegister(OnCategoryRegisterCallback callback) {
		detail::S().categoryRegister = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnCategoryRegister_Register(&detail::CategoryRegisterTrampoline);
		}
	}
	void OnCategoryRegisterUnregister() {
		detail::S().categoryRegister = nullptr;
		if (detail::Available()) {
			::admin_system::OnCategoryRegister_Unregister(&detail::CategoryRegisterTrampoline);
		}
	}

	void OnAction(OnActionCallback callback) {
		detail::S().action = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnAction_Register(&detail::ActionTrampoline);
		}
	}
	void OnActionUnregister() {
		detail::S().action = nullptr;
		if (detail::Available()) {
			::admin_system::OnAction_Unregister(&detail::ActionTrampoline);
		}
	}

	void OnPlayerPunish(OnPlayerPunishCallback callback) {
		detail::S().playerPunish = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnPlayerPunish_Register(&detail::PlayerPunishTrampoline);
		}
	}
	void OnPlayerPunishUnregister() {
		detail::S().playerPunish = nullptr;
		if (detail::Available()) {
			::admin_system::OnPlayerPunish_Unregister(&detail::PlayerPunishTrampoline);
		}
	}

	void OnOfflinePlayerPunish(OnOfflinePlayerPunishCallback callback) {
		detail::S().offlinePlayerPunish = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerPunish_Register(&detail::OfflinePlayerPunishTrampoline);
		}
	}
	void OnOfflinePlayerPunishUnregister() {
		detail::S().offlinePlayerPunish = nullptr;
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerPunish_Unregister(&detail::OfflinePlayerPunishTrampoline);
		}
	}

	void OnPlayerUnpunish(OnPlayerUnpunishCallback callback) {
		detail::S().playerUnpunish = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnPlayerUnpunish_Register(&detail::PlayerUnpunishTrampoline);
		}
	}
	void OnPlayerUnpunishUnregister() {
		detail::S().playerUnpunish = nullptr;
		if (detail::Available()) {
			::admin_system::OnPlayerUnpunish_Unregister(&detail::PlayerUnpunishTrampoline);
		}
	}

	void OnOfflinePlayerUnpunish(OnOfflinePlayerUnpunishCallback callback) {
		detail::S().offlinePlayerUnpunish = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerUnpunish_Register(&detail::OfflinePlayerUnpunishTrampoline);
		}
	}
	void OnOfflinePlayerUnpunishUnregister() {
		detail::S().offlinePlayerUnpunish = nullptr;
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerUnpunish_Unregister(&detail::OfflinePlayerUnpunishTrampoline);
		}
	}

	void OnPlayerPunishPre(OnPlayerPunishPreCallback callback) {
		detail::S().playerPunishPre = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnPlayerPunishPre_Register(&detail::PlayerPunishPreTrampoline);
		}
	}
	void OnPlayerPunishPreUnregister() {
		detail::S().playerPunishPre = nullptr;
		if (detail::Available()) {
			::admin_system::OnPlayerPunishPre_Unregister(&detail::PlayerPunishPreTrampoline);
		}
	}

	void OnOfflinePlayerPunishPre(OnOfflinePlayerPunishPreCallback callback) {
		detail::S().offlinePlayerPunishPre = std::move(callback);
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerPunishPre_Register(&detail::OfflinePlayerPunishPreTrampoline);
		}
	}
	void OnOfflinePlayerPunishPreUnregister() {
		detail::S().offlinePlayerPunishPre = nullptr;
		if (detail::Available()) {
			::admin_system::OnOfflinePlayerPunishPre_Unregister(&detail::OfflinePlayerPunishPreTrampoline);
		}
	}

private:
	AdminApi() = default;
};

// The object the ported modules use; name it g_pAdminApi to keep their code unchanged:
//
//     admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();
inline AdminApi& Admin() {
	return AdminApi::Get();
}

#ifdef ADMIN_SYSTEM_API_HAS_PLGDB
// Ready-to-use plgdb settings for a module that needs its own connection (the original
// hand-to-hand IMySQLConnection). Relative SQLite paths are the caller's business.
inline plgdb::Config MakeDatabaseConfig(const DatabaseConfig& config, std::string sqlitePath = {}) {
	plgdb::Config result;
	if (config.IsSqlite()) {
		result.driver = plgdb::Driver::SQLite;
		result.sqlitePath = std::move(sqlitePath);
	} else {
		result.driver = plgdb::Driver::MySQL;
		result.host = config.host;
		result.port = config.port;
		result.user = config.user;
		result.password = config.password;
		result.database = config.database;
	}
	return result;
}
#endif

} // namespace admin_system
