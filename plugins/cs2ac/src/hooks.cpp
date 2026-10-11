// cs2ac — hooks: the polyhook detours on the gamedata-resolved libserver functions, the s2sdk
// listeners, the game-event hooks and the FireBullets net message (utils/hooks.cpp +
// movement/mv_hooks.cpp).
//
// What is detoured and why (see PORT_PLAN.md §4 for the full table):
//   * ProcessUsercmds — the usercmd stream every detector consumes (the original's CDetour).
//   * SetupMove      — the authoritative per-tick command ("the last command the server will
//                      actually simulate for this tick"), used by Aimlock/Aimbot/Triggerbot/ShotCorrelator.
// Everything else the original hooked (PhysicsSimulate, ProcessMovement, Duck, LadderMove,
// OnJump*, AirMove, AirAccelerate, WalkMove, TryPlayerMove, CategorizePosition) is *observed*
// instead: the same data is sampled once per server frame from s2sdk/schema reads, which is the
// same tick resolution those hooks had but without depending on the CMoveData/CCSPlayer_Movement
// Services layouts, whose offsets cannot be verified from the source alone. The
// movement detectors' gates (ground transitions, jump presses, air time, frame-perfect jumps) are
// reproduced from the schema fields the original read (m_ModernJump, m_bOnGroundLastTick, …); see
// engine::PlayerFramePerfectJump and MovementDetectionService::OnFrame.

#include "cs2ac.hpp"

#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

#include <exception>
#include <filesystem>

namespace cs2ac {
namespace hooks {

namespace {

std::vector<HookState> g_states;
s2sdk::ConfigId g_config {};
bool g_initialized {};
bool g_usercmdsSeen {};
bool g_usercmdParseFailedLogged {};
bool g_setupMoveSeen {};
// Diagnostics (cs2ac_hooks): whether the detours fire at all and how often they produce a usable
// batch. A hook that fires but never yields commands means the captured layout is not this build's.
u64 g_usercmdHookCalls {};
u64 g_usercmdBatches {};
u64 g_setupMoveCalls {};

// Argument indices of the detoured functions.
//
// polyhook maps the *declared* argument list positionally onto the SysV registers (slot 0 = RDI,
// slot 1 = RSI, ...), so the list must describe the function exactly as the engine's ABI sees it.
// The reference here is the original cs2ac's own detour typing (utils/detours.h:
// `CDetour<decltype(movement::Detour_<name>)>` over funchook, i.e. plain cdecl/free-function
// signatures) - and those signatures put the *first real parameter* in RDI:
//
//   i32 ProcessUsercmds(CCSPlayerController*, PlayerCommand*, int, bool, float)   // RDI = controller
//   void SetupMove(CCSPlayer_MovementServices*, PlayerCommand*, CMoveData*)       // RDI = services
//
// An extra leading "this" slot would shift every read by one register: reading the command pointer
// as the movement-services pointer hands a PlayerCommand* to s2sdk's PlayerServicesToPlayerSlot(),
// which dereferences it (-> GetPawn()) - a wild pointer call inside the engine. The declarations below
// therefore start at the first real parameter, exactly like the original.
enum ProcessUsercmdsArgs {
	kProcessUsercmdsController = 0,
	kProcessUsercmdsCommands = 1,
	kProcessUsercmdsCount = 2,
	kProcessUsercmdsPaused = 3,
	kProcessUsercmdsMargin = 4,
};
enum SetupMoveArgs {
	kSetupMoveServices = 0,
	kSetupMoveCommand = 1,
	kSetupMoveMoveData = 2,
};

// PlayerCommand layout (sdk/usercmd.h): CUserCmdBase { vptr; int cmdNum; uint8 unk[4] } and the
// CSGOUserCmdPB message directly after it (offset 16). sizeof(PlayerCommand) is asserted as 144 on
// Linux by the original's sdk/usercmd.h, which is the stride of the engine's command array.
constexpr size_t kUserCmdNumberOffset = 8;
constexpr size_t kUserCmdMessageOffset = 16;
constexpr size_t kPlayerCommandSize = 144;

// A guard against a shifted argument list: if polyhook captured the arguments in a different
// order than declared, the values read as pointers would be small integers. Every pointer is
// therefore checked before it reaches s2sdk (which would dereference it).
bool LooksLikePointer(const void* pointer)
{
	return reinterpret_cast<uintptr_t>(pointer) > 0x10000;
}

int PlayerIndexFromControllerPointer(void* controller)
{
	if (!LooksLikePointer(controller)) {
		return 0;
	}
	const int32_t slot = s2sdk::EntPointerToPlayerSlot(controller);
	return slot >= 0 && slot < MAXPLAYERS ? slot + 1 : 0;
}

int PlayerIndexFromServicesPointer(void* services)
{
	if (!LooksLikePointer(services)) {
		return 0;
	}
	const int32_t slot = s2sdk::PlayerServicesToPlayerSlot(services);
	return slot >= 0 && slot < MAXPLAYERS ? slot + 1 : 0;
}

// Parses the PlayerCommand array of one ProcessUsercmds call.
bool ParseCommands(void* commandsPointer, int count, std::vector<UsercmdView>& out)
{
	// The engine never processes more than the player slots' worth of commands in one call; a
	// larger or smaller value means the captured argument list is not what was declared.
	if (!LooksLikePointer(commandsPointer) || count <= 0 || count > MAXPLAYERS) {
		return false;
	}
	const auto* base = static_cast<const uint8_t*>(commandsPointer);
	out.clear();
	out.reserve(static_cast<size_t>(count));
	for (int index = 0; index < count; ++index) {
		const uint8_t* command = base + static_cast<size_t>(index) * kPlayerCommandSize;
		UsercmdView view;
		i32 cmdNum = 0;
		std::memcpy(&cmdNum, command + kUserCmdNumberOffset, sizeof(cmdNum));
		// The message can never be longer than the rest of its own PlayerCommand slot, so the read
		// stays inside the object even if the array's real stride were smaller than assumed.
		const size_t available = kPlayerCommandSize - kUserCmdMessageOffset;
		if (!ParseUserCmdMessage(command + kUserCmdMessageOffset, available, view)) {
			continue;
		}
		// Plausibility gate for the object layout (sizeof(PlayerCommand), the cmdNum offset): a
		// wrong stride or offset produces command numbers outside any real range. The detectors
		// otherwise treat the batch as absent (fail soft) instead of acting on garbage.
		if (!usercmd::IsPlausibleCommandNumber(cmdNum)) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				LogWarning("The usercmd command numbers look implausible (first: %d); the ProcessUsercmds layout is not confirmed for this build "
						   "and usercmd detections stay disabled. See cs2ac_usercmd_dump / PORT_PLAN.md.", cmdNum);
			}
			return false;
		}
		// The wire message carries its own command number; the object's cmdNum is authoritative.
		view.commandNumber = cmdNum;
		out.push_back(std::move(view));
	}
	if (out.empty()) {
		return false;
	}
	// The engine passes the commands it is about to simulate in order, so the numbers must be
	// strictly increasing within one batch (gaps between batches happen and the network-safety
	// monitor tracks them). A decreasing or repeated sequence means the object stride
	// (sizeof(PlayerCommand)) does not match this build: the batch is dropped instead of fed to
	// the detectors.
	std::vector<i32> numbers;
	numbers.reserve(out.size());
	for (const UsercmdView& view : out) {
		numbers.push_back(view.commandNumber);
	}
	if (!usercmd::IsStrictlyIncreasing(numbers)) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			LogWarning("The usercmd array does not look like the expected PlayerCommand layout (command numbers are not increasing); "
					   "usercmd detections stay disabled for this session (see PORT_PLAN.md).");
		}
		return false;
	}
	return true;
}

polyhook::ResultType OnProcessUsercmds(polyhook::HookHandle /*hook*/, polyhook::ParametersHandle params, int32_t count,
									   polyhook::ReturnHandle /*ret*/, polyhook::CallbackType type)
{
	if (type != polyhook::CallbackType::Pre || !params) {
		return polyhook::ResultType::Ignored;
	}
	try {
		++g_usercmdHookCalls;
		if (count <= kProcessUsercmdsMargin) {
			// The declared argument list did not match what polyhook captured: report once and
			// leave the hook inert instead of misreading the engine's registers.
			if (!g_usercmdParseFailedLogged) {
				g_usercmdParseFailedLogged = true;
				LogWarning("The ProcessUsercmds hook captured %d arguments (expected at least %d); usercmd detections stay disabled.",
						   count, kProcessUsercmdsMargin + 1);
			}
			return polyhook::ResultType::Ignored;
		}
		const int playerIndex = PlayerIndexFromControllerPointer(polyhook::GetArgumentPointer(params, kProcessUsercmdsController));
		if (playerIndex < 1 || playerIndex > MAXPLAYERS) {
			return polyhook::ResultType::Ignored;
		}
		const int commandCount = polyhook::GetArgumentInt32(params, kProcessUsercmdsCount);
		void* commandsPointer = polyhook::GetArgumentPointer(params, kProcessUsercmdsCommands);
		std::vector<UsercmdView> commands;
		if (!ParseCommands(commandsPointer, commandCount, commands)) {
			return polyhook::ResultType::Ignored;
		}
		g_usercmdsSeen = true;
		++g_usercmdBatches;
		g_plugin.NoteUsercmd(commands.front());
		g_plugin.OnProcessUsercmds(playerIndex, commands);
	} catch (...) {
		// No exception may escape into the engine's usercmd processing.
	}
	return polyhook::ResultType::Ignored;
}

polyhook::ResultType OnSetupMove(polyhook::HookHandle /*hook*/, polyhook::ParametersHandle params, int32_t count,
								 polyhook::ReturnHandle /*ret*/, polyhook::CallbackType type)
{
	if (type != polyhook::CallbackType::Pre || !params) {
		return polyhook::ResultType::Ignored;
	}
	try {
		++g_setupMoveCalls;
		if (count <= kSetupMoveCommand) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				LogWarning("The SetupMove hook captured %d arguments (expected at least %d); SetupMove-based detections stay disabled.", count,
						   kSetupMoveCommand + 1);
			}
			return polyhook::ResultType::Ignored;
		}
		const int playerIndex = PlayerIndexFromServicesPointer(polyhook::GetArgumentPointer(params, kSetupMoveServices));
		if (playerIndex < 1 || playerIndex > MAXPLAYERS) {
			return polyhook::ResultType::Ignored;
		}
		const auto* commandPointer = static_cast<const uint8_t*>(polyhook::GetArgumentPointer(params, kSetupMoveCommand));
		if (!LooksLikePointer(commandPointer)) {
			return polyhook::ResultType::Ignored;
		}
		UsercmdView command;
		i32 cmdNum = 0;
		std::memcpy(&cmdNum, commandPointer + kUserCmdNumberOffset, sizeof(cmdNum));
		if (!ParseUserCmdMessage(commandPointer + kUserCmdMessageOffset, kPlayerCommandSize - kUserCmdMessageOffset, command)) {
			return polyhook::ResultType::Ignored;
		}
		command.commandNumber = cmdNum;
		g_setupMoveSeen = true;
		g_plugin.OnSetupMove(playerIndex, command);
	} catch (...) {
		// No exception may escape into the engine's movement processing.
	}
	return polyhook::ResultType::Ignored;
}

// --- s2sdk listeners ---------------------------------------------------------

void OnGameFrameListener(bool simulating, bool /*firstTick*/, bool /*lastTick*/)
{
	try {
		// The per-player teleport tracking runs inside the plugin's frame loop (which already
		// enumerates the connected players), so there is no second pass over all 64 slots here.
		g_plugin.OnGameFrame(simulating);
	} catch (...) {
	}
}

void OnMapStartListener()
{
	try {
		// Order matters: the engine gate is opened first so the plugin's maps-start reset runs with
		// a valid world, and the teleport sample state is cleared before anything reads it.
		engine::OnMapStart();
		g_plugin.OnMapInit();
	} catch (...) {
	}
}

void OnMapEndListener()
{
	try {
		// The engine gate is closed *first*: at map shutdown the client list is already emptied and
		// every s2sdk client query (IsFakeClient, GetClientName, IsClientConnected, ...)
		// dereferences a null entry (crash: NameChangerModule::Reset -> IsFakeClient ->
		// GetClientBySlot -> SIGSEGV). With gameReady false every engine:: getter returns a neutral
		// value, so the plugin's shutdown path below cannot touch the (gone) clients.
		engine::OnMapEnd();
		g_plugin.OnMapShutdown();
	} catch (...) {
	}
}

void OnClientActiveListener(int32_t playerSlot, bool /*isActive*/)
{
	try {
		// The SteamID is not queried here: a client that has just become active may not be
		// authenticated yet and nothing in the port needs it at this moment (the original read the
		// xuid the hook itself carried).
		g_plugin.OnClientActive(playerSlot, 0);
	} catch (...) {
	}
}

void OnClientFullyConnectListener(int32_t playerSlot)
{
	try {
		g_plugin.OnClientFullyConnect(playerSlot);
	} catch (...) {
	}
}

void OnClientDisconnectListener(int32_t playerSlot, s2sdk::NetworkDisconnectionReason /*reason*/)
{
	try {
		g_plugin.OnClientDisconnect(playerSlot);
	} catch (...) {
	}
}

void OnClientSettingsChangedListener(int32_t playerSlot)
{
	try {
		g_plugin.OnClientSettingsChanged(playerSlot);
	} catch (...) {
	}
}

// --- game events --------------------------------------------------------------

// Reads the subset of the event's fields the detectors use (the original read them off IGameEvent
// in the modules; reading once here keeps the modules engine-free). Each event only pays for the
// fields its detectors look at - weapon_fire is by far the most frequent game event.
detection::GameEventData ReadEvent(const plg::string& name, void* event)
{
	detection::GameEventData data;
	data.name = name.c_str();
	if (!event) {
		return data;
	}
	const std::string_view eventName(name.c_str());
	auto getInt = [&](const char* key, int fallback) {
		try {
			return static_cast<int>(s2sdk::GetEventInt(event, key));
		} catch (...) {
			return fallback;
		}
	};
	auto getBool = [&](const char* key) {
		try {
			return s2sdk::GetEventBool(event, key);
		} catch (...) {
			return false;
		}
	};
	auto getFloat = [&](const char* key) {
		try {
			return s2sdk::GetEventFloat(event, key);
		} catch (...) {
			return 0.0f;
		}
	};
	auto getString = [&](const char* key) {
		try {
			return std::string(s2sdk::GetEventString(event, key).c_str());
		} catch (...) {
			return std::string {};
		}
	};

	// The detector-side indexes are controller indexes (slot + 1); the engine's event "userid" is
	// not guaranteed to equal them, so the slot is resolved through s2sdk. A non-player victim or
	// attacker (world damage) yields -1 and is ignored.
	auto getPlayerIndex = [&](const char* key) {
		try {
			const int32_t slot = s2sdk::GetEventPlayerSlot(event, key);
			return slot >= 0 ? static_cast<int>(slot) + 1 : -1;
		} catch (...) {
			return -1;
		}
	};
	data.userid = getPlayerIndex("userid");
	// The same event field in its raw form is the engine's user id (what the {userid} punishment
	// placeholder needs, and the reference engine::PlayerUserId validates the gamedata offset
	// against). Cheap enough for the most frequent event (one int read off the event).
	if (data.userid >= 1 && data.userid <= MAXPLAYERS) {
		try {
			engine::RememberClientUserId(data.userid, static_cast<int>(s2sdk::GetEventInt(event, "userid")));
		} catch (...) {
		}
	}
	if (eventName == "weapon_fire") {
		data.weapon = getString("weapon");
		return data; // the Doubletap detector only compares weapon and tick
	}
	if (eventName == "player_spawn") {
		return data; // the AntiAim module only needs the subject
	}
	if (eventName == "smokegrenade_detonate") {
		data.smokeX = getFloat("x");
		data.smokeY = getFloat("y");
		data.smokeZ = getFloat("z");
		return data;
	}

	// player_hurt / player_death share the shot outcome fields.
	data.attacker = getPlayerIndex("attacker");
	data.hitgroup = getInt("hitgroup", HITGROUP_GENERIC);
	data.dmgHealth = getInt("dmg_health", 0);
	data.penetrated = getInt("penetrated", 0) > 0;
	data.thrusmoke = getBool("thrusmoke");
	if (eventName == "player_death") {
		data.weapon = getString("weapon");
		data.attackerinair = getBool("attackerinair");
		data.noscope = getBool("noscope");
		data.headshot = getBool("headshot");
		data.distance = getFloat("distance");
	}
	return data;
}

struct ConsumedEvent {
	const char* name;
};

constexpr ConsumedEvent kConsumedEvents[] = {
	{"weapon_fire"}, {"player_hurt"}, {"player_death"}, {"player_spawn"}, {"smokegrenade_detonate"},
};

s2sdk::ResultType OnGameEvent(const plg::string& name, void* event, bool /*dontBroadcast*/)
{
	try {
		const std::string_view eventName(name.c_str());
		for (const auto& consumed : kConsumedEvents) {
			if (eventName == consumed.name) {
				g_plugin.OnGameEvent(ReadEvent(name, event), 0);
				break;
			}
		}
	} catch (...) {
	}
	return s2sdk::ResultType::Continue;
}

// --- FireBullets net message --------------------------------------------------

// The user message id of CMsgTEFireBullets, resolved once from its name.
//
// This listener runs for EVERY net message the server sends to a real client (s2sdk's detour on
// CServerSideClientBase::SendNetMessage), and s2sdk::UserMessageGetMessageName answers by value
// (plg::string, one allocation per call) - so the name is resolved once here and every message is
// filtered by the cheap message id instead. -2 = not resolved yet, 0 = not resolvable on this build
// (the fallback below then keeps the name comparison).
int16_t g_fireBulletsMessageId {-2};

int16_t FireBulletsMessageId()
{
	if (g_fireBulletsMessageId == -2) {
		g_fireBulletsMessageId = __s2sdk_UserMessageFindMessageIdByName
									 ? s2sdk::UserMessageFindMessageIdByName(plg::string("CMsgTEFireBullets"))
									 : 0;
	}
	return g_fireBulletsMessageId;
}

void OnNetMessage(int32_t playerSlot, void* message)
{
	try {
		if (!message) {
			return;
		}
		const int16_t fireBulletsId = FireBulletsMessageId();
		if (fireBulletsId > 0 && __s2sdk_UserMessageGetMessageID) {
			if (s2sdk::UserMessageGetMessageID(message) != fireBulletsId) {
				return;
			}
		} else {
			if (!__s2sdk_UserMessageGetMessageName) {
				return;
			}
			// The id could not be resolved (or the binding is missing): compare the name. It must be
			// an owning string - the export returns its plg::string by value, so a string_view over
			// its c_str() would dangle in the next statement.
			const plg::string name = s2sdk::UserMessageGetMessageName(message);
			if (std::string_view(name.c_str()) != "CMsgTEFireBullets") {
				return;
			}
		}
		(void)playerSlot;
		// The fields the Silentaim detector needs, read through the s2sdk protobuf accessors.
		bool missingField = false;
		std::string missing;
		const auto require = [&](bool present, const char* field) {
			if (!present) {
				missingField = true;
				missing += missing.empty() ? field : std::string(", ") + field;
			}
		};
		const bool hasPlayer = s2sdk::UserMessageHasField(message, "player");
		const bool hasInaccuracy = s2sdk::UserMessageHasField(message, "inaccuracy");
		const bool hasSpread = s2sdk::UserMessageHasField(message, "spread");
		const bool hasWeaponId = s2sdk::UserMessageHasField(message, "weapon_id");
		require(hasPlayer, "player");
		require(hasInaccuracy, "inaccuracy");
		require(hasSpread, "spread");
		require(hasWeaponId, "weapon_id");
		const f32 inaccuracy = hasInaccuracy ? s2sdk::PbReadFloat(message, "inaccuracy", -1) : 0.0f;
		const f32 spread = hasSpread ? s2sdk::PbReadFloat(message, "spread", -1) : 0.0f;
		const u32 weaponId = hasWeaponId ? static_cast<u32>(s2sdk::PbReadUInt32(message, "weapon_id", -1)) : 0;
		// The engine packs the shooter as CEntityIndex (index | (serial & 0x3FF) << 14); the
		// controller index the player manager uses is the entity index itself.
		const int packed = hasPlayer ? static_cast<int>(s2sdk::PbReadUInt32(message, "player", -1)) : -1;
		const int playerIndex = packed >= 0 ? (packed & 0x3FFF) : -1;
		g_plugin.OnFireBullets(playerIndex, engine::CurrentTick(), inaccuracy, spread, weaponId, missingField, missing);
	} catch (...) {
		// No exception may escape into the engine's message dispatch.
	}
}

// --- installation -------------------------------------------------------------

void* ResolveSignature(const char* name, HookState& state)
{
	if (g_config == 0) {
		state.note = "no gamedata loaded";
		return nullptr;
	}
	void* address = s2sdk::GetGameConfigSignature(g_config, name);
	if (!address) {
		state.note = "signature not found in the loaded gamedata (or it did not match this build)";
		return nullptr;
	}
	state.resolved = true;
	return address;
}

bool InstallDetour(const char* name, void* address, polyhook::DataType returnType, const plg::vector<polyhook::DataType>& arguments,
				   polyhook::CallbackHandler handler, HookState& state)
{
	if (address == nullptr) {
		return false;
	}
	if (!__polyhook_HookDetour || !__polyhook_AddCallback) {
		state.note = "polyhook bindings are not available";
		return false;
	}
	try {
		const polyhook::HookHandle hook = polyhook::HookDetour(address, returnType, arguments, -1, name);
		if (!hook) {
			state.note = "polyhook could not hook the address";
			return false;
		}
		if (!polyhook::AddCallback(hook, polyhook::CallbackType::Pre, handler)) {
			state.note = "polyhook could not attach the callback";
			return false;
		}
		state.hooked = true;
		state.address = address;
		state.hook = hook;
		state.handler = handler;
		return true;
	} catch (const std::exception& error) {
		state.note = error.what();
		return false;
	} catch (...) {
		state.note = "unknown polyhook error";
		return false;
	}
}

bool LoadGamedata()
{
	if (!__s2sdk_LoadGameConfigFile) {
		LogError("The s2sdk LoadGameConfigFile binding is missing; cs2ac cannot load its gamedata.");
		return false;
	}
	std::string location;
	if (plg::plugin::GetLocation && plg::plugin::handle != nullptr) {
		location = std::string(plg::plugin::GetLocation(plg::plugin::handle).c_str());
	}
	std::vector<std::filesystem::path> candidates;
	candidates.emplace_back(GamedataDirectory() + "/gamedata.jsonc");
	if (!location.empty()) {
		const std::filesystem::path loc(location);
		candidates.push_back(loc / "gamedata.jsonc");
		candidates.push_back(loc.parent_path() / "gamedata.jsonc");
		candidates.push_back(loc.parent_path().parent_path() / "gamedata.jsonc");
	}
	plg::vector<plg::string> paths;
	std::string checked;
	for (const std::filesystem::path& candidate : candidates) {
		if (checked.size() < 512) {
			checked += (checked.empty() ? "" : ", ") + candidate.string();
		}
		std::error_code error;
		if (std::filesystem::exists(candidate, error)) {
			paths.push_back(candidate.string().c_str());
		}
	}
	if (paths.empty()) {
		LogError("cs2ac gamedata.jsonc was not found (looked for: %s); the movement detections stay disabled.", checked.c_str());
		return false;
	}
	g_config = s2sdk::LoadGameConfigFile(paths);
	if (g_config == 0) {
		LogError("cs2ac could not load its gamedata; the movement detections stay disabled.");
		return false;
	}
	return true;
}

} // namespace

bool Initialize()
{
	if (g_initialized) {
		return true;
	}
	// Shutdown() deliberately leaves the report entries in place (cs2ac_hooks still works after a
	// stop), so a re-init must start from an empty vector: otherwise every reload appends another
	// full set of states on top of the stale ones and the report/teardown walk mixed data.
	g_states.clear();

	engine::SetBindingsAvailable(__s2sdk_GetGameTickCount != nullptr && __s2sdk_GetClientName != nullptr);
	if (!engine::BindingsAvailable()) {
		LogError("The s2sdk bindings are not available; cs2ac stays inert.");
		return false;
	}
	if (!__polyhook_HookDetour || !__polyhook_AddCallback) {
		LogError("The polyhook bindings are not available; the usercmd detections stay disabled.");
	}

	LoadGamedata();

	// Gamedata offsets (all optional).
	if (g_config != 0) {
		engine::RegisterGamedataOffset("ClientUserIdOffset", s2sdk::GetGameConfigOffset(g_config, "ClientUserIdOffset"));
	}

	// --- detours ---
	{
		HookState state {"ProcessUsercmds", false, false, ""};
		void* address = ResolveSignature("ProcessUsercmds", state);
		if (address) {
			// Slots map onto RDI, RSI, RDX, RCX, XMM0 - the original's signature (its first
			// parameter is what the engine keeps in RDI; see the enum comment above).
			plg::vector<polyhook::DataType> arguments {
				polyhook::DataType::Pointer, // RDI: CCSPlayerController*
				polyhook::DataType::Pointer, // RSI: PlayerCommand*
				polyhook::DataType::Int32,   // RDX: numcmds
				polyhook::DataType::Bool,    // RCX: paused
				polyhook::DataType::Float,   // XMM0: margin
			};
			InstallDetour("cs2ac_ProcessUsercmds", address, polyhook::DataType::Int32, arguments, &OnProcessUsercmds, state);
		}
		if (!state.note.empty() && !state.hooked) {
			LogWarning("The ProcessUsercmds hook is unavailable (%s); usercmd-based detections stay disabled.", state.note.c_str());
		}
		g_states.push_back(std::move(state));
	}
	{
		HookState state {"SetupMove", false, false, ""};
		void* address = ResolveSignature("SetupMove", state);
		if (address) {
			// Slots map onto RDI, RSI, RDX: services (the object the method is called on), command
			// and move data. There is no separate `this` slot - the original's first parameter is
			// the movement services pointer itself (member function or free function, the register
			// layout is the same).
			plg::vector<polyhook::DataType> arguments {
				polyhook::DataType::Pointer, // RDI: CCSPlayer_MovementServices*
				polyhook::DataType::Pointer, // RSI: PlayerCommand*
				polyhook::DataType::Pointer, // RDX: CMoveData*
			};
			InstallDetour("cs2ac_SetupMove", address, polyhook::DataType::Void, arguments, &OnSetupMove, state);
		}
		if (!state.note.empty() && !state.hooked) {
			LogWarning("The SetupMove hook is unavailable (%s); the SetupMove-based detections stay disabled.", state.note.c_str());
		}
		g_states.push_back(std::move(state));
	}
	// The remaining original signatures are reported for diagnostics (cs2ac_hooks) even when the
	// port does not hook them (their data is sampled per frame instead).
	for (const char* name : {"ProcessMovement", "Duck", "LadderMove", "OnJumpLegacy", "OnJumpModern", "AirMove", "AirAccelerate", "WalkMove",
							 "TryPlayerMove", "CategorizePosition", "PhysicsSimulate", "GameEventManager", "GetLegacyGameEventListener"}) {
		HookState state {name, false, false, "sampled per frame (not hooked by this port)"};
		ResolveSignature(name, state);
		state.hooked = false;
		g_states.push_back(std::move(state));
	}
	if (g_config != 0) {
		HookState state {"ClientUserIdOffset", false, false, ""};
		const i32 offset = s2sdk::GetGameConfigOffset(g_config, "ClientUserIdOffset");
		state.resolved = offset >= 0;
		// The offset is only a shortcut: the {userid} placeholder is filled from the engine userid
		// the game events carry (see engine::PlayerUserId), and the offset is trusted only after it
		// agreed with one of them.
		state.note = offset >= 0 ? FormatString("offset %d (validated against the event userid before use)", offset)
								 : "missing (user ids still come from the game events)";
		g_states.push_back(std::move(state));
	}

	// --- s2sdk listeners (the binding globals are null when s2sdk is absent) ---
	if (__s2sdk_OnGameFrame_Register) {
		s2sdk::OnGameFrame_Register(&OnGameFrameListener);
	}
	if (__s2sdk_OnMapStart_Register) {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Register) {
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	}
	if (__s2sdk_OnClientActive_Register) {
		s2sdk::OnClientActive_Register(&OnClientActiveListener);
	}
	if (__s2sdk_OnClientFullyConnect_Register) {
		s2sdk::OnClientFullyConnect_Register(&OnClientFullyConnectListener);
	}
	if (__s2sdk_OnClientDisconnect_Register) {
		s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
	}
	if (__s2sdk_OnClientSettingsChanged_Register) {
		s2sdk::OnClientSettingsChanged_Register(&OnClientSettingsChangedListener);
	}

	// --- game events (the original hooked FireEvent pre+post; the pre hook is where it dispatched) ---
	if (__s2sdk_HookEvent) {
		for (const auto& consumed : kConsumedEvents) {
			s2sdk::HookEvent(consumed.name, &OnGameEvent, s2sdk::HookMode::Pre);
		}
	}

	// --- FireBullets ---
	if (__s2sdk_OnSendNetMessage_Register) {
		s2sdk::OnSendNetMessage_Register(&OnNetMessage);
	}

	g_initialized = true;
	Log("Hooks installed (%zu gamedata entries checked).", g_states.size());
	return true;
}

void Shutdown()
{
	if (!g_initialized) {
		return;
	}
	if (__s2sdk_OnGameFrame_Unregister) {
		s2sdk::OnGameFrame_Unregister(&OnGameFrameListener);
	}
	if (__s2sdk_OnMapStart_Unregister) {
		s2sdk::OnMapStart_Unregister(&OnMapStartListener);
	}
	if (__s2sdk_OnMapEnd_Unregister) {
		s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
	}
	if (__s2sdk_OnClientActive_Unregister) {
		s2sdk::OnClientActive_Unregister(&OnClientActiveListener);
	}
	if (__s2sdk_OnClientFullyConnect_Unregister) {
		s2sdk::OnClientFullyConnect_Unregister(&OnClientFullyConnectListener);
	}
	if (__s2sdk_OnClientDisconnect_Unregister) {
		s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
	}
	if (__s2sdk_OnClientSettingsChanged_Unregister) {
		s2sdk::OnClientSettingsChanged_Unregister(&OnClientSettingsChangedListener);
	}
	if (__s2sdk_UnhookEvent) {
		for (const auto& consumed : kConsumedEvents) {
			s2sdk::UnhookEvent(consumed.name, &OnGameEvent, s2sdk::HookMode::Pre);
		}
	}
	if (__s2sdk_OnSendNetMessage_Unregister) {
		s2sdk::OnSendNetMessage_Unregister(&OnNetMessage);
	}
	// Teardown in the order polyhook 2.5.6 requires: drop *our*
	// callback first, then unhook the target only when no callback is left on it. There is exactly
	// one detour per target function, shared with every other plugin (s2sdk hooks some of the same
	// functions), so unhooking while someone else's callback is still registered would tear down
	// their hook as well - and passing the trampoline (GetOriginalAddr) as the key would find
	// nothing and leave the engine calling freed code after this module is unloaded.
	for (auto& state : g_states) {
		if (!state.hooked || state.hook == nullptr) {
			continue;
		}
		try {
			if (state.handler != nullptr && __polyhook_RemoveCallback) {
				polyhook::RemoveCallback(state.hook, polyhook::CallbackType::Pre, state.handler);
			}
			bool remaining = false;
			if (__polyhook_AreCallbacksRegistered) {
				remaining = polyhook::AreCallbacksRegistered(state.hook);
			}
			if (!remaining && state.address != nullptr && __polyhook_UnhookDetour) {
				if (polyhook::UnhookDetour(state.address)) {
					state.hooked = false;
				} else {
					LogWarning("The %s detour could not be removed; it stays installed until the server restarts.", state.name.c_str());
				}
			} else {
				// Another plugin still owns a callback on this target: our detour object goes away
				// with the module, but the shared detour must stay.
				state.hooked = false;
			}
		} catch (...) {
			LogWarning("Removing the %s detour failed with an error; it stays installed until the server restarts.", state.name.c_str());
		}
	}
	// The official s2sdk gamedata guidance: always close gamedata files - use CloseGameConfigFile()
	// in OnPluginEnd() to prevent memory leaks. Shutdown is the plugin-end path (a reload loads the
	// file again in Initialize).
	if (g_config != 0 && __s2sdk_CloseGameConfigFile) {
		s2sdk::CloseGameConfigFile(g_config);
		g_config = 0;
	}
	g_initialized = false;
}

const std::vector<HookState>& States()
{
	return g_states;
}

void PrintReport()
{
	ConsoleMessage("Hook report (%zu entries):", g_states.size());
	for (const auto& state : g_states) {
		const char* status = state.hooked ? "hooked" : state.resolved ? "resolved" : "missing";
		if (!state.note.empty()) {
			ConsoleMessage("  %-26s %-9s %s", state.name.c_str(), status, state.note.c_str());
		} else {
			ConsoleMessage("  %-26s %s", state.name.c_str(), status);
		}
	}
	ConsoleMessage("  ProcessUsercmds: %llu calls, %llu accepted command batches; SetupMove: %llu calls (%s).",
				   static_cast<unsigned long long>(g_usercmdHookCalls), static_cast<unsigned long long>(g_usercmdBatches),
				   static_cast<unsigned long long>(g_setupMoveCalls), g_setupMoveSeen ? "command parsed" : "no command parsed yet");
	if (g_usercmdHookCalls > 0 && g_usercmdBatches == 0) {
		ConsoleMessage("  WARNING: the ProcessUsercmds hook fires but no batch was accepted - the captured argument layout does not match this "
					   "build; usercmd detections are effectively off.");
	}
	const engine::SchemaStats schema = engine::SchemaStatistics();
	ConsoleMessage("  schema: movement services resolved %llu times, missing %llu times, %llu offset misses, %llu jump-data reads.",
				   static_cast<unsigned long long>(schema.servicesResolved), static_cast<unsigned long long>(schema.servicesMissing),
				   static_cast<unsigned long long>(schema.offsetMisses), static_cast<unsigned long long>(schema.jumpDataReads));
	if (schema.servicesMissing > 0 && schema.servicesResolved == 0) {
		ConsoleMessage("  WARNING: the movement services pointer never resolved - the BHOP/hyperscroll/null subtick/destubticking gates that need it "
					   "are dead (check CBasePlayerPawn::m_pMovementServices in the schema).");
	}
	// {userid}: where the engine user id comes from (the game events are the always-correct source;
	// the gamedata offset is trusted only after it agreed with one of them).
	ConsoleMessage("  client userid: %s", engine::ClientUserIdStatus().c_str());
}

} // namespace hooks
} // namespace cs2ac
