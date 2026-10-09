// cs2ac — the plugin-facing half of the port: engine accessors (s2sdk), hooks (polyhook + s2sdk
// listeners), announcements, the webhook queue and the plugin object that ties the detection
// modules to the engine events (the original's cs2ac.h + utils/hooks.h + utils/utils.h).
//
// Everything in here is implemented by engine.cpp / hooks.cpp / announce.cpp / webhook.cpp and is
// only linked into the plugin (never into the tests).

#pragma once

#include "core.hpp"

// The plugin-side header is only used by the plugin translation units (never by the tests), so the
// s2sdk types the command callbacks need are declared here.
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <polyhook/polyhook.hpp>
#include <s2sdk/s2sdk.hpp>

namespace cs2ac {

// ---------------------------------------------------------------------------------------------
// Logging (the original's Msg/Warning, routed to the Plugify log and the server console)
// ---------------------------------------------------------------------------------------------

void Log(const char* format, ...);
void LogWarning(const char* format, ...);
void LogError(const char* format, ...);
// Console-only (the original's Msg): prefixed with [CS2AC], no severity mapping.
void ConsoleMessage(const char* format, ...);

// ---------------------------------------------------------------------------------------------
// engine: every s2sdk read/write the rest of the plugin needs
// ---------------------------------------------------------------------------------------------

namespace engine {

bool BindingsAvailable();
void SetBindingsAvailable(bool available);
bool GameReady();
void SetGameReady(bool ready);

void OnMapStart();
void OnMapEnd();

int CurrentTick();
f64 CurTime();
f32 FrameTime();
f64 Realtime();

// Per-player state (the original read these off CServerSideClient / the schema).
bool PlayerConnected(int index);
bool PlayerInGame(int index);
bool PlayerFakeClient(int index);
bool PlayerCSTV(int index);
bool PlayerAlive(int index);
// "The client is in game and has a pawn right now" - the gate for every s2sdk export that needs the
// pawn (they answer a neutral default and print "invalid player pawn" without one). Answers without
// printing anything in either negative case; the frame samplers use it to skip pawn-less clients.
bool PlayerHasPawn(int index);
const char* PlayerName(int index);
u64 PlayerSteamId(int index);
int PlayerTeam(int index);
int PlayerHealth(int index);
Vector PlayerOrigin(int index);
Vector PlayerEye(int index);
Vector PlayerVelocity(int index);
int PlayerMoveType(int index);
int PlayerActualMoveType(int index);
bool PlayerOnGround(int index);
bool PlayerOnGroundLastTick(int index);
bool PlayerScoped(int index);
bool PlayerDucking(int index);
int PlayerGroundEntity(int index);
Vector PlayerLastMovementImpulses(int index);
int PlayerUserId(int index);
// Records the engine userid a game event reported for a slot (hooks::ReadEvent) - the reference the
// ClientUserIdOffset gamedata value is validated against before it is trusted. Forget clears it when
// the slot's client disconnects (slot reuse).
void RememberClientUserId(int index, int userId);
void ForgetClientUserId(int index);
int PlayerActiveWeapon(int index);
// Player's view angles (v_angle / m_angEyeAngles), used where the original read the schema.
QAngle PlayerViewAngles(int index);
// The original's "frame-perfect jump" signal: the modern-jump data says the last usable jump press
// happened at or before the last landing (MovementEventService::mightBeModernPerf) — the engine
// consumed the press in the same tick as the landing, which is what the BHOP detector scores.
bool PlayerFramePerfectJump(int index);

// The client's own teleport-relevant timestamp: the original tracked teleports through a manual
// hook. Here the frame sampler records position discontinuities and exposes "teleported within the
// last X seconds" with the same thresholds.
bool PlayerJustTeleported(int index, f32 thresholdSeconds);

// Network quality. ping/jitter are always available; loss/choke would need net-channel offsets that
// no code here resolves (ClientNetStats fills only the ping), so lossChokeAvailable is always false
// until a net-channel source is ported.
struct NetStats {
	f32 pingSeconds {};
	f32 incomingLoss {};
	f32 outgoingLoss {};
	f32 incomingChoke {};
	f32 outgoingChoke {};
	f32 remoteFrametime {-1.0f};
	bool pingAvailable {};
	bool lossChokeAvailable {};
};
NetStats ClientNetStats(int index);

// Client console variables: the userinfo-style value (the original's GetClientConVarValue) and the
// asynchronous query service (the original's IClientCvarValue, which s2sdk implements).
const char* ClientCvarValue(int index, const char* name);
using ClientCvarCallback = void (*)(int slot, int status, const char* name, const char* value, void* user);
bool QueryClientCvar(int index, const char* name, ClientCvarCallback callback, void* user);

// Server console variables (the original's ConVarRefAbstract reads). Missing cvars and invalid
// handles (s2sdk's FindConVar returns 0xFFFFFFFF for a name it cannot resolve) are cached as
// "absent" and logged once - calling an s2sdk cvar function with such a handle makes s2sdk print
// "Invalid ConVar handle" on every call, which spammed the console at per-frame rates.
bool ServerCvarBool(const char* name, bool defaultValue);
f32 ServerCvarFloat(const char* name, f32 defaultValue);
int ServerCvarInt(const char* name, int defaultValue);
bool ServerCvarExists(const char* name);
// Per-frame cached views of the two cvars the detectors poll on hot paths.
bool SvCheatsEnabled();
bool SvCheatsAvailable();
bool TeammatesAreEnemiesCvar();
// Invalidates the per-frame cvar cache (called once per server frame by the plugin).
void OnFrameRefresh();
// Hands queued client-cvar replies to their handlers (called from the frame pump, never from inside
// the engine's cvar dispatch).
void DrainCvarReplies();
// Re-resolves every cached cvar name (called when the plugin's configuration is reloaded).
void InvalidateConVarCache();
// The raw registered value ("" when the cvar does not exist); used to resync the plugin's settings
// when an admin changes a cs2ac_* ConVar at runtime, exactly like the original's ConVar callbacks.
std::string ServerCvarString(const char* name);
// Registered by the plugin (the original's ConVar_Register). The types match the original's
// declarations: RegisterConVar is a CConVar<bool>, RegisterStringConVar a CConVar<CUtlString>
// (the port's own cs2ac_allow_sv_cheats_testing is a bool as well). The `protectedValue` argument
// is the FCVAR_PROTECTED flag the original put on the Discord webhook URL only.
bool RegisterConVar(const char* name, const char* defaultValue, const char* description);
bool RegisterStringConVar(const char* name, const char* defaultValue, const char* description, bool protectedValue);
bool SetCvarValue(const char* name, const char* value);

void ServerCommand(const char* command);

// Counters for the schema reads the movement detectors depend on, printed by cs2ac_hooks: the
// movement services pointer used to be read through the entity-handle accessor (invalid for a
// pointer field), which left every movement detector silently dead with no other symptom.
struct SchemaStats {
	u64 servicesResolved {};
	u64 servicesMissing {};
	u64 offsetMisses {};
	u64 jumpDataReads {};
};
SchemaStats SchemaStatistics();

// Gamedata-driven offsets (assets/gamedata.jsonc). The hook layer reads them once and registers
// them here; a missing entry only disables the feature that needs it.
void RegisterGamedataOffset(const char* name, i32 offset);
// One line describing where the {userid} placeholder gets its value from (cs2ac_check_config).
std::string ClientUserIdStatus();
// Once per frame per connected player: records teleport-scale position discontinuities (the
// original's CBaseEntity::Teleport hook) for that player.
void UpdateTeleportTrackingFor(int index);
void ForgetTeleportTracking(int index);
// A spawn/respawn counts as a teleport for the detectors' grace windows.
void MarkSpawned(int index);

// Crosshair trace for the Triggerbot detector (the original's INavPhysicsInterface::TraceLine).
struct TraceResult {
	bool hit {};
	int hitEntityIndex {0};
	Vector endPosition;
};
bool TraceLine(const Vector& start, const Vector& end, int ignoreEntityIndex, TraceResult& result);

// The game's own event manager: only the DLL Injection detector needs it (vtable call, gated by
// gamedata; see PORT_PLAN.md §3 #6).
bool LegacyListenerSubscribes(int index, const char* eventName, bool& subscribed);

// Position frame sampling support: the original's Player::GetEyeOrigin.
bool ReadEyeOrigin(int index, Vector& out);

} // namespace engine

// ---------------------------------------------------------------------------------------------
// hooks: polyhook detours + s2sdk listeners + event/netmessage fan-out
// ---------------------------------------------------------------------------------------------

namespace hooks {

struct HookState {
	std::string name;
	bool resolved {};
	bool hooked {};
	std::string note;
	// Teardown bookkeeping: polyhook keeps ONE detour per target function shared by every plugin,
	// so Shutdown() removes only this plugin's callback and unhooks the target only when no callback
	// is left on it (unhooking unconditionally would tear down s2sdk's/another plugin's hook too).
	void* address {};
	polyhook::HookHandle hook {};
	polyhook::CallbackHandler handler {};
};

// Installs everything; returns false only when the plugin cannot work at all (no polyhook/s2sdk).
bool Initialize();
void Shutdown();

const std::vector<HookState>& States();
// Diagnostics for cs2ac_hooks.
void PrintReport();

} // namespace hooks

// ---------------------------------------------------------------------------------------------
// announce: chat, center screen, punishment commands and the console evidence lines
// ---------------------------------------------------------------------------------------------

namespace announce {

enum class DetectionOutcome : u8 {
	PunishmentSent,
	Whitelisted,
	PunishmentDisabled,
	IdentityUnavailable,
	AlreadyPunished,
	CommandTooLong,
	CommandServiceUnavailable,
	NetworkUnstable,
};

std::string OutcomeText(DetectionOutcome outcome);
void AnnounceDetection(const char* detection, const char* playerName, DetectionOutcome outcome);
void AnnounceTest();
// The join watermark (chat + center screen), per player slot.
void AnnounceWatermarkTo(int slot, bool centerOnly);
void ClearWatermarkFor(int slot);
bool IsDetectionAnnouncementActive();
void ResetDetectionAnnouncement();
// Center-message queue pumping (the original's StartTimer(ResendDetectionHtml, 0.1)).
void OnGameFrame();

} // namespace announce

// ---------------------------------------------------------------------------------------------
// webhook: the Discord report queue (webhook.cpp). Pumped from OnPluginUpdate, never blocking the
// game thread.
// ---------------------------------------------------------------------------------------------

class WebhookService {
public:
	WebhookService();
	~WebhookService();
	WebhookService(const WebhookService&) = delete;
	WebhookService& operator=(const WebhookService&) = delete;

	void Reload();
	void Unload();
	void OnGameFrame();
	void Report(const char* detection, int playerIndex, std::string_view evidence, announce::DetectionOutcome outcome);
	void Test();

	[[nodiscard]] bool IsConfigured() const;
	[[nodiscard]] bool IsDisabled() const;
	[[nodiscard]] std::size_t QueueSize() const;

	static bool IsValidUrl(const char* url);
	static bool IsValidRoleId(const char* roleId);
	static bool IsValidLogoUrl(const char* url);

	struct Impl; // webhook.cpp (queue and Steam HTTP transport state)

private:
	std::unique_ptr<Impl> impl;
};

// ---------------------------------------------------------------------------------------------
// The detection pipeline (detection/detection_system.h DetectionSystem)
// ---------------------------------------------------------------------------------------------

namespace detection {

struct DoubletapState {
	int serverTick {-1};
	int incidents {};
	std::string weapon;
	NetworkSafetyEvidence networkEvidence;
};

// Detects two weapon-fire events arriving in the same or next server tick.
class DoubletapModule {
public:
	void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto, NetworkSafetyMonitor* networkSafety);
	void Unload();
	void Reset();
	void OnWeaponFire(const GameEventData& event, PlayerRef* player, int currentTick);
	void OnClientDisconnect(PlayerRef* player);

private:
	AnnounceCallback announce;
	AnnounceCallback announceNetworkVeto;
	NetworkSafetyMonitor* networkSafety {};
	std::array<DoubletapState, MAXPLAYERS + 1> playerData;
};

enum class TriggerContactMode : std::uint8_t {
	AimDriven,
	TargetDriven,
};

struct TriggerbotPending {
	int commandNumber {};
	int targetIndex {-1};
	int reactionTicks {};
	TriggerContactMode mode {};
};

struct TriggerbotIncident {
	Clock::time_point time;
	int reactionTicks {};
};

struct TriggerbotPlayerData {
	std::array<int, MAXPLAYERS + 1> lastContactTick;
	std::deque<TriggerbotPending> pending;
	std::deque<TriggerbotIncident> history;
	QAngle lastAngles;
	int lastCommandNumber {-1};
	int activeTarget {-1};
	int contactStartCommand {-1};
	TriggerContactMode activeMode {};
	bool lastAnglesValid {};
};

struct TriggerbotSmoke {
	Vector center;
	Clock::time_point expires;
};

// Detects repeated damaging shots fired within three ticks of fresh, unobstructed enemy hitbox contact.
class TriggerbotModule {
public:
	void Load(AnnounceCallback announce, NetworkSafetyMonitor* networkSafety);
	void Unload();
	void Reset();
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void OnPlayerHurt(const GameEventData& event, PlayerRef* attacker, PlayerRef* victim, const ShotRecord& shot);
	void OnGameEvent(const GameEventData& event);
	void OnClientDisconnect(PlayerRef* player);

private:
	AnnounceCallback announce;
	NetworkSafetyMonitor* networkSafety {};
	std::array<TriggerbotPlayerData, MAXPLAYERS + 1> playerData;
	std::deque<TriggerbotSmoke> smokes;
	Clock::time_point smokeStateUnknownUntil;
};

struct SilentIncident {
	Clock::time_point time;
	int points {};
};

// Detects damaging shots whose attack-history angle disagrees with the command's base view angle.
class SilentAimModule {
public:
	void Load(AnnounceCallback announce, ShotCorrelator* shots);
	void Unload();
	void Reset();
	void OnShotUpdated(PlayerRef* player, ShotRecord& shot);
	void OnGameFrame(int currentTick);
	void OnClientDisconnect(PlayerRef* player);

private:
	void Finalize(PlayerRef* player, ShotRecord& shot);

	AnnounceCallback announce;
	ShotCorrelator* shots {};
	std::array<std::deque<SilentIncident>, MAXPLAYERS + 1> evidence;
};

struct AimCommand {
	int commandNumber {};
	int clientTick {};
	int serverTick {-1};
	QAngle angles;
	Vector eyePosition;
	bool simulated {};
};

struct AimbotPlayerData {
	std::deque<AimCommand> commands;
	int pendingShot {-1};
	int victimIndex {-1};
	int lastCountedIncidentCommand {};
	bool pending {};
	bool hasCountedIncident {};
};

// Detects damaging command-angle snaps that rapidly converge on an enemy.
class AimbotModule {
public:
	void Load(AnnounceCallback announce, ShotCorrelator* shots);
	void Unload();
	void Reset();
	void OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands);
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void OnGameFrame(int currentTick);
	void OnPlayerHurt(PlayerRef* attacker, PlayerRef* victim, ShotRecord& shot);
	void OnClientDisconnect(PlayerRef* player);

private:
	bool Evaluate(PlayerRef* attacker, AimbotPlayerData& data, int currentTick);

	AnnounceCallback announce;
	ShotCorrelator* shots {};
	std::array<AimbotPlayerData, MAXPLAYERS + 1> playerData;
	std::array<std::deque<Clock::time_point>, MAXPLAYERS + 1> snapEvidence;
	std::array<std::deque<Clock::time_point>, MAXPLAYERS + 1> smoothEvidence;
};

struct AimlockSample {
	int serverTick {-1};
	QAngle angles;
	Vector eyePosition;
	bool valid {};
};

struct AimlockLagHypothesis {
	QAngle startBearing;
	f32 maximumTargetDisplacement {};
	f32 requiredTargetDisplacement {};
	int lagTicks {};
	int onTargetSamples {};
	bool valid {};
};

struct AimlockTrack {
	std::array<AimlockLagHypothesis, 5> hypotheses;
	int targetIndex {-1};
	int bodyPoint {-1};
	int startServerTick {-1};
	int lastServerTick {-1};
	int hypothesisCount {};
	int samples {};
};

struct AimlockPlayerData {
	AimlockSample pending;
	AimlockTrack track;
	int latchedTarget {-1};
	int latchedBodyPoint {-1};
	int breakStartTick {-1};
	int lastProcessedTick {-1};
	bool latched {};
};

// Detects sustained, unnaturally precise target tracking, including through walls.
class AimlockModule {
public:
	void Load(AnnounceCallback announce, ShotCorrelator* shots);
	void Unload();
	void Reset();
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void OnGameFrame(int currentTick);
	void OnClientDisconnect(PlayerRef* player);

private:
	void Evaluate(PlayerRef* player, AimlockPlayerData& data, const AimlockSample& sample);
	void AddIncident(PlayerRef* player, AimlockPlayerData& data, const AimlockLagHypothesis& hypothesis);

	AnnounceCallback announce;
	ShotCorrelator* shots {};
	std::array<AimlockPlayerData, MAXPLAYERS + 1> playerData;
	std::array<std::deque<Clock::time_point>, MAXPLAYERS + 1> evidence;
};

// Detects blacklisted client event subscriptions commonly created by injected code.
class DllInjectionModule {
public:
	void Load(AnnounceCallback announce);
	void Unload();
	void Reset();
	void OnGameFrame();
	void OnClientDisconnect(PlayerRef* player);

private:
	AnnounceCallback announce;
	std::array<Clock::time_point, MAXPLAYERS> nextScans;
};

struct AntiAimCommand {
	int commandNumber {};
	int clientTick {};
	int serverTick {-1};
	QAngle baseAngles;
	QAngle shotAngles;
	f32 historyYawDifference {};
	f32 subtickPitch {};
	f32 subtickYaw {};
	int mouseX {};
	int mouseY {};
	u32 problems {};
	bool attack {};
	bool hasHistoryAngles {};
	bool hasShotAngles {};
	bool simulated {};
};

struct AntiAimPlayerData {
	std::deque<AntiAimCommand> commands;
	Clock::time_point scoreTime;
	f32 score {};
	f32 mismatchScore {};
	std::array<f32, 3> spinSeconds;
	std::array<f32, 3> spinBreakSeconds;
	f32 jitterSeconds {};
	f32 jitterBreakSeconds {};
	int pendingShot {-1};
	int pendingShotTick {-1};
	int spinDebugBucket {-1};
	int jitterDebugBucket {-1};
	int lastMotionServerTick {-1};
	int lastMismatchEvidenceCommand {-1};
	bool invalidActive {};
	bool inconsistencyActive {};
	bool spinActive {};
	bool jitterActive {};
	bool suppressContinuous {};
	bool teleportGrace {};
};

// Detects impossible angles, command inconsistencies, jitter, attack-return, and sustained spin patterns.
class AntiAimModule {
public:
	void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto, NetworkSafetyMonitor* networkSafety);
	void Unload();
	void Reset();
	void OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands);
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void OnGameFrame(int currentTick);
	void OnWeaponFire(PlayerRef* player, const ShotRecord& shot);
	void OnGameEvent(const GameEventData& event, PlayerRef* player);
	void OnClientDisconnect(PlayerRef* player);

private:
	void AddEvidence(PlayerRef* player, AntiAimPlayerData& data, f32 weight, const char* reasonKey, const char* reason, bool continuous,
					 bool mismatch = false);
	void ApplyDecay(PlayerRef* player, AntiAimPlayerData& data);
	void EvaluateMotion(PlayerRef* player, AntiAimPlayerData& data, AntiAimCommand& command);
	void EvaluatePendingShot(PlayerRef* player, AntiAimPlayerData& data, int currentTick);
	void ResetMotion(AntiAimPlayerData& data);

	AnnounceCallback announce;
	AnnounceCallback announceNetworkVeto;
	NetworkSafetyMonitor* networkSafety {};
	std::array<AntiAimPlayerData, MAXPLAYERS + 1> playerData;
};

struct IrregularPending {
	u64 shotId {};
	Clock::time_point time;
	int fireTick {-1};
	int points {};
	bool airborne {};
	bool noScope {};
	bool success {};
};

struct IrregularBucket {
	i64 second {};
	int attempts {};
	int successes {};
	int score {};
};

struct IrregularPlayerData {
	std::deque<IrregularPending> pending;
	std::deque<IrregularBucket> evidence;
};

// Detects repeated success with unusually difficult airborne or unscoped shots.
class IrregularBehaviorModule {
public:
	void Load(AnnounceCallback announce);
	void Unload();
	void Reset();
	void OnWeaponFire(PlayerRef* player, const ShotRecord& shot);
	void OnPlayerDeath(const GameEventData& event, PlayerRef* attacker, PlayerRef* victim, ShotRecord& shot);
	void OnGameFrame(int currentTick);
	void OnClientDisconnect(PlayerRef* player);

private:
	void Finalize(PlayerRef* player, IrregularPlayerData& data, const IrregularPending& attempt);
	void Prune(IrregularPlayerData& data, i64 nowSecond);
	void Evaluate(PlayerRef* player, IrregularPlayerData& data);

	AnnounceCallback announce;
	std::array<IrregularPlayerData, MAXPLAYERS + 1> playerData;
};

struct AccuracyBucket {
	i64 second {};
	int attempts {};
	int hits {};
};

struct AccuracyPlayerData {
	std::deque<AccuracyBucket> evidence;
};

// Detects sustained near-perfect accuracy across a meaningful number of aimed shots.
class InhumanAccuracyModule {
public:
	void Load(AnnounceCallback announce, ShotCorrelator* shotCorrelator);
	void Unload();
	void Reset();
	void OnGameFrame(int currentTick);
	void OnClientDisconnect(PlayerRef* player);

private:
	void Finalize(PlayerRef* player, ShotRecord& shot);
	void Prune(AccuracyPlayerData& data, i64 nowSecond);

	AnnounceCallback announce;
	ShotCorrelator* shots {};
	std::array<AccuracyPlayerData, MAXPLAYERS + 1> playerData;
};

struct NameChangerPlayerData {
	std::string lastName;
	std::deque<Clock::time_point> changes;
	bool initialized {};
};

// Detects clients that change their visible name five times within one rolling minute.
class NameChangerModule {
public:
	void Load(AnnounceCallback announce);
	void Unload();
	void Reset();
	void OnClientReady(PlayerRef* player);
	void OnClientSettingsChanged(PlayerRef* player);
	void OnClientDisconnect(PlayerRef* player);

private:
	AnnounceCallback announce;
	std::array<NameChangerPlayerData, MAXPLAYERS + 1> playerData;
};

// The jump statistics the Autostrafe detector consumes (movement_analysis/jump_analysis).
struct JumpStats {
	f32 airtime {};
	int strafeCount {};
	f32 sync {};
	f32 gainEfficiency {};
	f32 distance {};
	bool valid {};
};

// movement_analysis/detection/movement_detection.h — the movement detectors (bhop, hyperscroll,
// autostrafe, nulls, subtick spam, desubticking, invalid input, invalid cvar).
//
// One instance per player, like the original's CS2ACPlayer::movementDetection (PlayerService).
class MovementDetectionService {
public:
	struct Infraction {
		enum class Type : u8 {
			Other = 0,
			StrafeHack,
			BhopHack,
			Hyperscroll,
			InvalidCvar,
			InvalidInput,
			Nulls,
			SubtickSpam,
			Desubtick,
			COUNT,
		};
		static const char* DisplayName(Type type);
	};

	void Reset();
	void ResetTransientDetectionState();
	bool ShouldRunDetections() const;
	bool ShouldCheckClientCvars() const;
	void InitCvarMonitor(PlayerRef* player);

	struct InputEvent {
		i32 cmdNum;
		f32 fraction;
		f32 framerate;
		u64 button;
		bool pressed;
		bool analog = false;
		f32 airSpeed = -1.0f;
	};

	std::deque<InputEvent> recentForwardBackwardEvents;
	std::deque<InputEvent> recentLeftRightEvents;
	i32 lastNullsCmdNum {-1};
	bool forwardBackwardNullsDirty {};
	bool leftRightNullsDirty {};
	std::vector<f32> nullsFramerateBuffer;
	std::vector<f32> nullsUnderlapBuffer;
	void CreateInputEvents(PlayerRef* player, const UsercmdView& command);
	void CheckNulls(PlayerRef* player);
	void AnalyzeNullsForAxis(PlayerRef* player, const std::deque<InputEvent>& events, u64 button1, u64 button2);

	std::deque<f32> recentJumps;

	struct LandingEvent {
		i32 cmdNum;
		f32 landingTime;
		bool pendingPerf = true;
		bool hasPerfectBhop {};
		bool shouldCountTowardsPerfChains {};
		u32 numJumpBefore {};
		u32 numJumpAfter {};
	};

	f32 lastValidMoveTypeTime = -1.0f;
	std::deque<LandingEvent> recentLandingEvents;
	bool bhopDirty {};
	void ParseCommandForJump(PlayerRef* player, const UsercmdView& command);
	void CreateLandEvent(PlayerRef* player);
	void CheckLandingEvents(PlayerRef* player);
	f32 currentAirTime {};
	bool airMovedThisFrame {};
	// Called from the hook layer at the equivalent of the original's movement hook points.
	void OnAirMoveFrame(f32 frametime);
	void OnProcessMovementFrame();
	void OnProcessMovementPostFrame(PlayerRef* player);
	void OnServerJump(PlayerRef* player, bool framePerfect);
	// CategorizePosition equivalent: a ground-flag transition starts a landing event.
	void OnGroundFrame(PlayerRef* player, bool onGround, bool wasOnGround, f64 curtime);
	void OnChangeMoveTypeFrame(PlayerRef* player, int oldMoveType, int newMoveType, f64 curtime);

	std::deque<f32> suspiciousSubtickMoveTimes;
	std::deque<f32> invalidCommandTimes;
	std::deque<f32> zeroWhenCommandTimes;
	std::deque<f32> numCommandsWithSubtickInputs;
	void CheckSubtickAbuse(PlayerRef* player, const UsercmdView& command);
	void CheckSuspiciousSubtickCommands(PlayerRef* player, f64 timeInServer);

	struct AngleFrame {
		i32 cmdNum;
		QAngle angle;
		f32 frametime;
	};

	f32 yawAccelPercent {};
	std::deque<AngleFrame> angleFrameHistory;
	f32 CalculateYawSpeed(size_t index);
	f32 CalculateYawAccel(size_t index);
	void DetectOptimization(PlayerRef* player, const UsercmdView& command, f32 frametime);

	void OnJumpFinish(PlayerRef* player, const JumpStats& jump);

	enum class JumpStatus : u8 {
		Normal = 0,
		HighStrafeCount,
		VeryHighStrafeCount,
		HighEfficiency,
	};
	std::deque<JumpStatus> recentJumpStatuses;

	f32 currentMaxFps {};
	bool cvarMonitorStarted {};
	std::size_t cvarQueryIndex {};
	f64 cvarCycleInterval {};
	std::set<std::string> invalidCvarLatches;
	std::set<std::string> invalidQueriedCvars;
	std::set<std::string> invalidUserInfoCvars;
	void MarkInvalidCvar(PlayerRef* player, const char* cvarName, const localization::Text& reason, bool kickOnly = false);
	void MarkValidCvar(const char* cvarName);
	void MarkCvarSource(PlayerRef* player, const char* cvarName, const localization::Text& reason, bool invalid, bool userInfo, bool kickOnly = false);
	void MarkInfraction(PlayerRef* player, Infraction::Type type, const localization::Text& reason, bool kickOnly = false);
	// cvars.cpp CheckClientCvars: one round of the integrity cycle (userinfo reads every cycle,
	// then one queried cvar). Called once per second per player by the plugin.
	void DispatchCvarQuery(PlayerRef* player);

	// The engine callbacks the original called back into (cs2ac.cpp HandleDetection).
	std::function<void(const char* detection, PlayerRef* player, const localization::Text& evidence, bool kickOnly)> reportInfraction;

private:
	void RefreshSettings();
	i32 currentCmdNum {};
	u64 settingsMask {};
	u64 revision {};
	static constexpr std::size_t maxNullInputEvents = 2048;
};

class DetectionSystem {
public:
	void Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto);
	void Unload();
	void Reset();
	void OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands);
	void OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick);
	void OnGameFrame(int currentTick);
	void OnGameEvent(const GameEventData& event, PlayerRef* player, int currentTick);
	void OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField, const std::string& missing);
	void OnClientReady(PlayerRef* player);
	void OnClientSettingsChanged(PlayerRef* player);
	void OnClientDisconnect(PlayerRef* player);
	// Movement-detector entry points (the original's movement_analysis service).
	MovementDetectionService* MovementFor(int playerIndex);
	// Once per server frame per player: the port's replacement for the original's movement-hook
	// bookkeeping (ground transitions, air time, frame-perfect jumps, the landings and subtick
	// checks that PhysicsSimulatePost ran).
	void MovementOnGameFrame(PlayerRef* player, f64 timeInServer);
	// The original's OnPlayerFullyConnect -> InitCvarMonitor.
	void MovementOnClientReady(PlayerRef* player);
	// One cycle of the Invalid CVar integrity check (cvars.cpp CheckClientCvars).
	void MovementCvars(PlayerRef* player);

	// Per-player frame-sampler state (ground flag, move type, frame-perfect latch).
	struct SamplerState {
		bool onGround {};
		int moveType {};
		bool framePerfectActive {};
		bool valid {};
	};
	std::array<SamplerState, MAXPLAYERS + 1> sampler {};

private:
	void RefreshSettings();

	NetworkSafetyMonitor networkSafety;
	ShotCorrelator shots;
	DoubletapModule doubletap;
	TriggerbotModule triggerbot;
	SilentAimModule silentAim;
	AimbotModule aimbot;
	AimlockModule aimlock;
	DllInjectionModule dllInjection;
	AntiAimModule antiAim;
	IrregularBehaviorModule irregularBehavior;
	InhumanAccuracyModule inhumanAccuracy;
	NameChangerModule nameChanger;
	std::array<std::unique_ptr<MovementDetectionService>, MAXPLAYERS + 1> movement;
	u64 settingsMask {};
	u64 settingsRevision {};
	bool teammatesAreEnemies {};
};

} // namespace detection

// ---------------------------------------------------------------------------------------------
// The plugin object (cs2ac.cpp CS2ACPlugin)
// ---------------------------------------------------------------------------------------------

class Plugin {
public:
	enum class PunishmentLevel : std::uint8_t {
		None,
		Kick,
		Ban,
	};

	void OnStart();  // OnPluginStart body
	void OnUpdate(); // OnPluginUpdate body (config + webhook + watermark pump)
	void OnEnd();

	bool IsLoaded() const { return loaded; }
	bool SimulatingPhysics() const { return simulatingPhysics; }
	void SetSimulatingPhysics(bool value) { simulatingPhysics = value; }

	void OnMapInit();
	void OnMapShutdown();
	void OnProcessUsercmds(int playerIndex, const std::vector<UsercmdView>& commands);
	void OnSetupMove(int playerIndex, const UsercmdView& command);
	void OnGameFrame(bool simulating);
	void OnGameEvent(const detection::GameEventData& event, int playerIndex);
	void OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField, const std::string& missing);
	void HandleDetection(const char* detection, PlayerRef* player, const localization::Text& evidence, bool kickOnly = false,
						 bool networkVetoed = false);
	void ProcessJoinWatermarks();

	void OnClientActive(int slot, u64 xuid);
	void OnClientFullyConnect(int slot);
	void OnClientSettingsChanged(int slot);
	void OnClientDisconnect(int slot);

	void PrintStatus();
	void PrintHelp();
	void PrintUsercmdDump();
	void NoteUsercmd(const UsercmdView& view);
	// The registered cs2ac_* commands (identical names and behaviour to the original's CON_COMMANDs).
	static s2sdk::ResultType OnCommand(int32_t playerSlot, s2sdk::ConCommandContext context, const plg::vector<plg::string>& args);
	void ReloadConfig();
	void CheckConfig();
	void TestAnnouncement();
	void TestWebhook();
	void ReportConfigLoadTimeout();
	void ApplyConfigLine(const std::string& name, const std::string& value);
	void FinishConfigLoad();
	void OnConfigLoaded();

	detection::DetectionSystem& Detection() { return detectionSystem; }
	WebhookService& Webhook() { return *webhook; }
	bool HasWebhook() const { return webhook != nullptr; }

private:
	void Activate();
	void ResetRuntime();
	void CleanupRuntime();
	void PrintConfigSummary(bool reloaded);
	void LoadConfigFile();   // reads cs2ac.cfg and applies it
	void RegisterCommands();
	void UnregisterCommands();
	// Resyncs the plugin's settings from the registered (possibly console-changed) ConVars.
	void SyncSettingsFromConVars();

	// Per-player frame bookkeeping (the original's MovementEventService::timeSpentInServer and the
	// Invalid CVar cycle timer).
	std::array<f64, MAXPLAYERS + 1> playerTime {};
	f64 lastFrameTime {};
	std::chrono::steady_clock::time_point lastCvarCycle {};
	// Diagnostic: the last usercmd the hook parsed (cs2ac_usercmd_dump).
	UsercmdView lastUsercmd;
	bool lastUsercmdValid {};

	bool loaded {};
	bool convarsRegistered {};
	bool svCheatsWatcherInstalled {};
	bool configLoaded {};
	bool configReloadPending {};
	bool configLoadFailed {};
	bool simulatingPhysics {};
	std::chrono::steady_clock::time_point lastConfigLoad {};
	std::chrono::steady_clock::time_point configLoadDeadline {};
	std::string activationError;
	detection::DetectionSystem detectionSystem;
	WebhookService* webhook {};
	struct JoinWatermarkState {
		std::chrono::steady_clock::time_point showAt;
		std::chrono::steady_clock::time_point expires;
		std::chrono::steady_clock::time_point nextCenterSend;
		int centerBroadcasts {};
		bool pending {};
		bool shown {};
		bool centerPending {};
		bool centerActive {};
	};
	std::array<JoinWatermarkState, MAXPLAYERS + 1> joinWatermarks {};
	std::array<PunishmentLevel, MAXPLAYERS + 1> punishmentLevels {};
};

extern Plugin g_plugin;

// config.cpp's config path helpers (the plugin resolves them through plg::GetConfigsDir()).
std::string ConfigDirectory();
std::string GamedataDirectory();
void SetConfigDirectory(const std::string& directory);

} // namespace cs2ac
