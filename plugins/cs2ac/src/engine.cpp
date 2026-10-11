// cs2ac — engine access (all s2sdk).
//
// This file is the port's only place that talks to s2sdk: player state, schema fields, cvars,
// traces, the game clock and the net stats. Every detector goes through these accessors, which is
// what replaces the original's direct engine/schema reads (utils/schema.cpp, sdk/entity/*).
//
// Failure policy: before the first map (or without the s2sdk bindings) every getter returns a
// neutral value - the original's rule 2 (never query clients before a map exists) is enforced here,
// once, instead of in every detector.

#include "cs2ac.hpp"

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <cstdarg>
#include <cstdio>
#include <filesystem>

namespace cs2ac {

// ---------------------------------------------------------------------------------------------
// Logging (the original's Msg/Warning; prefixed, and mirrored to the plugify log)
// ---------------------------------------------------------------------------------------------

namespace {
void PlugifyLog(const char* message, plg::Severity severity)
{
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}
} // namespace

void Log(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const std::string message = "[CS2AC] " + FormatStringV(format, arguments);
	va_end(arguments);
	PlugifyLog(message.c_str(), plg::Severity::Info);
}

void LogWarning(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const std::string message = "[CS2AC] " + FormatStringV(format, arguments);
	va_end(arguments);
	PlugifyLog(message.c_str(), plg::Severity::Warning);
}

void LogError(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	const std::string message = "[CS2AC] " + FormatStringV(format, arguments);
	va_end(arguments);
	PlugifyLog(message.c_str(), plg::Severity::Error);
}

// The original's Msg: server console only, no severity mapping (used for the command output).
void ConsoleMessage(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	std::string message = "[CS2AC] " + FormatStringV(format, arguments);
	va_end(arguments);
	std::printf("%s\n", message.c_str());
}

// ---------------------------------------------------------------------------------------------
// Directories (the original read addons/cs2ac/... through the engine filesystem)
// ---------------------------------------------------------------------------------------------

namespace {
// Set once from OnPluginStart with plg::GetConfigsDir() + "/cs2ac" (the original's addons/cs2ac
// directory). Until then the default is harmless: no config file is found and the built-in
// defaults (identical to the shipped cs2ac.cfg) stay in effect.
std::string g_configDirectory = "configs/cs2ac";
} // namespace

std::string ConfigDirectory()
{
	return g_configDirectory;
}

std::string GamedataDirectory()
{
	return g_configDirectory;
}

void SetConfigDirectory(const std::string& directory)
{
	if (!directory.empty()) {
		g_configDirectory = directory;
	}
}

// ---------------------------------------------------------------------------------------------
// Player handles (player/player.cpp — the controller index is the slot plus one)
// ---------------------------------------------------------------------------------------------

namespace {
PlayerManager s_playerManager;
constexpr int kMaxSlots = 64;

} // namespace

PlayerManager* g_pPlayerManager = &s_playerManager;

PlayerRef* PlayerManager::ToPlayer(i32 index)
{
	// One stable handle per controller index, like the original's PlayerManager::players[].
	static std::array<std::unique_ptr<PlayerRef>, MAXPLAYERS + 1> refs = [] {
		std::array<std::unique_ptr<PlayerRef>, MAXPLAYERS + 1> handles;
		for (i32 slot = 0; slot <= MAXPLAYERS; ++slot) {
			handles[static_cast<size_t>(slot)] = std::make_unique<PlayerRef>(slot);
		}
		return handles;
	}();
	if (index < 1 || index > MAXPLAYERS) {
		return nullptr;
	}
	return refs[static_cast<size_t>(index)].get();
}

bool PlayerRef::IsConnected() const { return engine::PlayerConnected(index); }
bool PlayerRef::IsInGame() const { return engine::PlayerInGame(index); }
bool PlayerRef::IsFakeClient() const { return engine::PlayerFakeClient(index); }
bool PlayerRef::IsCSTV() const { return engine::PlayerCSTV(index); }
bool PlayerRef::IsAlive() const { return engine::PlayerAlive(index); }
const char* PlayerRef::GetName() const { return engine::PlayerName(index); }
u64 PlayerRef::GetSteamId64(bool) const { return engine::PlayerSteamId(index); }

namespace engine {

namespace {

bool g_bindingsAvailable {};
bool g_gameReady {};
int g_lastTick {};

struct TeleportState {
	Vector lastOrigin;
	Vector lastEye;
	bool valid {};
	double lastTeleportTime {-1.0};
};
std::array<TeleportState, MAXPLAYERS + 1> g_teleport;

// Gamedata-loaded offsets (resolved in engine::LoadGamedataOffsets).
i32 g_clientUserIdOffset {-1};

// --- client user ids --------------------------------------------------------------------------
// The original asked the engine (IVEngineServer2::GetPlayerUserId) for the {userid} placeholder.
// s2sdk does not expose that call, so the port reads CServerSideClientBase::m_UserID from the client
// pointer - but a wrong offset would substitute the WRONG PLAYER into a kick command, so the value
// is cross-validated before it is ever used:
//   * every game event the plugin consumes carries the engine's own userid for its subject
//     ("userid"), which is exactly the value GetPlayerUserId returns - that is remembered per slot
//     and is always correct (see RememberClientUserId);
//   * the offset read is only used once it agreed with such an event userid for some player (the
//     offset is a class-layout constant, so one agreement makes it trustworthy for everyone);
//   * a disagreement is logged once and permanently rejects the offset for this process.
// Until a userid is known the placeholder stays unavailable (IdentityUnavailable), exactly like the
// original when the engine had no userid for the player yet - never a guessed id.
std::array<i32, MAXPLAYERS + 1> g_clientUserIds {};
bool g_userIdOffsetVerified {};
bool g_userIdOffsetRejected {};

// --- ConVar handles ---------------------------------------------------------------------------
//
// s2sdk's FindConVar returns the engine's ConVarRef handle; a name that does not resolve returns
// the cvar system's invalid handle, which is 0xFFFFFFFF (s2sdk logs "Invalid ConVar handle:
// 4294967295" for it), so a plain `if (!handle)` test is not enough: every s2sdk
// cvar call made with it printed that warning, once per call - and the per-frame paths call these
// helpers many times per frame. IsValidHandle (core.cpp, unit-tested) rejects 0, the invalid
// sentinel and its sign-extended spelling, so no s2sdk cvar function is ever called with one.

struct ConVarCacheEntry {
	uint64_t handle {};
	bool lookedUp {};
	bool missingLogged {};
	bool typeMismatch {}; // the existing cvar's engine type is not the type this plugin registers
	std::chrono::steady_clock::time_point retryAt {};
};

// The cache is looked up by name on the frame path (the debug switches go through DebugFlag ->
// ServerCvarExists/ServerCvarBool on every detector step), so the hashing and the equality accept a
// string_view: a lookup neither builds nor hashes a temporary std::string. The stored key stays an
// owning std::string, which keeps a name that comes from a temporary (the config loader's parsed
// lines) valid as a key.
struct ConVarNameHash {
	using is_transparent = void;
	std::size_t operator()(std::string_view name) const noexcept
	{
		std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL); // FNV-1a, 64-bit
		for (const char character : name) {
			hash ^= static_cast<unsigned char>(character);
			hash *= static_cast<std::size_t>(1099511628211ULL);
		}
		return hash;
	}
};
struct ConVarNameEqual {
	using is_transparent = void;
	bool operator()(std::string_view first, std::string_view second) const noexcept { return first == second; }
};
std::unordered_map<std::string, ConVarCacheEntry, ConVarNameHash, ConVarNameEqual> g_conVarCache;
std::chrono::steady_clock::time_point g_conVarCacheStamp {};
constexpr auto kMissingConVarRetry = std::chrono::seconds(60);

// Resolves (and caches) a cvar handle. A miss is retried once a minute (a cvar can be created by
// another plugin that loads later); misses are logged once per name instead of per call.
uint64_t ResolveConVar(const char* name)
{
	if (!name || !*name || !g_bindingsAvailable) {
		return 0;
	}
	const auto now = std::chrono::steady_clock::now();
	auto it = g_conVarCache.find(std::string_view(name));
	if (it != g_conVarCache.end() && it->second.lookedUp) {
		if (it->second.handle != 0 || now < it->second.retryAt) {
			return it->second.handle;
		}
	}

	const uint64_t raw = s2sdk::FindConVar(name);
	auto& entry = g_conVarCache[name];
	entry.lookedUp = true;
	entry.handle = cvar::IsValidHandle(raw) ? raw : 0;
	if (entry.handle == 0) {
		entry.retryAt = now + kMissingConVarRetry;
		if (!entry.missingLogged) {
			entry.missingLogged = true;
			LogWarning("The server convar '%s' is not available (%s); the features that read it keep their defaults.",
					   name, cvar::IsValidHandle(raw) ? "not found" : "the lookup returned an invalid handle");
		}
	}
	return entry.handle;
}

// Creation is typed: the original registered CConVar<bool> for the toggles and CConVar<CUtlString>
// for the strings, and s2sdk's readers are typed too (GetConVarBool rejects a cvar whose engine type
// is not bool with "Invalid cvar type ..."), so a cs2ac_enabled created as a string cvar would read
// back as false forever.
//
// Note the gates: the specific bindings are checked instead of g_bindingsAvailable. The first
// version gated on that flag and s2sdk then logged "Only 0 of 47 cs2ac convars could be registered",
// because at registration time the flag was still false (it is only set by hooks::Initialize); the
// plugin now also calls hooks::Initialize() *before* registering, but the direct binding check keeps
// registration independent of that ordering.
uint64_t CreateTypedConVar(const char* name, bool stringType, const char* defaultValue, const char* description, s2sdk::ConVarFlag flags)
{
	if (!name || !*name) {
		return 0;
	}
	if (stringType ? !__s2sdk_CreateConVarString : !__s2sdk_CreateConVarBool) {
		return 0;
	}
	const plg::string desc = description ? description : "";
	uint64_t raw = stringType
					   ? s2sdk::CreateConVarString(name, plg::string(defaultValue ? defaultValue : ""), desc, flags)
					   : s2sdk::CreateConVarBool(name, cvar::BoolFromString(defaultValue), desc, flags, false, false, false, false);
	// A reload (or a cvar created by an earlier build) makes the engine refuse the name: s2sdk then
	// reports "ConVar '...' is already exists" and returns the existing engine ref. If the returned
	// value is not usable, the cvar is still *there* - look it up so the plugin keeps working and
	// the config values are applied instead of being dropped.
	uint64_t handle = cvar::IsValidHandle(raw) ? raw : 0;
	if (handle == 0 && __s2sdk_FindConVar) {
		const uint64_t existing = s2sdk::FindConVar(name);
		handle = cvar::IsValidHandle(existing) ? existing : 0;
		if (handle != 0) {
			Log("The convar '%s' already existed (plugin reload or an earlier install); using it.", name);
		}
	}
	auto& entry = g_conVarCache[name];
	entry.lookedUp = true;
	entry.handle = handle;
	// A cvar with this name can already exist with another type (created by an older build of this
	// plugin, which registered everything as strings, or by another plugin). s2sdk cannot remove a
	// cvar, so it stays as it is until the server restarts: detect it now, because a typed read of
	// it would print "Invalid cvar type ..." on every call and return a default.
	if (entry.handle != 0 && __s2sdk_GetConVar) {
		const plg::any current = s2sdk::GetConVar(entry.handle);
		entry.typeMismatch = stringType ? !plg::holds_alternative<plg::string>(current) : !plg::holds_alternative<bool>(current);
		if (entry.typeMismatch && !entry.missingLogged) {
			entry.missingLogged = true;
			LogWarning("The convar '%s' already exists with a different type (created by an older plugin build?); it keeps working through the "
					   "type-agnostic reads, but remove it (or restart the server) to get the proper %s cvar.",
					   name, stringType ? "string" : "bool");
		}
	}
	if (entry.handle == 0 && !entry.missingLogged) {
		entry.missingLogged = true;
		LogWarning("The cs2ac convar '%s' could not be registered; its setting stays at the built-in default.", name);
	}
	return entry.handle;
}

// Per-frame cache for the cvars the detectors poll (sv_cheats, mp_teammates_are_enemies).
struct FrameCache {
	bool svCheats {};
	bool svCheatsAvailable {};
	bool svCheatsValid {};
	bool teammates {};
	bool teammatesValid {};
};
FrameCache g_frameCache;

// --- the per-frame player snapshot ------------------------------------------------------------
//
// Plugin::OnGameFrame() walks all 64 slots in several passes (the teleport sampler, the shot
// correlator's position frame, the network sampler, the aimbot/aimlock/silent-aim/irregular/
// inhuman-accuracy/DLL-injection modules and the movement sampler) and every pass asks the same
// player-state questions - and each of those questions is a cross-plugin s2sdk call. The plugin
// runs synchronously inside the engine's game frame and changes no engine state (its only writes
// are queued server commands and packets), so no answer can change between those passes: fetching
// a value once per slot per frame hands every reader in that frame exactly what a fresh read would
// have returned.
//
// The snapshot is alive only while Plugin::OnGameFrame() runs (FrameSnapshotScope) - the usercmd
// and setup-move hooks, the game events and the console commands run outside it and read the
// engine live, so nothing about the hook paths changes. The values are stored lazily: a slot that
// an earlier pass already answered "not connected" for never pays for the checks the later passes
// would have short-circuited on (this is what the passes do today - each asks its own predicate
// and stops at the first decisive answer).
struct FramePlayer {
	FrameMemo<bool> connected;
	FrameMemo<bool> inGame;
	FrameMemo<bool> fake;
	FrameMemo<bool> cstv;
	FrameMemo<bool> alive;
	// The pawn pointer and its handle: the frame samplers derive "has a pawn", the eye position
	// and the schema reads (movement services, jump data) from them.
	FrameMemo<void*> pawn;
	FrameMemo<i32> pawnHandle;
	FrameMemo<i32> team;
	FrameMemo<Vector> origin;
	FrameMemo<Vector> eye;
	FrameMemo<bool> onGround;
	FrameMemo<i32> moveType;
	FrameMemo<void*> movementServices;
	FrameMemo<bool> framePerfectJump;
};

std::array<FramePlayer, MAXPLAYERS + 1> g_framePlayers;
bool g_frameSnapshotActive {};

// The game clock inside one frame: curtime/frametime/engine time are the same for every read the
// frame performs, and the frame path reads them many times per frame (and per player).
FrameMemo<f64> g_frameCurTime;
FrameMemo<f32> g_frameFrameTime;
FrameMemo<f64> g_frameRealtime;

// The frame snapshot record of a slot, or nullptr - the snapshot is not active (every caller
// outside Plugin::OnGameFrame) or the index is not a player slot. A nullptr always means "read the
// engine live", which is what the accessors below fall back to.
FramePlayer* FrameSlotFor(int index)
{
	if (!g_frameSnapshotActive || index < 1 || index > MAXPLAYERS) {
		return nullptr;
	}
	return &g_framePlayers[static_cast<size_t>(index)];
}

// Pending client-cvar queries: s2sdk hands the query back through a plain function pointer, so the
// user pointer travels in this registry instead of through plg::any. Replies are queued and drained
// on the plugin's frame pump (never handled inside the engine's cvar dispatch).
struct CvarReply {
	int slot {};
	int status {};
	std::string name;
	std::string value;
	ClientCvarCallback callback {};
	void* user {};
};

constexpr std::size_t kMaxPendingCvarReplies = 64;

struct PendingCvarQuery {
	struct Slot {
		std::string name;
		ClientCvarCallback callback {};
		void* user {};
		bool active {};
	};
	std::array<Slot, MAXPLAYERS + 1> slots;
	std::deque<CvarReply> replies;
	std::size_t dropped {};
};
PendingCvarQuery g_pendingCvarQueries;

// Schema field caching: the original resolved every field once at startup and refused to load when
// one was missing (cs2ac.cpp AddSchemaRequirements). The port resolves lazily and caches the
// offset, so a missing field only disables the reads that need it (fail soft, rule 3).
//
// The cache is a flat table keyed by the (className, fieldName) pair. The names are copied into the
// entry once, when the pair is first resolved (a fixed set, ~14 entries), and every later lookup is
// a linear scan of short string comparisons - the previous std::string-keyed map built *and hashed*
// a "class.field" key string on every call, and these lookups run per player per frame (movement
// services, jump data, flags), which is one heap allocation per call on the game thread.
struct SchemaCacheEntry {
	std::string className;
	std::string fieldName;
	i32 offset {-2}; // -2 = not looked up yet, -1 = unavailable
};
// All the pairs this plugin resolves - the current set is 14, so the table has room to spare.
constexpr std::size_t kSchemaCacheCapacity = 32;
std::array<SchemaCacheEntry, kSchemaCacheCapacity> g_schemaCache;
std::size_t g_schemaCacheCount {};

// Diagnostics: how many schema reads actually produced a value (surfaced by cs2ac_hooks). The
// movement services pointer used to be read with the entity-HANDLE accessor, which never resolved
// for a pointer field - every movement detector was silently dead with no visible symptom.
// The counters live in the header's engine::SchemaStats (read by cs2ac_hooks).
SchemaStats g_schemaStats;

i32 SchemaFieldOffset(const char* className, const char* fieldName)
{
	if (!className || !fieldName) {
		return -1;
	}
	SchemaCacheEntry* entry = nullptr;
	for (std::size_t index = 0; index < g_schemaCacheCount; ++index) {
		SchemaCacheEntry& candidate = g_schemaCache[index];
		if (candidate.className == className && candidate.fieldName == fieldName) {
			entry = &candidate;
			break;
		}
	}
	if (entry == nullptr) {
		if (g_schemaCacheCount >= g_schemaCache.size()) {
			// Unreachable with the fixed set of fields this plugin resolves; resolve without caching
			// so a pair beyond the capacity still answers instead of being dropped.
			return s2sdk::GetSchemaOffset(className, fieldName);
		}
		entry = &g_schemaCache[g_schemaCacheCount++];
		entry->className = className;
		entry->fieldName = fieldName;
	}
	if (entry->offset == -2) {
		entry->offset = s2sdk::GetSchemaOffset(entry->className.c_str(), entry->fieldName.c_str());
		if (entry->offset < 0) {
			LogWarning("The required player data field %s.%s is unavailable; the detectors that read it are disabled.", entry->className.c_str(),
					   entry->fieldName.c_str());
		}
	}
	return entry->offset;
}

// The client's pawn, or nullptr - without ever letting s2sdk print a warning.
//   * s2sdk::GetClientPawn goes through helpers::GetController, which prints "Cannot execute 'void*
//     GetClientPawn(int)' on invalid player slot" for a slot whose controller entity does not exist
//     yet (a connecting client). The s2sdk player-manager's IsClientInGame (which never warns) gates
//     the call, and a client that is not in game cannot have a pawn anyway.
//   * A client in game without a current pawn (e.g. a client still connecting) is answered
//     nullptr by GetClientPawn itself - helpers::GetController only warns when the *controller* is
//     missing - so no warning is printed for that case either.
void* PawnPointer(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// The in-game gate comes from the same record (the identical s2sdk call), so a slot that
		// was already classified costs nothing here.
		return slot->pawn.Get([&] { return (!g_gameReady || !PlayerInGame(index)) ? nullptr : s2sdk::GetClientPawn(index - 1); });
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS || !s2sdk::IsClientInGame(index - 1)) {
		return nullptr;
	}
	return s2sdk::GetClientPawn(index - 1);
}

// The entity's handle, derived from the pawn pointer s2sdk already validated (GetClientPawn).
// Entities are read through handles; components through their pointer (see below).
i32 PawnHandle(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->pawnHandle.Get([&] {
			void* pawn = PawnPointer(index);
			return pawn ? s2sdk::EntPointerToEntHandle(pawn) : -1;
		});
	}
	void* pawn = PawnPointer(index);
	return pawn ? s2sdk::EntPointerToEntHandle(pawn) : -1;
}

// --- schema reads ----------------------------------------------------------------------------
// Two rules from the official s2sdk guidance (its gamedata/schema guides):
//   * entities are accessed through their HANDLE (GetEntData*/GetEntDataVector3D*), components
//     (the movement services) through the pointer variants - the pointer is only used in the same
//     tick as the owner entity validation;
//   * the field *offset* is resolved by name and the raw value read at it, so no CSchemaType* is
//     dereferenced (the typed GetEntSchema*(class, field) calls do that, and that pointer can be
//     the invalid -1 sentinel).
// Widths follow the schema dump SwiftlyS2 ships with the server addons
// (addons/swiftlys2/gamedata/cs2/sdk.json, "size" of each field): bools and the
// MoveType_t enum are 1 byte, the movement ticks/fracs are 4 bytes each, vectors 12, raw
// pointers 8.

Vector ToVector(const plg::vec3& value)
{
	return {static_cast<f32>(value.x), static_cast<f32>(value.y), static_cast<f32>(value.z)};
}

bool ReadNumber(i32 handle, void* pointer, const char* className, const char* fieldName, i32 width, i64& out)
{
	// Never resolve a schema name while the engine gate is closed (between maps): s2sdk builds the
	// per-class field table on the first lookup and dereferences the CSchemaType* of every field
	// while doing it, and a table built during the loading window can be cached empty (or with
	// poisoned type pointers) for the rest of the session - for every plugin that shares it.
	if (!g_gameReady) {
		return false;
	}
	const i32 offset = SchemaFieldOffset(className, fieldName);
	if (offset < 0) {
		++g_schemaStats.offsetMisses;
		return false;
	}
	if (IsValidHandle(handle) && __s2sdk_GetEntData) {
		out = s2sdk::GetEntData(handle, offset, width);
		return true;
	}
	if (pointer != nullptr && __s2sdk_GetEntData2) {
		out = s2sdk::GetEntData2(pointer, offset, width);
		return true;
	}
	return false;
}

bool ReadVector(i32 handle, void* pointer, const char* className, const char* fieldName, Vector& out)
{
	if (!g_gameReady) {
		return false;
	}
	const i32 offset = SchemaFieldOffset(className, fieldName);
	if (offset < 0) {
		++g_schemaStats.offsetMisses;
		return false;
	}
	if (IsValidHandle(handle) && __s2sdk_GetEntDataVector3D) {
		out = ToVector(s2sdk::GetEntDataVector3D(handle, offset));
		return true;
	}
	if (pointer != nullptr && __s2sdk_GetEntDataVector3D2) {
		out = ToVector(s2sdk::GetEntDataVector3D2(pointer, offset));
		return true;
	}
	return false;
}

// A pointer-typed field (m_p*): read the pointer-sized *value* stored at the field's offset.
//   * The CHandle accessors (GetEntSchemaEnt*) reject anything that is not an m_h* field and
//     return "invalid" - that is what silently disabled every movement detector.
//   * GetEntDataPtr(handle, offset) returns the ADDRESS of the field, not the value it stores
//     ("a pointer to the data at the given memory location"), so it cannot be used directly.
//   * The typed GetEntSchema (which does return the pointer as an integer) dereferences the
//     cached CSchemaType* of the field, and that pointer can be the -1 sentinel, so it is
//     avoided here as everywhere else in this file.
// Reading the width the schema declares for a raw pointer (sizeof(void*)) at the cached offset
// gives the pointer value with no type dereference.
void* ReadPointerField(i32 handle, const char* className, const char* fieldName)
{
	if (!g_gameReady || !IsValidHandle(handle)) {
		return nullptr;
	}
	i64 value = 0;
	if (!ReadNumber(handle, nullptr, className, fieldName, static_cast<i32>(sizeof(void*)), value)) {
		return nullptr;
	}
	return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

// Address of a struct-typed field embedded in `object` (pointer arithmetic on the validated
// object, no schema type involved).
void* EmbeddedStructAddress(void* object, const char* className, const char* fieldName)
{
	if (object == nullptr) {
		return nullptr;
	}
	const i32 offset = SchemaFieldOffset(className, fieldName);
	if (offset < 0) {
		++g_schemaStats.offsetMisses;
		return nullptr;
	}
	return static_cast<uint8_t*>(object) + offset;
}

// The pawn's movement services (CBasePlayerPawn::m_pMovementServices) - a *pointer* field, read as
// a pointer through the pawn's handle.
void* ReadMovementServices(int index)
{
	void* services = ReadPointerField(PawnHandle(index), "CBasePlayerPawn", "m_pMovementServices");
	if (services != nullptr) {
		++g_schemaStats.servicesResolved;
	} else if (g_gameReady) {
		// Only a *real* miss counts: with the gate closed (between maps) every read fails by design
		// and would drown the diagnostic that cs2ac_hooks prints.
		++g_schemaStats.servicesMissing;
	}
	return services;
}

void* MovementServices(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// The pointer cannot move inside one frame (the plugin writes nothing back); the schema
		// scan and the schema read happen once per player per frame instead of once per reader.
		return slot->movementServices.Get([&] { return ReadMovementServices(index); });
	}
	return ReadMovementServices(index);
}

} // namespace

FrameSnapshotScope::FrameSnapshotScope()
{
	// A fresh frame starts with every memo unfilled (a re-entered or leaked scope cannot reuse a
	// previous frame's answers).
	g_framePlayers = {};
	g_frameCurTime = {};
	g_frameFrameTime = {};
	g_frameRealtime = {};
	g_frameSnapshotActive = true;
}

FrameSnapshotScope::~FrameSnapshotScope()
{
	g_frameSnapshotActive = false;
}

// Called when the plugin's configuration is (re)loaded: the next reads look the names up again.
void InvalidateConVarCache()
{
	for (auto& [name, entry] : g_conVarCache) {
		(void)name;
		entry.lookedUp = false;
		entry.retryAt = {};
	}
}

bool BindingsAvailable() { return g_bindingsAvailable; }
void SetBindingsAvailable(bool available) { g_bindingsAvailable = available; }
bool GameReady() { return g_gameReady; }
void SetGameReady(bool ready) { g_gameReady = ready; }

void OnMapStart()
{
	g_gameReady = true;
	for (auto& state : g_teleport) {
		state = {};
	}
}

void OnMapEnd()
{
	// Close the gate first: from here until the next OnMapStart every engine:: getter (client,
	// entity, schema, cvar-value) returns a neutral value, so plugin shutdown paths that still run
	// cannot query the engine's already-emptied client list.
	g_gameReady = false;
	g_lastTick = 0;
	// The sampled positions belong to the map that just ended; keeping them would report every
	// player as having teleported on the first frame of the next map.
	for (auto& state : g_teleport) {
		state = {};
	}
	// The learned user ids are re-learned from the next map's events; keeping them could pair a
	// stale id with a slot whose client changed during the changelevel. The offset's verified/
	// rejected verdict is a property of the game build, so it survives.
	for (auto& userId : g_clientUserIds) {
		userId = 0;
	}
}

int CurrentTick()
{
	if (!g_bindingsAvailable) {
		return 0;
	}
	g_lastTick = s2sdk::GetGameTickCount();
	return g_lastTick;
}

f64 CurTime()
{
	if (g_frameSnapshotActive) {
		return g_frameCurTime.Get([&] { return g_bindingsAvailable ? static_cast<f64>(s2sdk::GetGameTime()) : 0.0; });
	}
	return g_bindingsAvailable ? static_cast<f64>(s2sdk::GetGameTime()) : 0.0;
}

f32 FrameTime()
{
	if (g_frameSnapshotActive) {
		return g_frameFrameTime.Get([&] { return g_bindingsAvailable ? s2sdk::GetGameFrameTime() : 0.0f; });
	}
	return g_bindingsAvailable ? s2sdk::GetGameFrameTime() : 0.0f;
}

f64 Realtime()
{
	if (g_frameSnapshotActive) {
		return g_frameRealtime.Get([&] { return g_bindingsAvailable ? s2sdk::GetEngineTime() : 0.0; });
	}
	return g_bindingsAvailable ? s2sdk::GetEngineTime() : 0.0;
}

bool PlayerConnected(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->connected.Get([&] { return g_gameReady && s2sdk::IsClientConnected(index - 1); });
	}
	return g_gameReady && index >= 1 && index <= MAXPLAYERS && s2sdk::IsClientConnected(index - 1);
}

bool PlayerInGame(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->inGame.Get([&] { return g_gameReady && s2sdk::IsClientInGame(index - 1); });
	}
	return g_gameReady && index >= 1 && index <= MAXPLAYERS && s2sdk::IsClientInGame(index - 1);
}

// The two "is not a player we detect on" flags keep their inverted default: with the engine gate
// closed (between maps) they answer true, so every caller treats the slot as one to skip.
bool PlayerFakeClient(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->fake.Get([&] { return !g_gameReady || s2sdk::IsFakeClient(index - 1); });
	}
	return !g_gameReady || index < 1 || index > MAXPLAYERS || s2sdk::IsFakeClient(index - 1);
}

bool PlayerCSTV(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->cstv.Get([&] { return !g_gameReady || s2sdk::IsClientSourceTV(index - 1); });
	}
	return !g_gameReady || index < 1 || index > MAXPLAYERS || s2sdk::IsClientSourceTV(index - 1);
}

bool PlayerAlive(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->alive.Get([&] { return g_gameReady && s2sdk::IsClientAlive(index - 1); });
	}
	return g_gameReady && index >= 1 && index <= MAXPLAYERS && s2sdk::IsClientAlive(index - 1);
}

// PlayerHasPawn's answer per slot for the current server frame: 0 = not asked yet, 1 = no pawn,
// 2 = pawn. The getters below each ask it, and the frame samplers call several getters per player
// per frame, so without this the gate alone doubled the s2sdk calls of the frame path (two exports
// per ask). Reset by OnFrameRefresh at the start of every frame.
std::array<std::uint8_t, MAXPLAYERS + 1> g_framePawn {};

bool PlayerHasPawn(int index)
{
	// PawnPointer applies the in-game gate itself, so this is exactly "the client is in game and has
	// a pawn right now"; both of its negative cases (not in game, in game without a pawn) answer
	// without letting s2sdk print anything (see PawnPointer).
	if (index < 1 || index > MAXPLAYERS) {
		return false;
	}
	std::uint8_t& cached = g_framePawn[static_cast<std::size_t>(index)];
	if (cached == 0) {
		cached = PawnPointer(index) != nullptr ? 2 : 1;
	}
	return cached == 2;
}

const char* PlayerName(int index)
{
	// The original returned a per-player cached CUtlString; keep the same "stable pointer until the
	// next call for that player" contract with one cache per slot.
	static std::array<std::string, MAXPLAYERS + 1> cache;
	if (index < 1 || index > MAXPLAYERS || !g_gameReady) {
		return "<unknown>";
	}
	plg::string name = s2sdk::GetClientName(index - 1);
	cache[index] = std::string(name.c_str());
	// Trim like CUtlString::Trim did in the original.
	std::string& value = cache[index];
	const std::size_t first = value.find_first_not_of(" \t\r\n");
	const std::size_t last = value.find_last_not_of(" \t\r\n");
	value = first == std::string::npos ? std::string {} : value.substr(first, last - first + 1);
	return value.c_str();
}

u64 PlayerSteamId(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(index - 1);
}

// The getters below are implemented by s2sdk exports that go through helpers::GetController2, which
// needs the client's PAWN. For a client in game without one (e.g. a client still connecting)
// the export prints "Cannot execute '...' on invalid player pawn: N" and answers its neutral
// default - the frame samplers call these getters for exactly those clients every frame. The pawn is
// therefore checked first (PlayerHasPawn never warns) and the same neutral default is returned
// directly, so the value is unchanged and the console stays quiet.
int PlayerTeam(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// GetClientTeam's own answer when there is no pawn (or no map).
		return slot->team.Get([&] { return (!g_gameReady || !PlayerHasPawn(index)) ? CS_TEAM_NONE : static_cast<int>(s2sdk::GetClientTeam(index - 1)); });
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return CS_TEAM_NONE;
	}
	if (!PlayerHasPawn(index)) {
		return CS_TEAM_NONE; // GetClientTeam's own answer when it has no pawn
	}
	return static_cast<int>(s2sdk::GetClientTeam(index - 1));
}

int PlayerHealth(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return 0;
	}
	if (!PlayerHasPawn(index)) {
		return 0; // GetClientHealth's own answer when it has no pawn
	}
	return s2sdk::GetClientHealth(index - 1);
}

Vector PlayerOrigin(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// GetClientAbsOrigin's own answer when there is no pawn (or no map).
		return slot->origin.Get([&] { return (!g_gameReady || !PlayerHasPawn(index)) ? Vector {} : ToVector(s2sdk::GetClientAbsOrigin(index - 1)); });
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return {};
	}
	if (!PlayerHasPawn(index)) {
		return {}; // GetClientAbsOrigin's own answer when it has no pawn
	}
	return ToVector(s2sdk::GetClientAbsOrigin(index - 1));
}

Vector PlayerEye(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// GetClientEyePosition's own answer when there is no pawn (or no map).
		return slot->eye.Get([&] { return (!g_gameReady || !PlayerHasPawn(index)) ? Vector {} : ToVector(s2sdk::GetClientEyePosition(index - 1)); });
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return {};
	}
	if (!PlayerHasPawn(index)) {
		return {}; // GetClientEyePosition's own answer when it has no pawn
	}
	return ToVector(s2sdk::GetClientEyePosition(index - 1));
}

Vector PlayerVelocity(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return {};
	}
	if (!PlayerHasPawn(index)) {
		return {}; // GetClientAbsVelocity's own answer when it has no pawn
	}
	return ToVector(s2sdk::GetClientAbsVelocity(index - 1));
}

int PlayerMoveType(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// GetClientMoveType's own answer when there is no pawn (or no map).
		return slot->moveType.Get([&] { return (!g_gameReady || !PlayerHasPawn(index)) ? MOVETYPE_NONE : static_cast<int>(s2sdk::GetClientMoveType(index - 1)); });
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return MOVETYPE_NONE;
	}
	if (!PlayerHasPawn(index)) {
		return MOVETYPE_NONE; // GetClientMoveType's own answer when it has no pawn
	}
	return static_cast<int>(s2sdk::GetClientMoveType(index - 1));
}

int PlayerActualMoveType(int index)
{
	i64 value = 0;
	// MoveType_t is a one-byte schema field.
	if (!ReadNumber(PawnHandle(index), nullptr, "CBaseEntity", "m_nActualMoveType", 1, value)) {
		return PlayerMoveType(index);
	}
	return static_cast<int>(value);
}

bool PlayerOnGround(int index)
{
	if (FramePlayer* slot = FrameSlotFor(index)) {
		// GetClientFlags' own answer when there is no pawn (or no map).
		return slot->onGround.Get([&] {
			return (!g_gameReady || !PlayerHasPawn(index)) ? false : (static_cast<u32>(s2sdk::GetClientFlags(index - 1)) & FL_ONGROUND) != 0;
		});
	}
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return false;
	}
	if (!PlayerHasPawn(index)) {
		return false; // GetClientFlags' own answer when it has no pawn
	}
	return (static_cast<u32>(s2sdk::GetClientFlags(index - 1)) & FL_ONGROUND) != 0;
}

bool PlayerOnGroundLastTick(int index)
{
	i64 value = 0;
	if (!ReadNumber(PawnHandle(index), nullptr, "CCSPlayerPawn", "m_bOnGroundLastTick", 1, value)) {
		return PlayerOnGround(index);
	}
	return value != 0;
}

bool PlayerScoped(int index)
{
	i64 value = 0;
	if (!ReadNumber(PawnHandle(index), nullptr, "CCSPlayerPawn", "m_bIsScoped", 1, value)) {
		return false;
	}
	return value != 0;
}

bool PlayerDucking(int index)
{
	i64 value = 0;
	void* services = MovementServices(index);
	// The movement services are a *component*, not an entity: the pointer variants are correct here
	// (the owning pawn was validated in the same tick), and a bool field is one byte.
	if (!ReadNumber(-1, services, "CCSPlayer_MovementServices", "m_bDucked", 1, value)
		&& !ReadNumber(-1, services, "CPlayer_MovementServices", "m_bDucked", 1, value)) {
		if (!PlayerHasPawn(index)) {
			return false; // GetClientFlags' own answer when it has no pawn
		}
		return (static_cast<u32>(s2sdk::GetClientFlags(index - 1)) & IN_DUCK) != 0;
	}
	return value != 0;
}

int PlayerGroundEntity(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return -1;
	}
	if (!PlayerHasPawn(index)) {
		return 0; // GetClientGroundEntity's own answer when it has no pawn
	}
	return s2sdk::GetClientGroundEntity(index - 1);
}

Vector PlayerLastMovementImpulses(int index)
{
	Vector value;
	void* services = MovementServices(index);
	// Component pointer variant (see PlayerDucking).
	if (!ReadVector(-1, services, "CPlayer_MovementServices", "m_vecLastMovementImpulses", value)) {
		return {};
	}
	return value;
}

// Records the engine userid a game event reported for a player slot (hooks::ReadEvent). The value is
// the engine's own userid - the same one IVEngineServer2::GetPlayerUserId returns - so it is always
// correct and doubles as the reference the gamedata offset is validated against.
void RememberClientUserId(int index, int userId)
{
	if (index < 1 || index > MAXPLAYERS || !PlausibleUserId(userId)) {
		return;
	}
	g_clientUserIds[static_cast<size_t>(index)] = userId;
}

// A player slot is reused by a different client after a disconnect, so a forgotten slot must not
// keep answering with the previous player's userid.
void ForgetClientUserId(int index)
{
	if (index >= 1 && index <= MAXPLAYERS) {
		g_clientUserIds[static_cast<size_t>(index)] = 0;
	}
}

namespace {

// The raw read of CServerSideClientBase::m_UserID (no validation): -1 when it cannot be read.
i32 ReadClientUserIdFromOffset(int index)
{
	if (g_clientUserIdOffset < 0) {
		return -1;
	}
	void* client = s2sdk::PlayerSlotToClientPtr(index - 1);
	if (!client) {
		return -1;
	}
	i32 userId = -1;
	std::memcpy(&userId, static_cast<const char*>(client) + g_clientUserIdOffset, sizeof(userId));
	return PlausibleUserId(userId) ? userId : -1;
}

} // namespace

int PlayerUserId(int index)
{
	// See the state comment above: the event userid is the always-correct source, the gamedata
	// offset is used only once it has proven itself against one.
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return -1;
	}
	const size_t slot = static_cast<size_t>(index);
	const i32 remembered = g_clientUserIds[slot];
	const i32 fromOffset = (g_userIdOffsetRejected || g_clientUserIdOffset < 0) ? -1 : ReadClientUserIdFromOffset(index);

	if (remembered > 0 && fromOffset > 0) {
		if (remembered != fromOffset && !g_userIdOffsetRejected) {
			// The offset does not name this build's m_UserID. Never use it again; the event-derived
			// value stays correct.
			g_userIdOffsetRejected = true;
			g_userIdOffsetVerified = false;
			LogWarning("The ClientUserIdOffset gamedata value (offset %d) disagrees with the userid the engine reports in game events "
					   "(%d vs %d); the offset is ignored and user ids come from the events.",
					   g_clientUserIdOffset, fromOffset, remembered);
			return remembered;
		}
		if (remembered == fromOffset && !g_userIdOffsetVerified) {
			g_userIdOffsetVerified = true;
			Log("ClientUserIdOffset verified against the engine's event userid (%d); the offset can be used for players without an event yet.",
				remembered);
		}
		return remembered;
	}
	if (remembered > 0) {
		return remembered;
	}
	// No event userid seen for this player yet: only a *verified* offset may answer.
	return g_userIdOffsetVerified ? fromOffset : -1;
}

int PlayerActiveWeapon(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return 0;
	}
	if (!PlayerHasPawn(index)) {
		return 0; // GetClientActiveWeapon's own answer when it has no pawn
	}
	return s2sdk::GetClientActiveWeapon(index - 1);
}

QAngle PlayerViewAngles(int index)
{
	Vector angles;
	if (!ReadVector(PawnHandle(index), nullptr, "CBasePlayerPawn", "v_angle", angles)) {
		if (!PlayerHasPawn(index)) {
			return {}; // GetClientEyeAngles' own answer when it has no pawn
		}
		angles = ToVector(s2sdk::GetClientEyeAngles(index - 1));
	}
	return {angles.x, angles.y, angles.z};
}

namespace {
bool ReadFramePerfectJump(int index)
{
	// MovementEventService::OnJumpModern compared the usable jump press against the landing:
	//   lastUsableJumpPress = m_nLastUsableJumpPressTick + m_flLastUsableJumpPressFrac
	//   modernLandTime      = m_nLastLandedTick + m_flLastLandedFrac
	//   mightBeModernPerf   = lastUsableJumpPress < modernLandTime
	// The jump struct lives inside the movement services (CCSPlayer_MovementServices::m_ModernJump).
	void* services = MovementServices(index);
	if (!services) {
		return false;
	}
	// m_ModernJump is an embedded struct: its address is the field offset inside the (already
	// validated) movement services component, and the four fields are then read at their own
	// offsets. No typed schema access, no CSchemaType* dereference.
	void* modernJump = EmbeddedStructAddress(services, "CCSPlayer_MovementServices", "m_ModernJump");
	if (modernJump == nullptr) {
		return false;
	}
	static i32 pressTick = SchemaFieldOffset("CCSPlayerModernJump", "m_nLastUsableJumpPressTick");
	static i32 pressFrac = SchemaFieldOffset("CCSPlayerModernJump", "m_flLastUsableJumpPressFrac");
	static i32 landedTick = SchemaFieldOffset("CCSPlayerModernJump", "m_nLastLandedTick");
	static i32 landedFrac = SchemaFieldOffset("CCSPlayerModernJump", "m_flLastLandedFrac");
	if (pressTick < 0 || pressFrac < 0 || landedTick < 0 || landedFrac < 0) {
		return false;
	}
	// Widths per the schema: the two *_Tick fields are int32, the two *_Frac fields are floats.
	i32 lastPressTick = 0;
	f32 lastPressFrac = 0.0f;
	i32 lastLandedTick = 0;
	f32 lastLandedFrac = 0.0f;
	std::memcpy(&lastPressTick, static_cast<const char*>(modernJump) + pressTick, sizeof(lastPressTick));
	std::memcpy(&lastPressFrac, static_cast<const char*>(modernJump) + pressFrac, sizeof(lastPressFrac));
	std::memcpy(&lastLandedTick, static_cast<const char*>(modernJump) + landedTick, sizeof(lastLandedTick));
	std::memcpy(&lastLandedFrac, static_cast<const char*>(modernJump) + landedFrac, sizeof(lastLandedFrac));
	if (!std::isfinite(lastPressFrac) || !std::isfinite(lastLandedFrac)) {
		return false;
	}
	++g_schemaStats.jumpDataReads;
	// The comparison itself (tick + fraction on both sides) is the pure, unit-tested helper.
	return movement::IsFramePerfectJump(lastPressTick, lastPressFrac, lastLandedTick, lastLandedFrac);
}
} // namespace

bool PlayerFramePerfectJump(int index)
{
	// The per-frame movement sampler asks this once per player; the schema scan and the four field
	// reads behind it are shared with every other reader of the frame (and with a repeated call).
	if (FramePlayer* slot = FrameSlotFor(index)) {
		return slot->framePerfectJump.Get([&] { return ReadFramePerfectJump(index); });
	}
	return ReadFramePerfectJump(index);
}

SchemaStats SchemaStatistics()
{
	return g_schemaStats;
}

bool PlayerJustTeleported(int index, f32 thresholdSeconds)
{
	if (index < 1 || index > MAXPLAYERS) {
		return false;
	}
	const double last = g_teleport[index].lastTeleportTime;
	if (last < 0.0) {
		return false;
	}
	return (Realtime() - last) <= static_cast<double>(thresholdSeconds);
}

// Called once per server frame per *connected* player by the plugin (the frame loop already
// enumerates them, so this is not a second pass over all slots): records position discontinuities
// (the original's CBaseEntity::Teleport hook) and marks spawns.
void UpdateTeleportTrackingFor(int index)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return;
	}
	// A client without a current pawn (e.g. still connecting) has no position to sample: its last
	// known one is kept, so a respawn at a different origin still reads as a teleport below.
	if (!PlayerHasPawn(index)) {
		return;
	}
	auto& state = g_teleport[static_cast<size_t>(index)];
	const Vector origin = PlayerOrigin(index);
	const Vector eye = PlayerEye(index);
	if (!detection::IsFinite(origin) || !detection::IsFinite(eye)) {
		state.valid = false;
		return;
	}
	if (state.valid) {
		// A teleport moves the player further in one frame than any legitimate movement can.
		// The original hooked CBaseEntity::Teleport; this is the same signal from the sampled
		// positions (it cannot miss a teleport that ends far from the previous position).
		constexpr f32 kTeleportDistance = 400.0f;
		if ((origin - state.lastOrigin).Length() >= kTeleportDistance) {
			state.lastTeleportTime = Realtime();
		}
	}
	state.lastOrigin = origin;
	state.lastEye = eye;
	state.valid = true;
}

void ForgetTeleportTracking(int index)
{
	if (index >= 1 && index <= MAXPLAYERS) {
		g_teleport[static_cast<size_t>(index)] = {};
	}
}

// The plugin marks spawns through the player_spawn event (a respawn is a teleport for the
// detectors' grace windows).
void MarkSpawned(int index)
{
	if (index >= 1 && index <= MAXPLAYERS) {
		g_teleport[index].lastTeleportTime = Realtime();
	}
}

// Reads the optional offsets from this plugin's gamedata (hooks.cpp loads the config file and
// passes the id in; the offsets are resolved once). Every entry is optional: a missing offset only
// disables the reads that need it.
void RegisterGamedataOffset(const char* name, i32 offset)
{
	if (!name) {
		return;
	}
	if (EqualsIgnoreCase(name, "ClientUserIdOffset")) {
		g_clientUserIdOffset = offset;
	}
}

// Whether the {userid} placeholder can be substituted right now, and why not when it cannot (used
// by cs2ac_check_config). The event-derived userids are always correct; the offset is only a bridge
// for the moment before a player's first event.
std::string ClientUserIdStatus()
{
	if (g_userIdOffsetRejected) {
		return FormatString("event userids only (ClientUserIdOffset %d was rejected)", g_clientUserIdOffset);
	}
	if (g_clientUserIdOffset < 0) {
		return "event userids only (no ClientUserIdOffset in the gamedata)";
	}
	if (g_userIdOffsetVerified) {
		return FormatString("offset %d verified against the engine's event userids", g_clientUserIdOffset);
	}
	int known = 0;
	for (size_t slot = 1; slot <= MAXPLAYERS; ++slot) {
		known += g_clientUserIds[slot] > 0 ? 1 : 0;
	}
	return FormatString("offset %d present (not verified yet); %d slot(s) learned from events so far", g_clientUserIdOffset, known);
}

NetStats ClientNetStats(int index)
{
	NetStats stats;
	if (!g_gameReady || index < 1 || index > MAXPLAYERS) {
		return stats;
	}
	const f32 latency = s2sdk::GetClientLatency(index - 1);
	if (std::isfinite(latency) && latency >= 0.0f) {
		stats.pingSeconds = latency;
		stats.pingAvailable = true;
	}
	// Loss/choke/framerate live on the client's net channel, which s2sdk does not expose. The
	// original read them through INetChannelInfo; here they come from gamedata offsets when a
	// server operator adds them (see PORT_PLAN.md §6) and stay unavailable otherwise, which the
	// network-safety monitor reports as "unknown" instead of vetoing every detection.
	return stats;
}

const char* ClientCvarValue(int index, const char* name)
{
	static std::array<std::string, MAXPLAYERS + 1> cache;
	// The same gate as every other accessor in this file: s2sdk's client lookup walks the client
	// list (and gpGlobals), both of which are torn down between the map end and the next map start;
	// without a live map a client cvar value is meaningless anyway.
	if (!g_gameReady || index < 1 || index > MAXPLAYERS || !name) {
		return nullptr;
	}
	plg::string value = s2sdk::GetClientConVarValue(index - 1, name);
	if (value.empty()) {
		return nullptr;
	}
	cache[index] = std::string(value.c_str());
	return cache[index].c_str();
}

bool QueryClientCvar(int index, const char* name, ClientCvarCallback callback, void* user)
{
	if (!g_gameReady || index < 1 || index > MAXPLAYERS || !name || !callback) {
		return false;
	}
	const int slot = index - 1;
	// s2sdk's CvarValueCallback is (slot, queryId, status, name, value, data) — the same shape as
	// the original's IClientCvarValue callback, minus the interface pointer. The user pointer is
	// carried in the registry because the s2sdk callback is a captureless function pointer.
	//
	// The reply is *queued*, not handled inline: the handler evaluates the cvar, feeds the
	// detector latches and can even send a punishment (console command, chat, center HTML), and
	// those s2sdk calls must not happen while the engine is inside its own cvar-response dispatch
	// (re-entering a subsystem from inside its callback is what the other ports avoid too).
	auto trampoline = [](int32_t playerSlot, int32_t /*queryId*/, s2sdk::CvarValueStatus status, const plg::string& cvarName,
						 const plg::string& cvarValue, const plg::vector<plg::any>& /*data*/) {
		if (playerSlot < 0 || playerSlot >= MAXPLAYERS) {
			return;
		}
		auto& pending = g_pendingCvarQueries.slots[static_cast<size_t>(playerSlot)];
		if (!pending.active || !pending.callback) {
			return;
		}
		auto& queue = g_pendingCvarQueries.replies;
		if (queue.size() >= kMaxPendingCvarReplies) {
			// Bounded: a reply that cannot be queued is dropped (the next cycle re-queries anyway).
			pending.active = false;
			++g_pendingCvarQueries.dropped;
			return;
		}
		CvarReply reply;
		reply.slot = playerSlot;
		reply.status = static_cast<int>(status);
		reply.name.assign(cvarName.c_str());
		reply.value.assign(cvarValue.c_str());
		reply.callback = pending.callback;
		reply.user = pending.user;
		queue.push_back(std::move(reply));
		pending.active = false;
	};
	auto& pending = g_pendingCvarQueries.slots[static_cast<size_t>(slot)];
	pending.name = name;
	pending.callback = callback;
	pending.user = user;
	pending.active = true;
	plg::vector<plg::any> data;
	// The binding documents "a cookie, -1 on failure", but s2sdk 2.18.2's implementation
	// (PlayerManager::QueryCvarValue) answers a bool: 1 when the query was sent, 0 when the client is
	// gone or has no net channel. Only a definite success counts as started - a `>= 0` test would
	// also accept the 0 failure answer.
	return s2sdk::QueryClientConVar(slot, name, trampoline, data) > 0;
}

bool ServerCvarBool(const char* name, bool defaultValue)
{
	const uint64_t handle = ResolveConVar(name);
	if (!handle) {
		return defaultValue;
	}
	// A pre-existing cvar of another type (see CreateTypedConVar) is read through its string form -
	// GetConVarBool would only warn "Invalid cvar type" and answer false.
	auto it = g_conVarCache.find(std::string_view(name));
	if (it != g_conVarCache.end() && it->second.typeMismatch) {
		const std::string value = s2sdk::GetConVarValue(handle).c_str();
		return cvar::BoolFromString(value.c_str());
	}
	return s2sdk::GetConVarBool(handle);
}

f32 ServerCvarFloat(const char* name, f32 defaultValue)
{
	const uint64_t handle = ResolveConVar(name);
	return handle ? s2sdk::GetConVarFloat(handle) : defaultValue;
}

int ServerCvarInt(const char* name, int defaultValue)
{
	const uint64_t handle = ResolveConVar(name);
	return handle ? s2sdk::GetConVarInt32(handle) : defaultValue;
}

bool ServerCvarExists(const char* name)
{
	return ResolveConVar(name) != 0;
}

std::string ServerCvarString(const char* name)
{
	const uint64_t handle = ResolveConVar(name);
	return handle ? std::string(s2sdk::GetConVarValue(handle).c_str()) : std::string {};
}

bool RegisterConVar(const char* name, const char* defaultValue, const char* description)
{
	return CreateTypedConVar(name, false, defaultValue, description, s2sdk::ConVarFlag::None) != 0;
}

// The original's CConVar<CUtlString> settings. `protectedValue` is the one flag it used
// (FCVAR_PROTECTED on cs2ac_webhook_url: the value must not be echoed by the console or rcon).
bool RegisterStringConVar(const char* name, const char* defaultValue, const char* description, bool protectedValue)
{
	return CreateTypedConVar(name, true, defaultValue, description,
							 protectedValue ? s2sdk::ConVarFlag::Protected : s2sdk::ConVarFlag::None)
		   != 0;
}

bool SetCvarValue(const char* name, const char* value)
{
	if (!value) {
		return false;
	}
	const uint64_t handle = ResolveConVar(name);
	if (!handle) {
		return false;
	}
	// No replication: the cs2ac_* settings are server-side only (the original's ConVars were plain
	// server convars; replicating them would push the whitelist and command templates to clients).
	//
	// The setter must match the cvar's *engine* type: s2sdk's typed setters refuse a mismatch with
	// "Invalid cvar type for variable 'cs2ac_enabled'. Expected: 'bool', but got: 'string'." and
	// drop the value. Every bool line of cs2ac.cfg (the master switch, the 18 detectors, the two
	// announcements, the 16 debug switches, sv_cheats testing - 39 of its 47 lines) used to be
	// applied through the string setter, so every server start printed those 39 warnings and the
	// engine cvars kept their registration default (SyncSettingsFromConVars then copied that
	// default back over the file's value). Dispatch on the type s2sdk reports for the handle, the
	// way its own config loader does (ConVarManager::UpdateConVarValue): bool cvars get a bool,
	// string cvars the string.
	if (__s2sdk_GetConVar) {
		const plg::any current = s2sdk::GetConVar(handle);
		if (plg::holds_alternative<bool>(current)) {
			s2sdk::SetConVarBool(handle, cvar::BoolFromString(value), false, false);
			return true;
		}
		if (!plg::holds_alternative<plg::string>(current) && __s2sdk_SetConVarValue) {
			// A cvar of some other type (leftover from an older build, or owned by another plugin):
			// s2sdk's type-agnostic setter parses the string like the engine console would.
			s2sdk::SetConVarValue(handle, plg::string(value), false, false);
			return true;
		}
	}
	s2sdk::SetConVarString(handle, plg::string(value), false, false);
	return true;
}

bool SvCheatsEnabled()
{
	if (!g_frameCache.svCheatsValid) {
		g_frameCache.svCheatsAvailable = ServerCvarExists("sv_cheats");
		g_frameCache.svCheats = g_frameCache.svCheatsAvailable && ServerCvarBool("sv_cheats", false);
		g_frameCache.svCheatsValid = true;
	}
	return g_frameCache.svCheats;
}

bool SvCheatsAvailable()
{
	SvCheatsEnabled(); // refreshes the frame cache
	return g_frameCache.svCheatsAvailable;
}

bool TeammatesAreEnemiesCvar()
{
	if (!g_frameCache.teammatesValid) {
		g_frameCache.teammates = ServerCvarBool("mp_teammates_are_enemies", false);
		g_frameCache.teammatesValid = true;
	}
	return g_frameCache.teammates;
}

// Called once per server frame by the plugin: the per-frame cvar cache is invalidated here so the
// detectors still see changes within one frame (the original read the ConVarRefs live), and the
// queued client-cvar replies are handed to their handlers on this normal game-thread path.
void OnFrameRefresh()
{
	g_frameCache.svCheatsValid = false;
	g_frameCache.teammatesValid = false;
	g_framePawn.fill(0);
}

void DrainCvarReplies()
{
	// Bounded per call: a burst of replies cannot stall the frame.
	constexpr std::size_t kMaxPerDrain = 32;
	std::size_t processed = 0;
	while (!g_pendingCvarQueries.replies.empty() && processed < kMaxPerDrain) {
		CvarReply reply = std::move(g_pendingCvarQueries.replies.front());
		g_pendingCvarQueries.replies.pop_front();
		++processed;
		if (!reply.callback) {
			continue;
		}
		try {
			reply.callback(reply.slot, reply.status, reply.name.c_str(), reply.value.c_str(), reply.user);
		} catch (...) {
			// Nothing may unwind into the frame pump.
		}
	}
	if (g_pendingCvarQueries.dropped != 0) {
		LogWarning("%zu client cvar replies were dropped because the queue was full.", g_pendingCvarQueries.dropped);
		g_pendingCvarQueries.dropped = 0;
	}
}

void ServerCommand(const char* command)
{
	if (!command || !g_bindingsAvailable) {
		return;
	}
	s2sdk::ServerCommand(command);
}

bool TraceLine(const Vector& start, const Vector& end, int ignoreEntityIndex, TraceResult& result)
{
	if (!g_gameReady || !g_bindingsAvailable) {
		return false;
	}
	// The trace starts at the player's eye, i.e. inside their own pawn, and the caller ignores that
	// player: the ignore entity must therefore be the PAWN. s2sdk::PlayerSlotToEntHandle answers the
	// slot's *controller* (s2sdk's own client export returns the client entity index, i.e. entity
	// slot+1), which has no collision - ignoring it left the shooter's own body (its hitboxes are on
	// the pawn) in the trace. The reference traces ignore the pawn too (the legacy's trace filter
	// pass entity, and the other ported plugins convert the pawn pointer to a handle for the ignore
	// list). An invalid ignore handle makes s2sdk's TraceLine print "invalid entity handle" and
	// refuse the trace, so a slot without a pawn is answered here the same way without the warning.
	const int ignore = PawnHandle(ignoreEntityIndex);
	if (!IsValidHandle(ignore)) {
		return false;
	}
	plg::vec3 outPos;
	double outFraction = 0.0;
	bool outHit = false;
	int32_t outEntHit = 0;
	bool outStartSolid = false;
	const plg::vec3 startPos {start.x, start.y, start.z};
	const plg::vec3 endPos {end.x, end.y, end.z};
	if (!s2sdk::TraceLine(startPos, endPos, MASK_SHOT, ignore, outPos, outFraction, outHit, outEntHit, outStartSolid)) {
		return false;
	}
	result.hit = outHit;
	result.endPosition = ToVector(outPos);
	result.hitEntityIndex = outEntHit >= 0 ? s2sdk::EntPointerToPlayerSlot(s2sdk::EntHandleToEntPointer(outEntHit)) + 1 : 0;
	return true;
}

bool LegacyListenerSubscribes(int index, const char* eventName, bool& subscribed)
{
	// The original called IGameEventManager2::FindListener for the player's legacy listener. The
	// listener pointer is resolvable through the GetLegacyGameEventListener signature, but the
	// FindListener vtable index can only be verified against the game binary itself, so the whole
	// call is gated behind a gamedata entry; without it the caller (DLL Injection) stays off.
	(void)index;
	(void)eventName;
	(void)subscribed;
	return false;
}

bool ReadEyeOrigin(int index, Vector& out)
{
	if (!PlayerHasPawn(index)) {
		// A client without a pawn has no eye position: answering false keeps the detectors from
		// recording the (0,0,0) GetClientEyePosition answers for one as a real sample.
		return false;
	}
	out = PlayerEye(index);
	return detection::IsFinite(out);
}

} // namespace engine
} // namespace cs2ac
