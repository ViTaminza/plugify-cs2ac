// cs2ac — shared declarations.
//
// This header is the port's "common.h": types, constants, the engine-free data structures the
// detectors consume (usercmd view, move data, shot records) and the declarations of the pieces
// that live in core.cpp. Nothing here may reference s2sdk/polyhook so that cs2ac_core stays
// linkable by the unit tests (tests/cs2ac_tests.cpp); the engine side is cs2ac.hpp.
//
// Everything is ported from the original cs2ac sources; comments marked "original:" point at the
// file the logic comes from.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The original's integer aliases (common.h).
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#define MAXPLAYERS 64

// common.h: the engine's fixed tick rate and the epsilon used across the detectors.
#define ENGINE_FIXED_TICK_INTERVAL 0.015625f
#define ENGINE_FIXED_TICK_RATE     (1.0f / ENGINE_FIXED_TICK_INTERVAL)
#define EPSILON                    0.000001f

#define CS_TEAM_NONE      0
#define CS_TEAM_SPECTATOR 1
#define CS_TEAM_T         2
#define CS_TEAM_CT        3

// sdk/datatypes.h InputBitMask_t (the bits the detectors look at).
#define IN_NONE      0x0ULL
#define IN_ATTACK    0x1ULL
#define IN_JUMP      0x2ULL
#define IN_DUCK      0x4ULL
#define IN_FORWARD   0x8ULL
#define IN_BACK      0x10ULL
#define IN_USE       0x20ULL
#define IN_TURNLEFT  0x80ULL
#define IN_TURNRIGHT 0x100ULL
#define IN_MOVELEFT  0x200ULL
#define IN_MOVERIGHT 0x400ULL
#define IN_ATTACK2   0x800ULL
#define IN_RELOAD    0x2000ULL

// bspflags.h
#define FL_ONGROUND 0x200

#define MOVETYPE_NONE   0
#define MOVETYPE_WALK   2
#define MOVETYPE_LADDER 9

// HITGROUP_* (game_constants.h)
#define HITGROUP_GENERIC 0
#define HITGROUP_HEAD    1
#define HITGROUP_CHEST   2
#define HITGROUP_STOMACH 3
#define HITGROUP_LEFTARM 4
#define HITGROUP_RIGHTARM 5
#define HITGROUP_LEFTLEG 6
#define HITGROUP_RIGHTLEG 7
#define HITGROUP_NECK    8

#define MASK_SHOT 0x400000B1

namespace cs2ac {

// ---------------------------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------------------------

struct Vector {
	f32 x {};
	f32 y {};
	f32 z {};

	constexpr Vector() = default;
	constexpr Vector(f32 ix, f32 iy, f32 iz) : x(ix), y(iy), z(iz) {}

	constexpr Vector operator+(const Vector& other) const { return {x + other.x, y + other.y, z + other.z}; }
	constexpr Vector operator-(const Vector& other) const { return {x - other.x, y - other.y, z - other.z}; }
	constexpr Vector operator*(f32 scale) const { return {x * scale, y * scale, z * scale}; }
	constexpr Vector& operator+=(const Vector& other) { x += other.x; y += other.y; z += other.z; return *this; }
	constexpr bool operator==(const Vector& other) const { return x == other.x && y == other.y && z == other.z; }
	constexpr bool operator!=(const Vector& other) const { return !(*this == other); }

	[[nodiscard]] constexpr f32 LengthSqr() const { return x * x + y * y + z * z; }
	[[nodiscard]] f32 Length() const { return std::sqrt(LengthSqr()); }
	[[nodiscard]] f32 Length2D() const { return std::sqrt(x * x + y * y); }
	void NormalizeInPlace()
	{
		const f32 length = Length();
		if (length > EPSILON) {
			x /= length;
			y /= length;
			z /= length;
		}
	}
};

struct QAngle {
	f32 x {};
	f32 y {};
	f32 z {};

	constexpr QAngle() = default;
	constexpr QAngle(f32 ix, f32 iy, f32 iz) : x(ix), y(iy), z(iz) {}
	constexpr bool operator==(const QAngle& o) const { return x == o.x && y == o.y && z == o.z; }
};

inline f32 DotProduct(const Vector& a, const Vector& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// utils/utils.cpp
f32 NormalizeDeg(f32 value);
f32 GetAngleDifference(f32 source, f32 target, f32 circle, bool relative = false);

// ---------------------------------------------------------------------------------------------
// Small string helpers (cs2ac.cpp ReplaceAll / SanitizeConsoleText, bhop's jump debug)
// ---------------------------------------------------------------------------------------------

void ReplaceAll(std::string& value, const std::string_view placeholder, const std::string_view replacement);
std::string SanitizeConsoleText(const char* value);
bool IsNumeric(const char* str);
std::string FormatString(const char* format, ...);
std::string FormatStringV(const char* format, va_list arguments);
std::string ToLower(std::string value);
std::string ToUpper(std::string value);
bool EqualsIgnoreCase(std::string_view first, std::string_view second);
// utils::CFormat: "{red}"/"{lime}"/"{grey}"/"{default}" and "{{" -> colour bytes, byte-identical to
// the original (and to s2sdk's utils::CFormat, which the print calls below run it through again).
bool CFormat(char* buffer, u64 bufferSize, const char* text);

// ---------------------------------------------------------------------------------------------
// Usercmd view (the parsed form of hl2sdk's CSGOUserCmdPB, sdk/usercmd.h)
// ---------------------------------------------------------------------------------------------

struct SubtickMove {
	bool hasButton {};
	u64 button {};
	bool pressed {};
	f32 when {};
	bool hasAnalogForwardDelta {};
	f32 analogForwardDelta {};
	bool hasAnalogLeftDelta {};
	f32 analogLeftDelta {};
	bool hasPitchDelta {};
	f32 pitchDelta {};
	bool hasYawDelta {};
	f32 yawDelta {};
};

// SVInterpInfo / CLInterpInfo of a CSGOInputHistoryEntryPB (cs_usercmd.proto): the client's
// interpolation window for that entry.
struct InterpInfo {
	bool hasSrcTick {};
	i32 srcTick {};
	bool hasDstTick {};
	i32 dstTick {};
	bool hasFrac {};
	f32 frac {};
};

// CSGOInputHistoryEntryPB fields the tick-integrity detector reads (fork v3-v5).
struct InputHistoryEntry {
	bool hasViewAngles {};
	QAngle viewAngles {};
	bool hasRenderTickCount {};
	i32 renderTickCount {};
	bool hasRenderTickFraction {};
	f32 renderTickFraction {};
	bool hasPlayerTickCount {};
	i32 playerTickCount {};
	bool hasSvInterp0 {};
	InterpInfo svInterp0;
	bool hasSvInterp1 {};
	InterpInfo svInterp1;
	bool hasClInterp {};
	InterpInfo clInterp;
};

// One usercmd as the detectors see it. "field seen" flags matter: the original distinguishes
// "missing" from "zero" (has_base(), has_viewangles(), has_button(), has_pitch_delta(), …).
struct UsercmdView {
	i32 commandNumber {};
	bool hasBase {};
	bool hasClientTick {};
	i32 clientTick {};
	bool hasViewAngles {};
	QAngle viewAngles {};
	bool hasConsumedServerAngleChanges {};
	u32 consumedServerAngleChanges {};
	u64 buttonState1 {};
	u64 buttonState2 {};
	u64 buttonState3 {};
	bool hasMouseDx {};
	i32 mouseDx {};
	bool hasMouseDy {};
	i32 mouseDy {};
	std::vector<SubtickMove> subtickMoves;
	std::vector<InputHistoryEntry> inputHistory;
	i32 attack1StartHistoryIndex {-1};
	bool hasAttack1StartHistoryIndex {};
	// Diagnostics: which wire fields the reader actually found (cs2ac_usercmd_dump).
	u32 fieldsSeen {};
};

// CInButtonState::GetButtonState (sdk/cinbuttonstate.h): the three 64-bit words are the bits of a
// three-bit down/up history per button.
constexpr u32 InButtonState(u64 state1, u64 state2, u64 state3, u64 button)
{
	return static_cast<u32>(!!(state1 & button)) + static_cast<u32>(!!(state2 & button)) * 2u
		   + static_cast<u32>(!!(state3 & button)) * 4u;
}
constexpr u32 kInButtonDownUp = 2;
constexpr u32 kInButtonUpDown = 3;
constexpr u32 kInButtonUpDownUp = 4;
constexpr u32 kInButtonDownUpDownUp = 6;

// core.cpp: protobuf wire reader for CSGOUserCmdPB (the message sits at PlayerCommand+16).
// Explicit field numbers are the CS2 wire constants for the current message layout.
// `available` is the number of readable bytes at `message` (the object's remaining bytes); the
// reader never reads past it, so a message that is shorter than the buffer cannot walk out of the
// object it is embedded in.
bool ParseUserCmdMessage(const void* message, size_t available, UsercmdView& view);

// ---------------------------------------------------------------------------------------------
// Move data (sdk/datatypes.h CMoveData / CMoveDataBase) — only the fields the port reads.
// The layout is asserted at startup against the engine's own size via gamedata (CmdDataSize).
// ---------------------------------------------------------------------------------------------

struct TouchListEntry {
	Vector deltaVelocity;
};

struct MoveDataView {
	QAngle absViewAngles;
	QAngle viewAngles;
	Vector lastMovementImpulses;
	f32 forwardMove {};
	f32 sideMove {};
	f32 upMove {};
	Vector velocity;
	QAngle angles;
	Vector absOrigin;
	f32 surfaceFriction {};
	int tickCount {};
	int targetTick {};
	f32 subtickStartFraction {};
	f32 subtickEndFraction {};
	Vector outWishVel;
	Vector oldAnglesUnused;
	f32 maxSpeed {};
	bool inAir {};
	bool gameCodeMovedPlayer {};
	std::vector<SubtickMove> subtickMoves;
	std::vector<SubtickMove> attackSubtickMoves;
	bool hasSubtickInputs {};
	// Diag: the sizeof(CMoveData) the running game reports through the detour (0 when unknown).
	u32 engineMoveDataSize {};
};

// ---------------------------------------------------------------------------------------------
// Player handle (player/player.h Player, but engine reads go through engine.cpp)
// ---------------------------------------------------------------------------------------------

class PlayerRef {
public:
	explicit PlayerRef(i32 index) : index(index) {}

	// player/player.h
	[[nodiscard]] bool IsConnected() const;
	[[nodiscard]] bool IsInGame() const;
	[[nodiscard]] bool IsFakeClient() const;
	[[nodiscard]] bool IsCSTV() const;
	[[nodiscard]] bool IsAlive() const;
	[[nodiscard]] const char* GetName() const;
	[[nodiscard]] u64 GetSteamId64(bool validated = true) const;
	[[nodiscard]] i32 GetPlayerSlot() const { return index - 1; }

	const i32 index;
};

class PlayerManager {
public:
	PlayerRef* ToPlayer(i32 index);
	PlayerRef* ToPlayerBySlot(i32 slot) { return ToPlayer(slot + 1); }
};

extern PlayerManager* g_pPlayerManager;

// ---------------------------------------------------------------------------------------------
// Localization (localization.cpp)
// ---------------------------------------------------------------------------------------------

namespace localization {
struct Argument {
	std::string_view name;
	std::string value;
};

using Arguments = std::initializer_list<Argument>;

struct Text {
	std::string english;
	std::string localized;
};

// translationsDir is the directory holding the original <lang>.txt phrase files
// (<configs>/cs2ac/translations); the file format and the fallback rules are the original's.
void Reload(const char* language, const std::string& translationsDir);
void Shutdown();
const char* CurrentLanguage();
std::string Get(const char* key, const char* english);
Text Format(const char* key, const char* english, Arguments arguments = {});
Text Watermark(Arguments arguments = {});
// Test hooks.
std::size_t LoadedPhraseCount();
bool HasPhrase(const char* key);
// Where the one-time "some phrases fell back to English" warning is reported.
void SetWarningSink(std::function<void(const std::string&)> sink);
} // namespace localization

// ---------------------------------------------------------------------------------------------
// Settings (settings.cpp) — same names, defaults and semantics as the original ConVars.
// ---------------------------------------------------------------------------------------------

enum class DetectionType : std::uint8_t {
	Aimbot,
	Aimlock,
	AntiAim,
	Autostrafe,
	Bhop,
	DllInjection,
	Desubticking,
	Doubletap,
	Hyperscroll,
	InhumanAccuracy,
	InvalidCvar,
	InvalidInput,
	IrregularBehavior,
	NameChanger,
	Nulls,
	SilentAim,
	SubtickSpam,
	Triggerbot,
	Count,
};

// The original's detectionNames[] (cs2ac.cpp).
extern const char* const kDetectionNames[static_cast<size_t>(DetectionType::Count)];

namespace settings {
// ConVar names, in the order they appear in the original's Configuration struct.
struct ConVarSpec {
	const char* name;
	const char* defaultValue; // "1"/"0" for booleans, the string value otherwise
	const char* description;
	bool isString;
};

constexpr size_t kConVarCount = 49;
extern const ConVarSpec kConVars[kConVarCount];

// Typed values for the fork's numeric settings (the original declared CConVar<int>/<float>).
struct NumericConVarSpec {
	const char* name;
	bool isFloat;
	const char* defaultValue;
	const char* description;
};
constexpr size_t kNumericConVarCount = 22;
extern const NumericConVarSpec kNumericConVars[kNumericConVarCount]; // config + detection switches

// Every cs2ac_*_debug switch (the original's per-module CConVar<bool> diagnostics, default false;
// the names AND descriptions are the ones the original declared next to each detection module).
constexpr size_t kDebugConVarCount = 19;
struct DebugConVarSpec {
	const char* name;
	const char* description;
};
extern const DebugConVarSpec kDebugConVars[kDebugConVarCount];

// Runtime configuration state (mirrors the original's Configuration + settings:: accessors).
void Reset();
bool IsPluginEnabled();
bool IsDetectionEnabled(DetectionType detection);
bool IsPlayerWhitelisted(u64 steamId);
std::size_t GetWhitelistCount();
std::size_t GetRejectedWhitelistCount();
std::size_t GetDuplicateWhitelistCount();
std::size_t GetEnabledDetectionCount();
u64 GetDetectionMask();
u64 GetRevision();
bool ShowChatAnnouncements();
bool ShowCenterAnnouncements();
bool ShowJoinWatermark();
bool AutomaticUpdatesEnabled();
bool WeakSignalsKick();
const char* GetUpdateRepository();
// Fork settings introduced by v3-v11 (see config.cpp). The plugin layer keeps them in sync with the
// registered ConVars exactly like the original's CConVar reads.
bool BoolSetting(const char* name, bool defaultValue);
f32 FloatSetting(const char* name, f32 defaultValue);
int IntSetting(const char* name, int defaultValue);
const char* GetPunishmentCommand();
const char* GetKickCommand();
const char* GetWebhookUrl();
const char* GetWebhookRoleId();
const char* GetWebhookServerAddress();
const char* GetWebhookLogoUrl();
const char* GetLanguage();
bool AllowSvCheatsTesting();
bool DebugFlag(const char* convarName);
// The value the config file/diagnostics set (without the live-ConVar resolver below).
bool DebugFlagValue(const char* convarName);
// The plugin layer installs a resolver so a console change (`cs2ac_aimbot_debug 1`) is visible
// immediately, like the original's ConVar reads.
void SetDebugResolver(std::function<bool(const char*)> resolver);

// Applied by the config loader when a setting changes (the original's ConVar change callbacks).
void SetDetectionSetting(DetectionType detection, bool value);
void SetEnabled(bool value);
void SetAnnouncements(bool chat, bool center);
void SetWhitelist(const char* value);
void SetString(const char* name, const char* value);
void SetBool(const char* name, bool value);
void SetInt(const char* name, int value);
void SetFloat(const char* name, f32 value);
void MarkConfigReloaded();
void BumpRevision();
// Reads a setting's current raw string value (used to report the config summary).
const char* RawValue(const char* name);
} // namespace settings

namespace cvar {
// The cvar system's invalid ConVarRef handle. s2sdk's FindConVar returns it for a name it cannot
// resolve (s2sdk logs "Invalid ConVar handle: 4294967295" for it), and passing it to any
// s2sdk cvar function makes s2sdk print that warning on every call. The engine's ConVarRef is a
// 32-bit handle, so both the zero-extended and the sign-extended spellings are rejected.
constexpr u64 kInvalidHandle = 0xFFFFFFFFull;
bool IsValidHandle(u64 handle);
// The engine's own truth spellings for a bool cvar ("1"/"true"/"yes", case-insensitive): used both
// for the defaults in the convar table and for reading cvars whose engine type is a string.
bool BoolFromString(const char* value);
} // namespace cvar

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; the engine-side
// PawnHandle() answers -1 for "no pawn"). The sign means nothing - once the engine's serial
// counter passes 65535 bit 31 is set and a valid handle is a negative int; a `handle >= 0` test
// then silently takes the pointer fallback for a pawn whose handle is perfectly usable. Not to be
// confused with cvar::IsValidHandle above, which is about ConVar handles.
inline bool IsValidHandle(i32 h) { return h != -1 && h != 0; }

// ---------------------------------------------------------------------------------------------
// Per-frame memo (engine.cpp's player snapshot)
// ---------------------------------------------------------------------------------------------

// One player-state value that is fetched at most once per server frame and reused by every reader
// of that frame. The plugin's frame loop (Plugin::OnGameFrame) reads the same fields in several
// passes - the teleport sampler, the shot correlator's position frame, the network sampler, five
// detector modules and the movement sampler - and every one of those reads is a cross-plugin
// s2sdk call. The plugin changes no engine state while it runs, so the value cannot change between
// those passes: fetching it once and handing the same value to every reader passes each detector
// exactly the input a fresh read would have returned.
//
// `filled` is what makes the sharing observable to the tests: the fetch functor runs only on the
// first Get() after the memo was cleared (the engine clears the whole per-slot record once per
// frame by assigning {}) and never for a reader that only finds the value already there.
template <typename T>
struct FrameMemo {
	T value {};
	bool filled {};

	template <typename Fetch>
	const T& Get(Fetch&& fetch)
	{
		if (!filled) {
			filled = true;
			value = fetch();
		}
		return value;
	}
};

namespace engine {
// An engine user id is a small positive integer (CS2 assigns them per connection; 0 means
// "no player", e.g. in the game events). Used to sanity-check both the userids events report and
// the raw read of CServerSideClientBase::m_UserID, so a wrong gamedata offset cannot feed a random
// number into a punishment command.
bool PlausibleUserId(i64 value);
} // namespace engine

namespace usercmd {
// Plausibility gates of the ProcessUsercmds detour (hooks.cpp). The engine hands over a contiguous
// PlayerCommand array whose stride (sizeof(PlayerCommand) = 144 on Linux) is a layout assumption;
// a wrong stride/offset produces command numbers that are out of range or not strictly increasing
// within one batch. Such a batch is dropped (fail soft) instead of being fed to the detectors.
constexpr i32 kMaxCommandNumber = 1 << 24;
bool IsPlausibleCommandNumber(i32 number);
bool IsStrictlyIncreasing(const std::vector<i32>& numbers);
} // namespace usercmd

namespace movement {
// MovementEventService::OnJumpModern (movement_analysis/events/movement_events.cpp):
//   lastUsableJumpPress = m_nLastUsableJumpPressTick + m_flLastUsableJumpPressFrac
//   modernLandTime      = m_nLastLandedTick         + m_flLastLandedFrac
//   mightBeModernPerf   = lastUsableJumpPress < modernLandTime
// The data comes from the pawn's movement services (engine::PlayerFramePerfectJump); this helper is
// the pure part so the comparison and its edge cases (equal times, negative fractions) are unit
// tested without an engine.
f32 JumpEventTime(i32 tick, f32 fraction);
bool IsFramePerfectJump(i32 pressTick, f32 pressFraction, i32 landTick, f32 landFraction);
} // namespace movement

// The cs2ac.cfg parser (config.cpp): "name value" lines, "//" comments, quoted values, and the
// trailing cs2ac_config_loaded marker. Returns the parsed entries in file order.
struct ConfigLine {
	std::string name;
	std::string value;
	u32 line {};
};
std::vector<ConfigLine> ParseConfigFile(const std::string& contents);
// Original CheckCommandTemplate (cs2ac.cpp) — returns the number of findings and prints them.
int CheckCommandTemplate(const char* settingName, const char* commandTemplate, bool requireSteamId);

// ---------------------------------------------------------------------------------------------
// Detection data structures (detection/detection_system.h, verbatim semantics)
// ---------------------------------------------------------------------------------------------

namespace detection {
using Clock = std::chrono::steady_clock;

// localization::Text + the raw English reason, exactly as the original's AnnounceCallback.
using AnnounceCallback = std::function<void(const char* detection, PlayerRef* player, const localization::Text& evidence)>;

bool IsBallisticWeapon(std::string_view weapon);
// detection_system.cpp TeammatesAreEnemies(): the plugin sets this from mp_teammates_are_enemies.
void SetTeammatesAreEnemies(bool value);
bool TeammatesAreEnemies();
bool IsFinite(const QAngle& angles);
bool IsFinite(const Vector& vector);
Vector AimForward(const QAngle& angles);
f32 AngularDistance(const QAngle& first, const QAngle& second);
std::string_view NormalizeWeapon(std::string_view weapon);
bool AreOpponents(int firstTeam, int secondTeam);
bool IsEligibleHuman(PlayerRef* player);

struct NetworkSafetyEvidence {
	f32 pingMilliseconds {};
	f32 jitterMilliseconds {};
	f32 incomingLoss {};
	f32 outgoingLoss {};
	f32 incomingChoke {};
	f32 outgoingChoke {};
	int commandGaps {};
	int unavailableSamples {};
	bool vetoed {};
};

// network_safety.cpp: ShouldVeto and CaptureSample feed the unit tests.
bool ShouldVeto(f32 ping, f32 jitter, f32 incomingLoss, f32 outgoingLoss, f32 incomingChoke, f32 outgoingChoke, int unavailableSamples);
localization::Text AddNetworkSafetyDetails(localization::Text details, const NetworkSafetyEvidence& evidence);

// Triggerbot's reaction scoring (triggerbot_module.cpp) — pure, unit-tested.
struct ReactionScore {
	int score {};
	int oneTickCount {};
	int twoTickCount {};
	int normalCount {};
};
constexpr int kTriggerbotDetectionThreshold = 10;
template<size_t N>
constexpr ReactionScore ScoreReactions(const std::array<int, N>& ticks, size_t count)
{
	ReactionScore result;
	for (size_t index = 0; index < count && index < N; ++index) {
		const int tick = ticks[index];
		if (tick >= 0 && tick <= 1) {
			++result.oneTickCount;
			result.score += 2;
		} else if (tick == 2) {
			++result.twoTickCount;
			result.score = (std::max)(0, result.score - 3);
		} else if (tick >= 3) {
			++result.normalCount;
			result.score = (std::max)(0, result.score - 3);
		}
	}
	return result;
}

// Aimbot's smoothed-aim envelope (aimbot_module.cpp) — pure, unit-tested.
struct SmoothThresholds {
	f32 movement {};
	f32 improvement {};
	f32 finalError {};
	f32 efficiency {};
	int steps {};
	bool shrinking {};
};
constexpr bool MeetsSmoothThresholds(f32 movement, f32 improvement, f32 finalError, f32 efficiency, int steps, bool shrinking)
{
	return movement >= 13.0f && improvement >= 0.94f && finalError <= 1.0f && efficiency >= 0.94f && steps >= 3 && shrinking;
}

// AntiAim's sustained-spin/jitter helpers (antiaim_module.cpp) — pure, unit-tested.
f32 YawDelta(f32 from, f32 to);
constexpr bool MeetsCoverage(int onTarget, int samples)
{
	return samples > 0 && onTarget * 20 >= samples * 19;
}
f32 LerpF(f32 t, f32 minimum, f32 maximum);
u32 LerpU32(f32 t, u32 minimum, u32 maximum);

// Nulls' required-perfect-cstrafes scaling (nulls.cpp) — pure, unit-tested.
struct NullsTiming {
	f32 medianFramerate {};
	f32 maxFps {};
	f32 underlapMedian {};
	std::size_t underlapCount {};
	std::size_t total {};
};
u32 NullsRequiredPerfectCstrafes(const NullsTiming& timing, std::size_t eventCount);
bool NullsUnderlapForgiven(const NullsTiming& timing);
u32 NullsAdjustedRequiredPerfectCstrafes(u32 required, f32 underlapMedian);

// Invalid CVar's client-cvar cycle (cvars.cpp) — pure, unit-tested. The original drew
// cvarCycleInterval = RandomFloat(1.0f, 5.0f) at the start of every round of the queried cvars and
// re-armed its repeating timer with cvarCycleInterval / kCvarQueryCount, so one cvar was queried
// per CvarQueryDelay() and a full round took cvarCycleInterval seconds.
constexpr std::size_t kCvarQueryCount = 10;
f64 CvarCycleInterval(int playerIndex);
f64 CvarQueryDelay(f64 cvarCycleInterval);

// ---------------------------------------------------------------------------------------------
// Fork v3-v11 decisions that are pure and therefore unit-tested (decisions.cpp).
// ---------------------------------------------------------------------------------------------

// Comma-separated, case-insensitive detection lists (`cs2ac_ban_detections`, …).
bool ListContains(const char* list, std::string_view item);

// Punishment policy (fork cs2ac.cpp SeverityForDetection). `kickOnly` is the movement modules'
// per-finding flag; RCS is report-only regardless.
enum class DetectionSeverity : u8 {
	Report,
	Kick,
	Ban,
};
bool IsReportOnlyDetection(std::string_view detection);
DetectionSeverity SeverityForDetection(std::string_view detection, bool kickOnly, const char* banList, const char* kickList,
									   bool weakSignalsKick);

// Rolling-window escalation counters shared by stream/burst kicks and the announce throttle.
// Returns true when the window holds >= threshold entries after adding `now`.
bool WindowedCountReached(std::deque<i64>& times, i64 nowSeconds, int threshold, int seconds);

// MULTIPLE SIGNALS: adds the detection to the connection's set unless it is ignored, and reports
// whether the set reached `needed`.
bool ComboReached(std::set<std::string>& kinds, std::string_view detection, const char* ignoreList, int needed);

// Mass guard (fork v11): one entry per distinct SteamID within the window; returns true when the
// number of distinct players reaches `limit` (the caller then keeps the detection demoted).
struct MassGuardHit {
	i64 seconds {};
	u64 steamId {};
};
bool MassGuardReached(std::deque<MassGuardHit>& hits, i64 nowSeconds, int windowSeconds, u64 steamId, int limit);

// Repeat ban (fork v10): a second kick for a listed detection within the window bans; detections in
// `needsOther` additionally require another finding in the window (BHOP, NULLS and RCS do not count).
bool RepeatBanReached(std::deque<i64>& kicks, const std::unordered_map<std::string, i64>& findings, std::string_view detection,
					  const char* repeatList, const char* needsOther, i64 nowSeconds, int windowSeconds);

// Reconciliation data of a shot entry, captured when the shot command arrives (fork v5).
struct ShotInterpInfo {
	bool known {};
	f64 offset {};
	i32 src0 {-1};
	i32 dst0 {-1};
	f32 frac0 {};
	i32 src1 {-1};
	i32 dst1 {-1};
	f32 frac1 {-1.0f};
	f32 clFrac {-1.0f};
};

// Tick-integrity math (fork tick_integrity_module.cpp).
constexpr f64 kTickNone = -1.0;
f64 TickRenderTime(bool hasTick, i32 tick, bool hasFraction, f32 fraction);
f64 TickInterpTime(const InterpInfo& interp);
// How far `shot` lies below both neighbours (negative: no dip); NaN without context.
f64 TickDip(f64 previous, f64 shot, f64 next);
bool TickHasFraction(f64 time);
bool TickRewriteFingerprint(bool wholeTick, f64 interpTime);
std::size_t TickDipBucket(f32 dipTicks);
std::size_t TickInterpOffsetBucket(f32 offsetTicks);
std::size_t TickExcessBucket(f32 value, const f32* limits, std::size_t count);
// Whether a dip >= minTicks counts toward BACKTRACK. `gap` is the clock movement around the shot.
bool BacktrackDipCounts(f32 dipTicks, f32 minTicks, f64 gap, f32 maxGap, bool forged, bool requireForged, bool previousFraction,
						bool nextFraction);

// SILENTAIM scoring (fork v7/v7.1).
constexpr f32 kSilentAimMinimumCountedExcess = 0.25f;
constexpr f32 kSilentAimStillCameraTurn = 0.5f;
constexpr f32 kSilentAimFlickHalfExcess = 2.0f;
constexpr f32 kSilentAimBlatantExcess = 22.5f;
int SilentAimPoints(f32 excess, f32 cameraTurn, bool airborne, bool headshot, bool wallbang, bool smokeBonus, bool noscope);
std::size_t SilentAimExcessBucket(f32 excess);
std::size_t SilentAimCameraBucket(f32 cameraTurn);

// TRIGGER PATTERN (fork trigger_pattern_module.cpp).
std::size_t TriggerPatternHoldBucket(f32 milliseconds);
int TriggerPatternMedian(std::vector<int> values);

// RCS (fork recoil_control_module.cpp).
bool IsAutomaticWeapon(std::string_view weapon);
f32 RecoilAngleDelta(f32 to, f32 from);
f32 RecoilCorrelation(const std::vector<f32>& x, const std::vector<f32>& y);
bool RecoilSprayQualifies(bool invalid, int maxShots, std::size_t samples, std::string_view weapon);
struct RecoilSprayResult {
	f32 bestCorrelation {};
	f32 gain {};
	int bestLag {};
};
bool RecoilSuspiciousShare(const std::deque<RecoilSprayResult>& recent, f32 threshold, f32 suspiciousShare);

// Minimal JSON string literal for attacker-controlled text (detection log).
std::string JsonString(const char* value);

struct TrackedPosition {
	Vector origin;
	Vector eyePosition;
	int team {};
	bool valid {};
	bool alive {};
	bool teleported {};
};

struct PositionFrame {
	int serverTick {-1};
	std::array<TrackedPosition, MAXPLAYERS + 1> players;
};

struct ShotCommand {
	int commandNumber {};
	int clientTick {};
	int serverTick {-1};
	QAngle baseAngles;
	QAngle attackAngles;
	Vector eyePosition;
	f32 subtickPitchTravel {};
	f32 subtickYawTravel {};
	QAngle previousBaseAngles;
	int mouseDx {};
	int mouseDy {};
	bool airborne {};
	bool scoped {};
	bool simulated {};
	bool consumed {};
	bool subtickAnglesValid {true};
	bool previousBaseValid {};
};

struct ShotRecord {
	u64 id {};
	u32 generation {};
	int playerIndex {};
	int commandNumber {};
	int clientTick {};
	int serverTick {-1};
	int fireTick {-1};
	QAngle angles;
	QAngle baseAngles;
	QAngle silentPreviousBaseAngles;
	Vector eyePosition;
	std::string weapon;
	Clock::time_point fireTime;
	f32 silentDeviation {};
	f32 silentAllowance {};
	f32 silentInaccuracy {};
	f32 silentSpread {};
	f32 silentSubtickPitchTravel {};
	f32 silentSubtickYawTravel {};
	f32 silentUnsupportedDeviation {};
	f32 silentAdjacentPathExcess {};
	u32 silentWeaponId {};
	int silentMouseDx {};
	int silentMouseDy {};
	int victimIndex {-1};
	bool airborne {};
	bool scoped {};
	bool hurtSeen {};
	bool deathSeen {};
	bool headshot {};
	bool wallbang {};
	bool throughSmoke {};
	bool smokeClearedByHe {};
	bool silentFireSeen {};
	bool silentHitSeen {};
	bool silentRejected {};
	bool silentMovementSupported {};
	bool silentSubtickAnglesValid {true};
	bool silentPreviousBaseValid {};
	bool aimbotConsumed {};
	bool silentMeasured {};
	bool silentConsumed {};
	bool irregularConsumed {};
	bool accuracyConsumed {};
};

struct ShotPlayerData {
	std::deque<ShotCommand> commands;
	std::deque<ShotRecord> shots;
	u32 generation {1};
	int lastViewCommandNumber {-1};
	QAngle lastViewBaseAngles;
	bool lastViewValid {};
};

// Event data mirroring the fields the original reads off IGameEvent (events are read once in the
// hook layer, so the detectors stay free of s2sdk).
struct GameEventData {
	std::string name;
	int userid {-1};
	int attacker {-1};
	std::string weapon;
	int hitgroup {HITGROUP_GENERIC};
	int dmgHealth {};
	int smokeEntityId {-1};
	bool penetrated {};
	bool thrusmoke {};
	bool attackerinair {};
	bool attackerblind {};
	bool noscope {};
	bool headshot {};
	f32 distance {};
	f32 smokeX {};
	f32 smokeY {};
	f32 smokeZ {};
};

class ShotCorrelator {
public:
	void Reset();
	void OnClientDisconnect(PlayerRef* player);
	void OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands);
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void CaptureFrame(int currentTick);
	void Prune(int currentTick);

	ShotRecord* OnWeaponFire(const GameEventData& event, PlayerRef* player, int currentTick);
	ShotRecord* OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField, std::string& missing);
	ShotRecord* OnPlayerHurt(const GameEventData& event, PlayerRef* victim, int currentTick);
	ShotRecord* OnPlayerDeath(const GameEventData& event, PlayerRef* victim, int currentTick);

	const PositionFrame* FindFrame(int serverTick) const;
	const TrackedPosition* FindPosition(int serverTick, int playerIndex) const;
	std::deque<ShotRecord>& GetShots(int playerIndex);

	// Test seam: lets the unit tests inject player positions without an engine.
	std::function<void(int playerIndex, TrackedPosition& out)> captureOverride;

private:
	ShotRecord* MatchEvent(PlayerRef* player, std::string_view weapon, int currentTick);
	static void AdvanceGeneration(ShotPlayerData& data);

	std::array<ShotPlayerData, MAXPLAYERS + 1> playerData;
	std::deque<PositionFrame> positionFrames;
	// GetShots returns a reference; an out-of-range index gets this fallback (one per correlator,
	// so callers never share a hidden static object).
	std::deque<ShotRecord> emptyShots;
	u64 nextShotId {1};
};

class NetworkSafetyMonitor {
public:
	struct Sample {
		Clock::time_point time;
		f32 pingMilliseconds {};
		f32 incomingLoss {};
		f32 outgoingLoss {};
		f32 incomingChoke {};
		f32 outgoingChoke {};
		bool valid {};
		bool lossChokeAvailable {};
	};

	struct PlayerData {
		int lastCommandNumber {-1};
		int lastClientTick {-1};
		Clock::time_point nextSample;
		std::deque<Clock::time_point> commandGaps;
		std::deque<Sample> samples;
	};

	void Reset();
	void OnGameFrame();
	void OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands);
	NetworkSafetyEvidence Evaluate(PlayerRef* player);
	void OnClientDisconnect(PlayerRef* player);

	// Set once when loss/choke readings are unavailable (logged by the caller).
	bool lossChokeUnavailableNoted {};

private:
	static void Prune(PlayerData& data, Clock::time_point now);

	std::array<PlayerData, MAXPLAYERS + 1> playerData;
};

} // namespace detection
} // namespace cs2ac
