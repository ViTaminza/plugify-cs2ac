// cs2ac — the movement detectors: Bhop, Hyperscroll, Autostrafe, Nulls, Subtick Spam,
// Desubticking, Invalid Input and Invalid CVar.
//
// Ported from the original's movement_analysis/detection/movement_detection.cpp and its
// detectors (bhop.cpp, cvars.cpp, nulls.cpp, subtick.cpp, strafe_optimizer.cpp, jumps.cpp). Every
// threshold macro is kept verbatim. The data comes from the parsed usercmds, the move data of the
// movement detours and the player schema reads; the jump/landing bookkeeping the original hooked
// out of the movement functions is fed by hooks.cpp at the equivalent points
// (CategorizePosition -> OnGroundFrame, OnJumpLegacy/Modern -> OnServerJump, AirMove -> OnAirMove).

#include "cs2ac.hpp"

namespace cs2ac {
namespace detection {

namespace {
bool BhopDebug() { return settings::DebugFlag("cs2ac_bhop_debug"); }
bool HyperscrollDebug() { return settings::DebugFlag("cs2ac_hyperscroll_debug"); }
bool AutostrafeDebug() { return settings::DebugFlag("cs2ac_autostrafe_debug"); }
bool NullsDebug() { return settings::DebugFlag("cs2ac_nulls_debug"); }
bool SubtickDebug() { return settings::DebugFlag("cs2ac_subtick_debug"); }
bool InvalidCvarDebug() { return settings::DebugFlag("cs2ac_invalid_cvar_debug"); }

DetectionType DetectionForInfraction(MovementDetectionService::Infraction::Type type)
{
	using Type = MovementDetectionService::Infraction::Type;
	switch (type) {
	case Type::StrafeHack: return DetectionType::Autostrafe;
	case Type::BhopHack: return DetectionType::Bhop;
	case Type::Hyperscroll: return DetectionType::Hyperscroll;
	case Type::InvalidCvar: return DetectionType::InvalidCvar;
	case Type::InvalidInput: return DetectionType::InvalidInput;
	case Type::Nulls: return DetectionType::Nulls;
	case Type::SubtickSpam: return DetectionType::SubtickSpam;
	case Type::Desubtick: return DetectionType::Desubticking;
	case Type::Other:
	case Type::COUNT: return DetectionType::Count;
	}
	return DetectionType::Count;
}
} // namespace

const char* MovementDetectionService::Infraction::DisplayName(Type type)
{
	// Infraction::displayNames[] of the original.
	static const char* displayNames[] = {
		"OTHER", "AUTOSTRAFE", "BHOP", "HYPERSCROLL", "INVALID CVAR", "INVALID INPUT", "NULLS", "SUBTICK SPAM", "DESUBTICKING",
	};
	const auto index = static_cast<u8>(type);
	return index < static_cast<u8>(Type::COUNT) ? displayNames[index] : "OTHER";
}

// ---------------------------------------------------------------------------------------------
// Service state (movement_detection.cpp)
// ---------------------------------------------------------------------------------------------

void MovementDetectionService::Reset()
{
	currentMaxFps = 0.0f;
	cvarMonitorStarted = false;
	cvarQueryIndex = 0;
	cvarCycleInterval = 0.0;
	nextCvarQuery = {};
	nullsFramerateBuffer.reserve(maxNullInputEvents);
	nullsUnderlapBuffer.reserve(maxNullInputEvents);
	invalidCvarLatches.clear();
	invalidQueriedCvars.clear();
	invalidUserInfoCvars.clear();
	ResetTransientDetectionState();
	settingsMask = settings::GetDetectionMask();
	revision = settings::GetRevision();
}

void MovementDetectionService::ResetTransientDetectionState()
{
	currentMaxFps = 0.0f;
	currentCmdNum = 0;
	lastNullsCmdNum = -1;
	forwardBackwardNullsDirty = false;
	leftRightNullsDirty = false;
	nullsFramerateBuffer.clear();
	nullsUnderlapBuffer.clear();
	invalidCvarLatches.clear();
	invalidQueriedCvars.clear();
	invalidUserInfoCvars.clear();

	recentForwardBackwardEvents.clear();
	recentLeftRightEvents.clear();
	suspiciousSubtickMoveTimes.clear();
	invalidCommandTimes.clear();
	zeroWhenCommandTimes.clear();
	numCommandsWithSubtickInputs.clear();
	recentJumpStatuses.clear();
	recentJumps.clear();
	recentLandingEvents.clear();
	bhopDirty = false;
	currentAirTime = 0.0f;
	airMovedThisFrame = false;
	lastValidMoveTypeTime = -1.0f;
	angleFrameHistory.clear();
	yawAccelPercent = 0.0f;
}

void MovementDetectionService::RefreshSettings()
{
	const u64 currentMask = settings::GetDetectionMask();
	const u64 currentRevision = settings::GetRevision();
	if (currentMask != settingsMask || currentRevision != revision) {
		ResetTransientDetectionState();
		settingsMask = currentMask;
		revision = currentRevision;
	}
}

// cvars.cpp ShouldRunDetections / ShouldCheckClientCvars.
// This is called many times per frame (per player, per detector), so the sv_cheats read goes
// through the engine's per-frame cache instead of an s2sdk lookup each time.
bool MovementDetectionService::ShouldRunDetections() const
{
	if (!settings::IsPluginEnabled()) {
		return false;
	}
	if (!engine::SvCheatsAvailable()) {
		return true; // the original: an unavailable sv_cheats disables the gate, not the detectors
	}
	return !engine::SvCheatsEnabled() || settings::AllowSvCheatsTesting();
}

bool MovementDetectionService::ShouldCheckClientCvars() const
{
	return ShouldRunDetections() && settings::IsDetectionEnabled(DetectionType::InvalidCvar);
}

void MovementDetectionService::InitCvarMonitor(PlayerRef* player)
{
	if (cvarMonitorStarted || !player || player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	cvarMonitorStarted = true;
	// cvars.cpp: StartTimer(CheckClientCvars, RandomFloat(1.0, 5.0), true, true). The first query of
	// a round waits one whole cycle interval, every following one cvarCycleInterval / 10 - the
	// delay the original's timer callback returned for its next fire. The port dispatches from the
	// frame pump (Plugin::OnGameFrame) and paces itself through nextCvarQuery.
	cvarCycleInterval = CvarCycleInterval(player->index);
	cvarQueryIndex = 0;
	nextCvarQuery = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(cvarCycleInterval));
}

// ---------------------------------------------------------------------------------------------
// Invalid CVar (cvars.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr f32 minimumFpsMax = 64.0f;
constexpr f32 maximumMYaw = 0.3f;
constexpr size_t kQueriedCvarCount = kCvarQueryCount;
// cvars.cpp cvarNames[] — "m_yaw" is listed but disabled there; the queried cycle only queries the
// entries below (the userinfo check covers sensitivity).
const char* const kQueriedCvars[kQueriedCvarCount] = {
	"fps_max", "sv_cheats", "sensitivity", "cl_showpos", "cam_showangles", "cl_drawhud", "cl_pitchdown", "cl_pitchup", "cl_yawspeed", "fov_cs_debug",
};
const char* const kUserInfoCvars[] = {"sensitivity"};

localization::Text InvalidNumber(const char* cvar)
{
	return localization::Format("evidence.invalid_cvar.invalid_number", "The client returned an invalid number for {cvar}.", {{"cvar", cvar}});
}

localization::Text AboveMaximum(const char* cvar, double value, const char* maximum)
{
	return localization::Format("evidence.invalid_cvar.above_maximum", "{cvar} is {value}, but the maximum allowed value is {maximum}.",
								{{"cvar", cvar}, {"value", FormatString("%.6g", value)}, {"maximum", maximum}});
}

localization::Text OutsideRange(const char* cvar, double value, const char* minimum, const char* maximum)
{
	return localization::Format("evidence.invalid_cvar.outside_range", "{cvar} is {value}, but it must be between {minimum} and {maximum}.",
								{{"cvar", cvar}, {"value", FormatString("%.6g", value)}, {"minimum", minimum}, {"maximum", maximum}});
}

localization::Text MustEqual(const char* cvar, double value, const char* expected)
{
	return localization::Format("evidence.invalid_cvar.must_equal", "{cvar} is {value}, but it must be {expected}.",
								{{"cvar", cvar}, {"value", FormatString("%.6g", value)}, {"expected", expected}});
}

// cvars.cpp: sv_cheats may only be enforced 30 seconds after it was turned off (the value has to
// propagate to the clients). The port tracks the same grace window from the server cvar state.
std::chrono::steady_clock::time_point cheatCvarCheckerGraceUntil {};
bool g_svCheatsLastValue {false};
bool g_svCheatsInitialized {false};

bool ShouldEnforceCheatCvars()
{
	const bool svCheats = engine::SvCheatsEnabled();
	if (!g_svCheatsInitialized) {
		g_svCheatsInitialized = true;
		g_svCheatsLastValue = svCheats;
		if (!svCheats) {
			cheatCvarCheckerGraceUntil = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		}
	} else if (g_svCheatsLastValue != svCheats) {
		g_svCheatsLastValue = svCheats;
		if (!svCheats) {
			cheatCvarCheckerGraceUntil = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		}
	}
	if (svCheats) {
		return false;
	}
	return std::chrono::steady_clock::now() > cheatCvarCheckerGraceUntil;
}

struct CvarQueryContext {
	int playerIndex {};
	MovementDetectionService* service {};
};

void OnClientCvarResult(int slot, int status, const char* cvarName, const char* cvarValue, void* user)
{
	auto* context = static_cast<CvarQueryContext*>(user);
	if (!context || !context->service || !cvarName) {
		return;
	}
	auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(slot + 1) : nullptr;
	if (!player || !player->IsInGame()) {
		return;
	}
	auto* service = context->service;
	// cvars.cpp ValidateQueriedCvar: only intact values are evaluated.
	constexpr int kValueIntact = 0;
	if (status != kValueIntact) {
		if (InvalidCvarDebug()) {
			ConsoleMessage("Invalid CVar: player slot %d returned status %d for %s; the reply was ignored.", slot, status, cvarName);
		}
		return;
	}
	if (!service->ShouldCheckClientCvars()) {
		return;
	}

	const std::string name = cvarName;
	bool invalid = false;
	auto markInvalid = [&](const localization::Text& reason, bool kickOnly = false) {
		if (EqualsIgnoreCase(name, "m_yaw") || EqualsIgnoreCase(name, "sensitivity")) {
			service->MarkCvarSource(player, cvarName, reason, true, false, kickOnly);
		} else {
			service->MarkInvalidCvar(player, cvarName, reason, kickOnly);
		}
		invalid = true;
	};

	if (EqualsIgnoreCase(name, "m_yaw")) {
		if (!IsNumeric(cvarValue)) {
			markInvalid(InvalidNumber("m_yaw"));
		} else if (const double yaw = std::strtod(cvarValue, nullptr); yaw > maximumMYaw) {
			markInvalid(AboveMaximum("m_yaw", yaw, "0.3"), true);
		}
	} else if (EqualsIgnoreCase(name, "fps_max")) {
		if (!IsNumeric(cvarValue)) {
			markInvalid(InvalidNumber("fps_max"));
		} else {
			const double fps = std::strtod(cvarValue, nullptr);
			service->currentMaxFps = static_cast<f32>(fps);
			if (fps > 0.0 && fps < minimumFpsMax) {
				markInvalid(localization::Format("evidence.invalid_cvar.fps_max",
												 "fps_max is {value}, but it must be at least 64 or 0 for unlimited.",
												 {{"value", FormatString("%.6g", fps)}}),
							true);
			}
		}
	} else if (EqualsIgnoreCase(name, "sv_cheats")) {
		if (!ShouldEnforceCheatCvars()) {
			return;
		}
		// Allow time for the server's disabled value to reach the client before checking it.
		const bool numeric = IsNumeric(cvarValue);
		const bool disabled = EqualsIgnoreCase(cvarValue, "false") || (numeric && std::strtod(cvarValue, nullptr) == 0.0);
		if (!disabled) {
			markInvalid(localization::Format("evidence.invalid_cvar.sv_cheats",
											 "sv_cheats is enabled or invalid on the client while it is disabled on the server."));
		}
	} else if (EqualsIgnoreCase(name, "sensitivity")) {
		const bool numeric = IsNumeric(cvarValue);
		const f32 sens = numeric ? static_cast<f32>(std::strtod(cvarValue, nullptr)) : 0.0f;
		// The actual upper bound is 8.0f but this is to account for possible future updates.
		if (!numeric || sens < 0.0001f || sens > 20.0f) {
			localization::Text reason = numeric ? OutsideRange("sensitivity", sens, "0.0001", "20") : InvalidNumber("sensitivity");
			markInvalid(reason);
		}
	} else if (EqualsIgnoreCase(name, "cl_showpos") || EqualsIgnoreCase(name, "cam_showangles")) {
		if (!ShouldEnforceCheatCvars()) {
			return;
		}
		if (!EqualsIgnoreCase(cvarValue, "0") && !EqualsIgnoreCase(cvarValue, "false")) {
			markInvalid(localization::Format("evidence.invalid_cvar.enabled_without_cheats",
											 "{cvar} is enabled on the client despite sv_cheats being disabled on the server.",
											 {{"cvar", cvarName}}));
		}
	} else if (EqualsIgnoreCase(name, "cl_drawhud")) {
		if (!ShouldEnforceCheatCvars()) {
			return;
		}
		if (EqualsIgnoreCase(cvarValue, "0") || EqualsIgnoreCase(cvarValue, "false")) {
			markInvalid(localization::Format("evidence.invalid_cvar.disabled_without_cheats",
											 "{cvar} is disabled on the client despite sv_cheats being disabled on the server.",
											 {{"cvar", "cl_drawhud"}}));
		}
	} else if (EqualsIgnoreCase(name, "fov_cs_debug")) {
		if (!ShouldEnforceCheatCvars()) {
			return;
		}
		const double fovValue = IsNumeric(cvarValue) ? std::strtod(cvarValue, nullptr) : std::numeric_limits<double>::quiet_NaN();
		if (!std::isfinite(fovValue) || fovValue != 0.0) {
			markInvalid(std::isfinite(fovValue) ? MustEqual("fov_cs_debug", fovValue, "0") : InvalidNumber("fov_cs_debug"));
		}
	} else if (EqualsIgnoreCase(name, "cl_pitchdown")) {
		const bool numeric = IsNumeric(cvarValue);
		const double value = numeric ? std::strtod(cvarValue, nullptr) : 0.0;
		if (!numeric || value != 89.0) {
			markInvalid(numeric ? MustEqual("cl_pitchdown", value, "89") : InvalidNumber("cl_pitchdown"));
		}
	} else if (EqualsIgnoreCase(name, "cl_pitchup")) {
		const bool numeric = IsNumeric(cvarValue);
		const double value = numeric ? std::strtod(cvarValue, nullptr) : 0.0;
		if (!numeric || value != 89.0) {
			markInvalid(numeric ? MustEqual("cl_pitchup", value, "89") : InvalidNumber("cl_pitchup"));
		}
	} else if (EqualsIgnoreCase(name, "cl_yawspeed")) {
		const bool numeric = IsNumeric(cvarValue);
		const double value = numeric ? std::strtod(cvarValue, nullptr) : 0.0;
		if (!numeric || value != 210.0) {
			markInvalid(numeric ? MustEqual("cl_yawspeed", value, "210") : InvalidNumber("cl_yawspeed"));
		}
	}
	if (!invalid) {
		if (EqualsIgnoreCase(name, "m_yaw") || EqualsIgnoreCase(name, "sensitivity")) {
			service->MarkCvarSource(player, cvarName, {}, false, false);
		} else {
			service->MarkValidCvar(cvarName);
		}
	}
}

// cvars.cpp CheckUserInfoCvars (the userinfo reads do not need the async query).
void CheckUserInfoCvars(PlayerRef* player, MovementDetectionService* service)
{
	for (const char* name : kUserInfoCvars) {
		const char* value = engine::ClientCvarValue(player->index, name);
		if (!value || !*value) {
			continue;
		}
		if (EqualsIgnoreCase(name, "m_yaw")) {
			if (!IsNumeric(value)) {
				service->MarkCvarSource(player, name, InvalidNumber("m_yaw"), true, true);
			} else if (const double yaw = std::strtod(value, nullptr); yaw > maximumMYaw) {
				service->MarkCvarSource(player, name, AboveMaximum("m_yaw", yaw, "0.3"), true, true, true);
			} else {
				service->MarkCvarSource(player, name, {}, false, true);
			}
		} else if (EqualsIgnoreCase(name, "sensitivity")) {
			const bool numeric = IsNumeric(value);
			const f32 sens = numeric ? static_cast<f32>(std::strtod(value, nullptr)) : 0.0f;
			if (!numeric || sens < 0.0001f || sens > 20.0f) {
				service->MarkCvarSource(player, name, numeric ? OutsideRange("sensitivity", sens, "0.0001", "20") : InvalidNumber("sensitivity"), true,
										true);
			} else {
				service->MarkCvarSource(player, name, {}, false, true);
			}
		}
	}
}

CvarQueryContext& QueryContext(int playerIndex)
{
	static std::array<CvarQueryContext, MAXPLAYERS + 1> contexts;
	contexts[static_cast<size_t>(playerIndex)].playerIndex = playerIndex;
	return contexts[static_cast<size_t>(playerIndex)];
}
} // namespace

// cvars.cpp CheckClientCvars, driven from the frame pump: the userinfo reads run at the start of
// every round and then one queried cvar every cvarCycleInterval / kQueriedCvarCount seconds, so a
// full round of the queried cvars takes cvarCycleInterval seconds (the original's timer re-armed
// itself with exactly that delay on every fire).
void MovementDetectionService::DispatchCvarQuery(PlayerRef* player)
{
	if (!player || !player->IsInGame()) {
		cvarQueryIndex = 0;
		cvarCycleInterval = 0.0;
		nextCvarQuery = {};
		return;
	}
	if (!ShouldCheckClientCvars()) {
		cvarQueryIndex = 0;
		cvarCycleInterval = 0.0;
		nextCvarQuery = {};
		return;
	}
	const Clock::time_point now = Clock::now();
	if (now < nextCvarQuery) {
		return;
	}
	if (cvarQueryIndex >= kQueriedCvarCount) {
		cvarQueryIndex = 0;
	}
	if (cvarQueryIndex == 0) {
		CheckUserInfoCvars(player, this);
		// A new round picks its interval, like the original's index-0 fire.
		cvarCycleInterval = CvarCycleInterval(player->index);
	}
	auto& context = QueryContext(player->index);
	context.playerIndex = player->index;
	context.service = this;
	if (!engine::QueryClientCvar(player->index, kQueriedCvars[cvarQueryIndex], &OnClientCvarResult, &context)) {
		if (InvalidCvarDebug()) {
			ConsoleMessage("Invalid CVar: the query for %s could not be started.", kQueriedCvars[cvarQueryIndex]);
		}
	}
	++cvarQueryIndex;
	if (cvarQueryIndex == kQueriedCvarCount) {
		cvarQueryIndex = 0;
	}
	nextCvarQuery = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<f64>(CvarQueryDelay(cvarCycleInterval)));
}

void MovementDetectionService::MarkInvalidCvar(PlayerRef* player, const char* cvarName, const localization::Text& reason, bool kickOnly)
{
	RefreshSettings();
	if (!cvarName) {
		return;
	}
	if (invalidCvarLatches.emplace(cvarName).second) {
		if (InvalidCvarDebug()) {
			ConsoleMessage("Invalid CVar: %s marked %s invalid and armed its latch: %s", player->GetName(), cvarName, reason.english.c_str());
		}
		MarkInfraction(player, Infraction::Type::InvalidCvar, reason, kickOnly);
	} else if (InvalidCvarDebug()) {
		ConsoleMessage("Invalid CVar: %s still has invalid %s; its existing latch prevented a duplicate detection.", player->GetName(), cvarName);
	}
}

void MovementDetectionService::MarkValidCvar(const char* cvarName)
{
	if (cvarName && invalidCvarLatches.erase(cvarName) > 0 && InvalidCvarDebug()) {
		ConsoleMessage("Invalid CVar: a client returned %s to a valid value; its latch is armed for a future change.", cvarName);
	}
}

void MovementDetectionService::MarkCvarSource(PlayerRef* player, const char* cvarName, const localization::Text& reason, bool invalid, bool userInfo,
											  bool kickOnly)
{
	auto& source = userInfo ? invalidUserInfoCvars : invalidQueriedCvars;
	if (invalid) {
		source.emplace(cvarName);
		MarkInvalidCvar(player, cvarName, reason, kickOnly);
		return;
	}
	source.erase(cvarName);
	const auto& other = userInfo ? invalidQueriedCvars : invalidUserInfoCvars;
	if (other.find(cvarName) == other.end()) {
		MarkValidCvar(cvarName);
	}
}

void MovementDetectionService::MarkInfraction(PlayerRef* player, Infraction::Type type, const localization::Text& reason, bool kickOnly)
{
	RefreshSettings();
	const auto detection = DetectionForInfraction(type);
	if (!settings::IsDetectionEnabled(detection)) {
		return;
	}
	if (reportInfraction) {
		reportInfraction(Infraction::DisplayName(type), player, reason, kickOnly);
	}
}

// ---------------------------------------------------------------------------------------------
// Nulls (nulls.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr size_t numMinInputEventsForDetection = 128;
constexpr u32 numConsecutivePerfectCstrafeForDetectionMinimum = 128;
constexpr u32 numConsecutivePerfectCstrafeForDetectionMaximum = 640;
// The higher the FPS, the less likely player can get perfect counter-strafes by chance.
constexpr f32 fpsForMinimumSuspicion = 64.0f;  // We shouldn't count any attempt below this FPS.
constexpr f32 fpsForMaximumSuspicion = 256.0f;
constexpr f32 analogCstrafeWeight = 2.0f;      // Perfect analog strafes are extremely suspicious. Most (if not all) ingame null aliases abuse analog inputs.
constexpr f32 minAirSpeedForDetection = 100.0f; // Only consider airstrafes with at least this airspeed to avoid false positives.
// Only count counterstrafe attempts as underlap if the keypresses are at most this far apart. Consider higher values as brand new inputs.
constexpr f32 underlapCountThreshold = 0.2f;
constexpr f32 underlapPercentageThresholdLocal = 0.1f; // At least 10% of the strafes should have underlap to consider the median underlap duration.
// Higher underlap average means the player are unlikely to be nulling.
// If the player's underlap average is above this value, we won't consider them for nulls detection.
constexpr f32 underlapMedianForgivenessThresholdLocal = 0.02f; // ~10% of a flat ground jump, considering 7.5 strafes on average.
} // namespace

void MovementDetectionService::CreateInputEvents(PlayerRef* player, const UsercmdView& command)
{
	auto resetInput = [&]() {
		recentForwardBackwardEvents.clear();
		recentLeftRightEvents.clear();
		forwardBackwardNullsDirty = false;
		leftRightNullsDirty = false;
	};
	// Ignore bots.
	if (!command.hasBase || player->IsFakeClient() || player->IsCSTV() || !player->IsAlive()) {
		resetInput();
		lastNullsCmdNum = -1;
		return;
	}
	if (command.commandNumber <= lastNullsCmdNum) {
		return;
	}
	if (lastNullsCmdNum >= 0 && command.commandNumber != lastNullsCmdNum + 1) {
		resetInput();
	}
	lastNullsCmdNum = command.commandNumber;
	// Note: We assume that the subtick inputs here is consistent with the button states in cmd->buttonstates.
	// Usually the only way this can be false is if the client is not legitimate.
	if (command.subtickMoves.empty()) {
		// No movement transition happened in this command. Preserve earlier transitions:
		// clearing them here lets ordinary idle commands - or inserted empty commands - erase all Nulls evidence.
		return;
	}

	// The original read INetChannelInfo::GetRemoteFramerate and reset the evidence when it was
	// unavailable. s2sdk does not expose the net channel, so the port records the events with an
	// unknown frame rate (-1) instead: the required-perfect-cstrafe scaling of AnalyzeNullsForAxis
	// then falls back to its own unknown-frame-rate value (the original's ENGINE_FIXED_TICK_INTERVAL
	// fallback -> 640 required), which is the most conservative setting the original had.
	// engine::ClientNetStats is deliberately not queried here: it fills only the ping
	// (engine.cpp ClientNetStats) - remoteFrametime, like the loss/choke fields, is never filled
	// (s2sdk has no net-channel accessor), so the per-usercmd read that used to feed this always
	// answered -1 anyway.
	const f32 remoteFrameTime = -1.0f;

	bool invalidInput = false;
	auto recordEvent = [&](u64 button, bool pressed, bool analog, f32 fraction) {
		if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) {
			invalidInput = true;
			return;
		}
		InputEvent event {command.commandNumber, fraction, remoteFrameTime, button, pressed, analog, -1.0f};
		// Only record airspeed if the player might be airstrafing.
		if (!engine::PlayerOnGround(player->index) && engine::PlayerMoveType(player->index) == MOVETYPE_WALK) {
			// This isn't the actual airspeed at the time of the input, but it's close enough for our purposes.
			event.airSpeed = engine::PlayerVelocity(player->index).Length2D();
			if (!std::isfinite(event.airSpeed)) {
				invalidInput = true;
				return;
			}
		}
		auto& events = button == IN_FORWARD || button == IN_BACK ? recentForwardBackwardEvents : recentLeftRightEvents;
		auto& dirty = button == IN_FORWARD || button == IN_BACK ? forwardBackwardNullsDirty : leftRightNullsDirty;
		for (auto existing = events.rbegin(); existing != events.rend(); ++existing) {
			if (existing->cmdNum != event.cmdNum || existing->fraction != event.fraction) {
				break;
			}
			if (existing->button == event.button) {
				if (existing->pressed != event.pressed) {
					events.erase(std::next(existing).base());
					dirty = true;
				} else {
					dirty = dirty || (!existing->analog && event.analog);
					existing->analog |= event.analog;
				}
				return;
			}
		}
		events.push_back(event);
		dirty = true;
		if (events.size() > maxNullInputEvents) {
			events.pop_front();
		}
	};

	u64 oldButtons = 0;
	const Vector lastMovementImpulses = engine::PlayerLastMovementImpulses(player->index);
	if (lastMovementImpulses.x > 0) {
		oldButtons |= IN_FORWARD;
	} else if (lastMovementImpulses.x < 0) {
		oldButtons |= IN_BACK;
	}

	if (lastMovementImpulses.y > 0) {
		oldButtons |= IN_MOVELEFT;
	} else if (lastMovementImpulses.y < 0) {
		oldButtons |= IN_MOVERIGHT;
	}

	for (const SubtickMove& step : command.subtickMoves) {
		const bool hasButton = step.hasButton;
		// Buttons that are not direction inputs are ignored.
		const u64 button = step.button;
		if (hasButton) {
			if (button == IN_FORWARD || button == IN_BACK || button == IN_MOVELEFT || button == IN_MOVERIGHT) {
				recordEvent(button, step.pressed, false, step.when);
			}
		} else if (step.hasAnalogForwardDelta || step.hasAnalogLeftDelta) {
			// Analog movement input changes
			if ((step.hasAnalogForwardDelta && !std::isfinite(step.analogForwardDelta))
				|| (step.hasAnalogLeftDelta && !std::isfinite(step.analogLeftDelta))) {
				invalidInput = true;
				continue;
			}
			// Helper to record an input event
			auto recordAnalogEvent = [&](u64 analogButton, bool pressed) { recordEvent(analogButton, pressed, true, step.when); };

			// Helper to release a button if it's currently pressed
			auto tryRelease = [&](u64 analogButton) {
				if (oldButtons & analogButton) {
					recordAnalogEvent(analogButton, false);
					oldButtons &= ~analogButton;
				}
			};

			// Helper to press a button if it's not currently pressed
			auto tryPress = [&](u64 analogButton) {
				if (!(oldButtons & analogButton)) {
					recordAnalogEvent(analogButton, true);
					oldButtons |= analogButton;
				}
			};
			if (step.hasAnalogForwardDelta && step.analogForwardDelta != 0.0f) {
				const f32 delta = step.analogForwardDelta;
				if (delta == 1.0f) {
					// Back to neutral OR neutral to forward
					if (oldButtons & IN_BACK) {
						tryRelease(IN_BACK);
					} else {
						tryPress(IN_FORWARD);
					}
				} else if (delta == -1.0f) {
					// Forward to neutral OR neutral to back
					if (oldButtons & IN_FORWARD) {
						tryRelease(IN_FORWARD);
					} else {
						tryPress(IN_BACK);
					}
				} else if (delta == 2.0f) {
					// Back to forward
					tryRelease(IN_BACK);
					tryPress(IN_FORWARD);
				} else if (delta == -2.0f) {
					// Forward to back
					tryRelease(IN_FORWARD);
					tryPress(IN_BACK);
				}
			}
			if (step.hasAnalogLeftDelta && step.analogLeftDelta != 0.0f) {
				const f32 delta = step.analogLeftDelta;
				if (delta == 1.0f) {
					// Right to neutral OR neutral to left
					if (oldButtons & IN_MOVERIGHT) {
						tryRelease(IN_MOVERIGHT);
					} else {
						tryPress(IN_MOVELEFT);
					}
				} else if (delta == -1.0f) {
					// Left to neutral OR neutral to right
					if (oldButtons & IN_MOVELEFT) {
						tryRelease(IN_MOVELEFT);
					} else {
						tryPress(IN_MOVERIGHT);
					}
				} else if (delta == 2.0f) {
					// Right to left
					tryRelease(IN_MOVERIGHT);
					tryPress(IN_MOVELEFT);
				} else if (delta == -2.0f) {
					// Left to right
					tryRelease(IN_MOVELEFT);
					tryPress(IN_MOVERIGHT);
				}
			}
		}
	}
	if (invalidInput) {
		resetInput();
	}
}

void MovementDetectionService::AnalyzeNullsForAxis(PlayerRef* player, const std::deque<InputEvent>& events, u64 button1, u64 button2)
{
	if (!player->IsAlive()) {
		return;
	}
	// Not enough data to check.
	if (events.size() < numMinInputEventsForDetection) {
		if (NullsDebug()) {
			ConsoleMessage("Nulls: not enough input events for nulls detection (%zu/%d)", events.size(), static_cast<int>(numMinInputEventsForDetection));
		}
		return;
	}
	nullsFramerateBuffer.clear();
	auto& framerates = nullsFramerateBuffer;
	for (const InputEvent& event : events) {
		if (event.framerate > 0.0f) {
			framerates.push_back(event.framerate);
		}
	}
	f32 medianFramerate = ENGINE_FIXED_TICK_INTERVAL; // the original's fallback when no frame rate is known
	if (!framerates.empty()) {
		auto medianFrameratePosition = framerates.begin() + static_cast<std::ptrdiff_t>(framerates.size() / 2);
		std::nth_element(framerates.begin(), medianFrameratePosition, framerates.end());
		medianFramerate = *medianFrameratePosition;
	}
	// The median FPS should not exceed fps_max set by players.
	if (currentMaxFps != 0) {
		medianFramerate = (std::max)(medianFramerate, 1.0f / currentMaxFps); // Min(measured fps, fps_max)
	}
	const f32 ratio = std::clamp((1 / medianFramerate - fpsForMinimumSuspicion) / (fpsForMaximumSuspicion - fpsForMinimumSuspicion), 0.0f, 1.0f);
	const u32 requiredPerfectCstrafes = LerpU32(1 - ratio, numConsecutivePerfectCstrafeForDetectionMinimum, numConsecutivePerfectCstrafeForDetectionMaximum);
	if (events.size() < requiredPerfectCstrafes) {
		if (NullsDebug()) {
			ConsoleMessage("Nulls: not enough input events (%zu/%d)", events.size(), static_cast<int>(requiredPerfectCstrafes));
		}
		return;
	}
	// Analyze the input events for perfect counter-strafes.
	u32 numOverlaps = 0;
	f32 numPerfect = 0;
	f32 numConsecutivePerfect = 0;
	f32 maxConsecutivePerfect = 0;
	nullsUnderlapBuffer.clear();
	auto& underlapDurations = nullsUnderlapBuffer;

	// Track the last release event and current press state for each direction
	const InputEvent* lastButton1Release = nullptr;
	const InputEvent* lastButton2Release = nullptr;
	bool button1Pressed = false;
	bool button2Pressed = false;

	for (size_t i = 0; i < events.size(); ++i) {
		const InputEvent& event = events[i];

		// Track release events regardless of FPS or airspeed
		if (!event.pressed) {
			if (event.button == button1) {
				lastButton1Release = &event;
				button1Pressed = false;
			} else if (event.button == button2) {
				lastButton2Release = &event;
				button2Pressed = false;
			}
			continue;
		}

		// Now handling press events
		// First, check if we should analyze this event (apply filters)
		bool shouldAnalyze = true;
		if (event.framerate > 0.0f && 1 / event.framerate < fpsForMinimumSuspicion) {
			shouldAnalyze = false;
		}
		if (event.airSpeed < minAirSpeedForDetection) {
			shouldAnalyze = false;
		}
		const f32 weight = event.analog ? analogCstrafeWeight : 1.0f;
		// Check for overlap: pressing one key while opposite key is still held
		const bool isOverlap = (event.button == button1 && button2Pressed) || (event.button == button2 && button1Pressed);
		if (isOverlap) {
			// Check if the opposite key gets released at the same tick/fraction (perfect null)
			bool nulled = false;
			for (size_t j = i + 1; j < events.size(); ++j) {
				const InputEvent& nextEvent = events[j];
				// If we've moved to a different tick/fraction, stop looking
				if (nextEvent.cmdNum != event.cmdNum || nextEvent.fraction != event.fraction) {
					break;
				}
				// Check if this is the release of the opposite button
				if (!nextEvent.pressed) {
					if ((event.button == button1 && nextEvent.button == button2) || (event.button == button2 && nextEvent.button == button1)) {
						nulled = true;
						break;
					}
				}
			}

			if (nulled) {
				// This is actually a perfect counter-strafe, not an overlap
				if (shouldAnalyze) {
					if (NullsDebug() && event.cmdNum == currentCmdNum) {
						ConsoleMessage("Nulls: perfect @ %f", static_cast<double>(static_cast<f32>(event.cmdNum) + event.fraction));
					}
					numPerfect += weight;
					numConsecutivePerfect += weight;
					if (numConsecutivePerfect > maxConsecutivePerfect) {
						maxConsecutivePerfect = numConsecutivePerfect;
					}
				} else {
					numConsecutivePerfect = 0;
				}
			} else {
				if (event.airSpeed >= minAirSpeedForDetection && NullsDebug() && event.cmdNum == currentCmdNum) {
					ConsoleMessage("Nulls: overlap @ %f", static_cast<double>(static_cast<f32>(event.cmdNum) + event.fraction));
				}
				if (event.airSpeed >= minAirSpeedForDetection) {
					numOverlaps += static_cast<u32>(weight);
				}
				numConsecutivePerfect = 0;
			}
		}

		// Update press state
		if (event.button == button1) {
			button1Pressed = true;
		} else if (event.button == button2) {
			button2Pressed = true;
		}

		// If it was an overlap or we're not analyzing, skip the rest
		if (isOverlap || !shouldAnalyze) {
			if (!shouldAnalyze) {
				numConsecutivePerfect = 0;
			}
			continue;
		}

		// Not an overlap, so check for perfect/underlap counter-strafe
		const InputEvent* oppositeRelease = nullptr;
		if (event.button == button1 && lastButton2Release != nullptr) {
			oppositeRelease = lastButton2Release;
		} else if (event.button == button2 && lastButton1Release != nullptr) {
			oppositeRelease = lastButton1Release;
		}

		if (oppositeRelease == nullptr) {
			numConsecutivePerfect = 0;
			continue; // No counter-strafe detected
		}

		// Calculate timing between opposite key release and current key press
		const f32 timeDiff =
			(static_cast<f32>(event.cmdNum - oppositeRelease->cmdNum) + (event.fraction - oppositeRelease->fraction)) * ENGINE_FIXED_TICK_INTERVAL;

		// Only consider this if it's reasonably close (not a brand new input)
		if (timeDiff > underlapCountThreshold) {
			numConsecutivePerfect = 0;
			continue;
		}

		// Note: timeDiff < 0 (overlap) is already handled earlier in the loop
		if (timeDiff == 0.0f) {
			if (NullsDebug() && event.cmdNum == currentCmdNum) {
				ConsoleMessage("Nulls: perfect @ %f", static_cast<double>(static_cast<f32>(event.cmdNum) + event.fraction));
			}
			// Perfect: exactly 0 time between release and press
			numPerfect += weight;
			numConsecutivePerfect += weight;
			if (numConsecutivePerfect > maxConsecutivePerfect) {
				maxConsecutivePerfect = numConsecutivePerfect;
			}
		} else {
			if (NullsDebug() && event.cmdNum == currentCmdNum) {
				ConsoleMessage("Nulls: underlap %.3f ms @ %f", static_cast<double>(timeDiff * 1000.0f),
							   static_cast<double>(static_cast<f32>(event.cmdNum) + event.fraction));
			}
			// Underlap: gap between release and press
			underlapDurations.push_back(timeDiff);
			numConsecutivePerfect = 0;
		}
	}
	f32 underlapMedian = 0.0f;
	if (!underlapDurations.empty()) {
		auto underlapMedianPosition = underlapDurations.begin() + static_cast<std::ptrdiff_t>(underlapDurations.size() / 2);
		std::nth_element(underlapDurations.begin(), underlapMedianPosition, underlapDurations.end());
		underlapMedian = *underlapMedianPosition;
	}

	const u32 total = numOverlaps + static_cast<u32>(numPerfect) + static_cast<u32>(underlapDurations.size());
	// A normal amount of underlap means the input is not mechanically perfect.
	if (underlapMedian >= underlapMedianForgivenessThresholdLocal
		&& (static_cast<f32>(underlapDurations.size()) / static_cast<f32>(total)) >= underlapPercentageThresholdLocal) {
		if (NullsDebug()) {
			ConsoleMessage("Nulls: underlap median too high: %.2f ms", static_cast<double>(underlapMedian * 1000.0f));
		}
		return;
	}

	// The higher the underlap median, the less likely the player is nulling.
	// We scale up the required perfect cstrafes based on how high the underlap median is.
	const f32 underlapRatio = std::clamp(underlapMedian / underlapMedianForgivenessThresholdLocal, 0.0f, 1.0f);
	// Squared because we want to be more strict on lower underlap medians.
	const u32 adjustedRequiredPerfectCstrafes =
		LerpU32(underlapRatio * underlapRatio, requiredPerfectCstrafes, numConsecutivePerfectCstrafeForDetectionMaximum);

	if (maxConsecutivePerfect >= static_cast<f32>(adjustedRequiredPerfectCstrafes)) {
		const char* axis = button1 == IN_FORWARD ? "forward/backward" : "left/right";
		const std::string localizedAxis = localization::Get(button1 == IN_FORWARD ? "evidence.nulls.axis.forward_backward" : "evidence.nulls.axis.left_right", axis);
		localization::Text details {
			FormatString("While airborne, the player repeatedly switched between opposite %s inputs with machine-perfect timing. The timing score "
						 "reached %u; %u was required at the measured frame rate. The median delay between releasing one key and pressing the other "
						 "was %.2f ms, with %u overlapping switches at %.2f FPS.",
						 axis, static_cast<u32>(maxConsecutivePerfect), adjustedRequiredPerfectCstrafes,
						 static_cast<double>(underlapMedian * 1000.0f), numOverlaps, static_cast<double>(1 / medianFramerate)),
			localization::Format("evidence.nulls",
								 "While airborne, the player repeatedly switched between opposite {axis} inputs with machine-perfect timing. The timing "
								 "score reached {score}; {required} was required at the measured frame rate. The median delay between releasing one key "
								 "and pressing the other was {gap} ms, with {overlaps} overlapping switches at {fps} FPS.",
								 {{"axis", localizedAxis},
								  {"score", FormatString("%u", static_cast<u32>(maxConsecutivePerfect))},
								  {"required", FormatString("%u", adjustedRequiredPerfectCstrafes)},
								  {"gap", FormatString("%.2f", static_cast<double>(underlapMedian * 1000.0f))},
								  {"overlaps", FormatString("%u", numOverlaps)},
								  {"fps", FormatString("%.2f", static_cast<double>(1 / medianFramerate))}})
				.localized,
		};
		MarkInfraction(player, Infraction::Type::Nulls, details);
		if (button1 == IN_FORWARD) {
			recentForwardBackwardEvents.clear();
		} else {
			recentLeftRightEvents.clear();
		}
		return;
	}

	if (NullsDebug()) {
		ConsoleMessage("Nulls: perfect %d (consecutive %d, required %d) | overlap %u; underlap median %.1f ms | fps %.1f | samples %u",
					   static_cast<int>(numPerfect), static_cast<int>(maxConsecutivePerfect), static_cast<int>(adjustedRequiredPerfectCstrafes),
					   numOverlaps, static_cast<double>(underlapMedian * 1000.0f), static_cast<double>(1 / medianFramerate), total);
	}
}

void MovementDetectionService::CheckNulls(PlayerRef* player)
{
	if (!player->IsAlive()) {
		recentForwardBackwardEvents.clear();
		recentLeftRightEvents.clear();
		lastNullsCmdNum = -1;
		forwardBackwardNullsDirty = false;
		leftRightNullsDirty = false;
		return;
	}
	if (forwardBackwardNullsDirty) {
		forwardBackwardNullsDirty = false;
		AnalyzeNullsForAxis(player, recentForwardBackwardEvents, IN_FORWARD, IN_BACK);
	}
	if (leftRightNullsDirty) {
		leftRightNullsDirty = false;
		AnalyzeNullsForAxis(player, recentLeftRightEvents, IN_MOVELEFT, IN_MOVERIGHT);
	}
}

// ---------------------------------------------------------------------------------------------
// Subtick checks (subtick.cpp): Invalid Input, Subtick Spam, Desubticking
// ---------------------------------------------------------------------------------------------

namespace {
constexpr f32 subtickInitialIgnoreTime = 10.0f;
constexpr f32 subtickInvalidCommandWindow = 5.0f;
constexpr size_t subtickInvalidCommandThreshold = 8;
constexpr f32 subtickSuspiciousMovesWindow = 0.5f;
constexpr size_t subtickSuspiciousMovesThreshold = 20;
constexpr f32 subtickSubtickInputsWindow = 20.0f;
constexpr size_t subtickSubtickInputsThreshold = 30;
constexpr f32 subtickZeroWhenRatioThreshold = 0.9f;

// Every command should have all button presses/releases accounted for in subtick moves.
// Only cheats that modify buttons without updating subtick moves would fail this.
bool VerifyCommand(const UsercmdView& command)
{
	u64 expectedButtons = command.buttonState2 | command.buttonState3;
	expectedButtons &= (IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK);
	for (const SubtickMove& move : command.subtickMoves) {
		if (move.hasButton) {
			expectedButtons &= ~move.button;
		} else {
			// analog input?
			if (move.hasAnalogForwardDelta && move.analogForwardDelta != 0.0f) {
				expectedButtons &= ~(IN_FORWARD | IN_BACK);
			}
			if (move.hasAnalogLeftDelta && move.analogLeftDelta != 0.0f) {
				expectedButtons &= ~(IN_MOVELEFT | IN_MOVERIGHT);
			}
		}
	}
	return expectedButtons == 0;
}

// Yaw/pitch alias abuse: too many subtick moves with the same button + timing and non-zero angles.
bool HasExcessiveSubtickMovesWithAngles(const UsercmdView& command)
{
	if (command.subtickMoves.size() < 2) {
		return false;
	}
	static std::vector<std::pair<u64, f32>> buttonTimes;
	buttonTimes.clear();
	buttonTimes.reserve(command.subtickMoves.size());
	for (const SubtickMove& step : command.subtickMoves) {
		if (!step.pressed || !step.hasButton) {
			continue;
		}
		// Ignore attack buttons and turn binds (since they would not work anyway).
		if (step.button == IN_ATTACK || step.button == IN_ATTACK2 || step.button == IN_USE || step.button == IN_RELOAD || step.button == IN_TURNLEFT
			|| step.button == IN_TURNRIGHT) {
			continue;
		}
		if (step.hasPitchDelta || step.hasYawDelta) {
			buttonTimes.emplace_back(step.button, step.when);
		}
	}
	std::sort(buttonTimes.begin(), buttonTimes.end());
	buttonTimes.erase(std::unique(buttonTimes.begin(), buttonTimes.end()), buttonTimes.end());
	int numSuspicious = 0;
	for (const SubtickMove& step : command.subtickMoves) {
		if (step.pressed || !step.hasButton) {
			continue;
		}
		if (std::binary_search(buttonTimes.begin(), buttonTimes.end(), std::pair<u64, f32> {step.button, step.when})) {
			++numSuspicious;
			if (numSuspicious >= 2) {
				return true;
			}
		}
	}
	return false;
}

bool PossibleDesubtickedCommand(const UsercmdView& command)
{
	if (command.subtickMoves.empty()) {
		return false;
	}

	bool hasZeroWhen = true;
	bool hasNonAnalogMove = false;
	for (const SubtickMove& step : command.subtickMoves) {
		if (step.hasAnalogForwardDelta || step.hasAnalogLeftDelta) {
			continue;
		}
		hasNonAnalogMove = true;
		hasZeroWhen &= (step.when == 0.0f);
	}
	return hasNonAnalogMove && hasZeroWhen;
}
} // namespace

void MovementDetectionService::CheckSubtickAbuse(PlayerRef* player, const UsercmdView& command)
{
	if (player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	const f64 curtime = engine::CurTime();
	if (settings::IsDetectionEnabled(DetectionType::InvalidInput) && !VerifyCommand(command)) {
		invalidCommandTimes.push_back(static_cast<f32>(curtime));
		if (SubtickDebug()) {
			ConsoleMessage("Invalid Input: suspicious command %d.", command.commandNumber);
		}
	}
	if (settings::IsDetectionEnabled(DetectionType::SubtickSpam) && HasExcessiveSubtickMovesWithAngles(command)) {
		suspiciousSubtickMoveTimes.push_back(static_cast<f32>(curtime));
	}
	if (!command.subtickMoves.empty()) {
		numCommandsWithSubtickInputs.push_back(static_cast<f32>(curtime));
	}
	if (PossibleDesubtickedCommand(command)) {
		zeroWhenCommandTimes.push_back(static_cast<f32>(curtime));
	}
}

void MovementDetectionService::CheckSuspiciousSubtickCommands(PlayerRef* player, f64 timeInServer)
{
	if (player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	if (timeInServer < subtickInitialIgnoreTime) {
		// Don't check for the first 10 seconds to avoid false positives on connect... just in case.
		invalidCommandTimes.clear();
		suspiciousSubtickMoveTimes.clear();
		numCommandsWithSubtickInputs.clear();
		zeroWhenCommandTimes.clear();
		return;
	}
	const f32 currentTime = static_cast<f32>(engine::CurTime());

	while (!invalidCommandTimes.empty() && currentTime - invalidCommandTimes.front() > subtickInvalidCommandWindow) {
		invalidCommandTimes.pop_front();
	}
	if (invalidCommandTimes.size() >= subtickInvalidCommandThreshold) {
		MarkInfraction(player, Infraction::Type::InvalidInput,
					   localization::Format("evidence.invalid_input",
											"Within {seconds} seconds, {commands} commands changed movement buttons without sending the "
											"timing records that should explain those changes.",
											{{"commands", FormatString("%zu", invalidCommandTimes.size())},
											 {"seconds", FormatString("%.1f", static_cast<double>(subtickInvalidCommandWindow))}}));
		invalidCommandTimes.clear();
	}

	while (!suspiciousSubtickMoveTimes.empty() && currentTime - suspiciousSubtickMoveTimes.front() > subtickSuspiciousMovesWindow) {
		suspiciousSubtickMoveTimes.pop_front();
	}
	if (suspiciousSubtickMoveTimes.size() >= subtickSuspiciousMovesThreshold) {
		MarkInfraction(player, Infraction::Type::SubtickSpam,
					   localization::Format("evidence.subtick_spam",
											"Within {seconds} seconds, {commands} commands packed repeated movement changes and view-angle changes "
											"into the exact same moment.",
											{{"commands", FormatString("%zu", suspiciousSubtickMoveTimes.size())},
											 {"seconds", FormatString("%.1f", static_cast<double>(subtickSuspiciousMovesWindow))}}));
		suspiciousSubtickMoveTimes.clear();
	}

	while (!numCommandsWithSubtickInputs.empty() && currentTime - numCommandsWithSubtickInputs.front() > subtickSubtickInputsWindow) {
		numCommandsWithSubtickInputs.pop_front();
	}
	while (!zeroWhenCommandTimes.empty() && currentTime - zeroWhenCommandTimes.front() > subtickSubtickInputsWindow) {
		zeroWhenCommandTimes.pop_front();
	}
	if (numCommandsWithSubtickInputs.size() >= subtickSubtickInputsThreshold) {
		const f32 ratio = static_cast<f32>(zeroWhenCommandTimes.size()) / static_cast<f32>(numCommandsWithSubtickInputs.size());
		if (ratio >= subtickZeroWhenRatioThreshold) {
			localization::Text details =
				localization::Format("evidence.desubticking",
									 "The player repeatedly sent movement inputs with their normal between-tick timing removed. Of {commands} "
									 "checked movement commands, {zero} had their timing set to zero ({ratio}%).",
									 {{"zero", FormatString("%zu", zeroWhenCommandTimes.size())},
									  {"commands", FormatString("%zu", numCommandsWithSubtickInputs.size())},
									  {"ratio", FormatString("%.1f", static_cast<double>(ratio * 100.0f))}});
			if (SubtickDebug()) {
				ConsoleMessage("Desubticking: player slot %d: %s", player->GetPlayerSlot(), details.english.c_str());
			}
			MarkInfraction(player, Infraction::Type::Desubtick, details);
			zeroWhenCommandTimes.clear();
			numCommandsWithSubtickInputs.clear();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Autostrafe optimizer (strafe_optimizer.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr size_t maxAngleFrameHistory = 5;
} // namespace

f32 MovementDetectionService::CalculateYawSpeed(size_t index)
{
	if (index == 0) {
		return 0.0f;
	}

	const f32 currentYaw = angleFrameHistory[index].angle.y;
	const f32 lastYaw = angleFrameHistory[index - 1].angle.y;
	const f32 frameTime = angleFrameHistory[index].frametime;

	f32 yawDiff = currentYaw - lastYaw;
	if (yawDiff > 180.0f) {
		yawDiff -= 360.0f;
	}
	if (yawDiff < -180.0f) {
		yawDiff += 360.0f;
	}

	return frameTime > 0.0f ? yawDiff / frameTime : 0.0f;
}

f32 MovementDetectionService::CalculateYawAccel(size_t index)
{
	if (index < 2) {
		return 0.0f;
	}

	const f32 currentSpeed = CalculateYawSpeed(index);
	const f32 lastSpeed = CalculateYawSpeed(index - 1);
	const f32 frameTime = angleFrameHistory[index].frametime;

	return (currentSpeed - lastSpeed) / frameTime;
}

void MovementDetectionService::DetectOptimization(PlayerRef* player, const UsercmdView& command, f32 frametime)
{
	if (!command.hasBase || !command.hasViewAngles) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: optimizer evidence reset: command %d has no usable view angles.", command.commandNumber);
		}
		angleFrameHistory.clear();
		yawAccelPercent = 0.0f;
		return;
	}
	if (!std::isfinite(frametime) || frametime <= 0.0f) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: optimizer evidence reset: command %d has invalid frame time %.6f.", command.commandNumber,
						   static_cast<double>(frametime));
		}
		angleFrameHistory.clear();
		yawAccelPercent = 0.0f;
		return;
	}
	if (!angleFrameHistory.empty() && command.commandNumber != angleFrameHistory.back().cmdNum + 1) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: optimizer evidence reset: command %d did not follow command %d.", command.commandNumber,
						   angleFrameHistory.back().cmdNum);
		}
		angleFrameHistory.clear();
		yawAccelPercent = 0.0f;
	}

	const QAngle angles = command.viewAngles;
	if (!std::isfinite(angles.x) || !std::isfinite(angles.y) || !std::isfinite(angles.z)) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: optimizer evidence reset: command %d has non-finite view angles.", command.commandNumber);
		}
		angleFrameHistory.clear();
		yawAccelPercent = 0.0f;
		return;
	}

	angleFrameHistory.push_back({command.commandNumber, angles, frametime});
	while (angleFrameHistory.size() > maxAngleFrameHistory) {
		angleFrameHistory.pop_front();
	}

	const size_t historySize = angleFrameHistory.size();
	bool evaluated = false;
	bool matched = false;
	// check if there are enough angle frames to calculate yaw speed
	if (historySize > 2) {
		const f32 lastYawSpeed = CalculateYawSpeed(historySize - 2);
		const f32 currentYawSpeed = CalculateYawSpeed(historySize - 1);
		const bool switchedStrafeDirection = std::signbit(currentYawSpeed) != std::signbit(lastYawSpeed);

		// check if there are enough angle frames to calculate yaw accel
		if (historySize >= maxAngleFrameHistory && switchedStrafeDirection) {
			evaluated = true;
			const f32 yawAccel2TicksAgo = std::abs(CalculateYawAccel(historySize - 3));
			const f32 lastYawAccel = std::abs(CalculateYawAccel(historySize - 2));
			const f32 currentYawAccel = std::abs(CalculateYawAccel(historySize - 1));
			const f32 lastNextDiff = std::abs(currentYawAccel - yawAccel2TicksAgo);

			if (lastNextDiff < 1.0f) {
				const f32 avgAccel = (currentYawAccel + yawAccel2TicksAgo) * 0.5f;
				if (avgAccel < 2.0f && (lastYawAccel - avgAccel) > 2.0f) {
					matched = true;
				}
			}
		}
	}
	if (evaluated) {
		yawAccelPercent = yawAccelPercent * 0.95f + (matched ? 0.05f : 0.0f);
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: optimizer command %d direction switch %s; rolling evidence is %.2f%%.", command.commandNumber,
						   matched ? "matched" : "did not match", static_cast<double>(yawAccelPercent * 100.0f));
		}
	}

	// finally check for suspicious yaw accel patterns
	if (yawAccelPercent > 0.9f) {
		MarkInfraction(player, Infraction::Type::StrafeHack,
					   localization::Format("evidence.autostrafe.optimizer",
											"The player's airborne turns repeatedly followed the same machine-perfect acceleration pattern. The "
											"rolling match score reached {score}%.",
											{{"score", FormatString("%.1f", static_cast<double>(yawAccelPercent * 100.0f))}}));
		yawAccelPercent = 0.0f;
		angleFrameHistory.clear();
	}
}

// ---------------------------------------------------------------------------------------------
// Bhop / Hyperscroll (bhop.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr f32 jumpPatternWindow = 0.25f;
constexpr f32 minAirTimeForBhop = 4.0f * ENGINE_FIXED_TICK_INTERVAL;
constexpr f32 bhopIgnoreDuration = 4.0f * ENGINE_FIXED_TICK_INTERVAL;
constexpr f32 oldJumpPurgeThreshold = jumpPatternWindow * ENGINE_FIXED_TICK_RATE;
constexpr size_t minSampleCount = 20;
constexpr size_t windowSize = 30;
constexpr u32 numConsecutivePerfsForInfraction = 10;
constexpr u32 perfScoreForPatternCheck = 7;
constexpr f32 perfRatioForHyperscrollInfraction = 0.6f;
constexpr f32 repetitivePatternThreshold = 0.9f;
constexpr u32 lowPatternThreshold = 4;
constexpr f32 highPatternThreshold = 16.0f;

bool ShouldTrackBhop()
{
	return settings::IsDetectionEnabled(DetectionType::Bhop) || settings::IsDetectionEnabled(DetectionType::Hyperscroll);
}
} // namespace

void MovementDetectionService::SetCurrentCmdNum(i32 cmdNum)
{
	currentCmdNum = cmdNum;
}

void MovementDetectionService::ParseCommandForJump(PlayerRef* player, const UsercmdView& command)
{
	if (!ShouldTrackBhop() || player->IsFakeClient() || player->IsCSTV()) {
		return;
	}

	auto recordJump = [this](f32 jumpTime, PlayerRef* current) {
		recentJumps.push_back(jumpTime);
		for (auto& event : recentLandingEvents) {
			const f32 timeSinceLanding = jumpTime - static_cast<f32>(event.cmdNum);
			if (timeSinceLanding >= 0.0f && timeSinceLanding <= oldJumpPurgeThreshold) {
				event.numJumpAfter++;
			}
		}
		bhopDirty = true;
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s recorded a jump press at command time %.3f; %zu recent presses remain in the pattern window.",
						   current->GetName(), static_cast<double>(jumpTime), recentJumps.size());
		}
	};

	// The original used CInButtonState::IsButtonNewlyPressed(IN_JUMP), the parsed view carries the
	// same two button-state words, so the "newly pressed" test is buttonstate2/3 (the pressed-this-
	// tick bits) rather than a transition.
	if (command.subtickMoves.empty()) {
		if ((command.buttonState2 | command.buttonState3) & IN_JUMP) {
			recordJump(static_cast<f32>(currentCmdNum), player);
		}
	} else {
		// Assume that the +jump inputs are all legit. Faking +jump's wouldn't really help with anything.
		for (const SubtickMove& step : command.subtickMoves) {
			if (step.hasButton && step.button == IN_JUMP && step.pressed) {
				if (!std::isfinite(step.when) || step.when < 0.0f || step.when > 1.0f) {
					if (BhopDebug() || HyperscrollDebug()) {
						ConsoleMessage("Bhop/Hyperscroll: %s evidence reset: jump input command %d had invalid subtick time %.6f.", player->GetName(),
									   command.commandNumber, static_cast<double>(step.when));
					}
					recentJumps.clear();
					recentLandingEvents.clear();
					bhopDirty = false;
					currentAirTime = 0.0f;
					airMovedThisFrame = false;
					return;
				}
				recordJump(static_cast<f32>(currentCmdNum) + step.when, player);
			}
		}
	}

	// Purge old jump attempts.
	recentJumps.erase(std::remove_if(recentJumps.begin(), recentJumps.end(),
									 [&](f32 when) { return (static_cast<f32>(currentCmdNum) - when) > oldJumpPurgeThreshold; }),
					  recentJumps.end());
}

void MovementDetectionService::CreateLandEvent(PlayerRef* player)
{
	if (!ShouldTrackBhop() || player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	if (!ShouldRunDetections()) {
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s landing ignored: inherited movement detections are currently unavailable.", player->GetName());
		}
		return;
	}
	// Check for minimum air time
	if (currentAirTime < minAirTimeForBhop) {
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s landing ignored: %.4f seconds in air is below the %.4f-second minimum.", player->GetName(),
						   static_cast<double>(currentAirTime), static_cast<double>(minAirTimeForBhop));
		}
		return;
	}
	// Check for valid movetype
	const f32 timeSinceInvalidMoveType = static_cast<f32>(engine::CurTime()) - lastValidMoveTypeTime;
	if (timeSinceInvalidMoveType < bhopIgnoreDuration) {
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s landing ignored: a non-walking movement type was used %.4f seconds ago.", player->GetName(),
						   static_cast<double>(timeSinceInvalidMoveType));
		}
		return;
	}
	// Check for teleport
	if (engine::PlayerJustTeleported(player->index, bhopIgnoreDuration)) {
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s landing ignored: the player teleported within the four-tick grace period.", player->GetName());
		}
		return;
	}
	// Check sv_autobunnyhopping
	if (engine::ServerCvarBool("sv_autobunnyhopping", false)) {
		if (BhopDebug() || HyperscrollDebug()) {
			ConsoleMessage("Bhop/Hyperscroll: %s landing ignored: sv_autobunnyhopping is enabled.", player->GetName());
		}
		return;
	}
	auto& event = recentLandingEvents.emplace_back();
	event.cmdNum = currentCmdNum;
	event.landingTime = static_cast<f32>(engine::CurTime());
	event.numJumpBefore = static_cast<u32>(recentJumps.size());
	event.shouldCountTowardsPerfChains = engine::ServerCvarFloat("sv_jump_spam_penalty_time", 0.0f) >= ENGINE_FIXED_TICK_INTERVAL;
	bhopDirty = true;
	if (BhopDebug() || HyperscrollDebug()) {
		ConsoleMessage("Bhop/Hyperscroll: %s accepted landing %zu of %d at command %d: %.4f seconds in air, %u earlier jump presses, chain "
					   "eligible: %s.",
					   player->GetName(), recentLandingEvents.size(), static_cast<int>(minSampleCount), event.cmdNum,
					   static_cast<double>(currentAirTime), event.numJumpBefore, event.shouldCountTowardsPerfChains ? "yes" : "no");
	}
}

void MovementDetectionService::OnAirMoveFrame(f32 frametime)
{
	if (!ShouldTrackBhop()) {
		return;
	}
	airMovedThisFrame = true;
	currentAirTime += frametime;
}

void MovementDetectionService::OnProcessMovementFrame()
{
	if (!ShouldTrackBhop()) {
		return;
	}
	airMovedThisFrame = false;
}

void MovementDetectionService::OnProcessMovementPostFrame(PlayerRef* player)
{
	if (!ShouldTrackBhop()) {
		return;
	}
	if (!airMovedThisFrame) {
		currentAirTime = 0.0f;
		if (!recentLandingEvents.empty() && recentLandingEvents.back().pendingPerf) {
			recentLandingEvents.back().pendingPerf = false;
			bhopDirty = true;
		}
	}
	(void)player;
}

// The original's MovementEventService::OnJumpModernPost/OnJumpLegacyPost -> OnJump().
void MovementDetectionService::OnServerJump(PlayerRef* player, bool framePerfect)
{
	if (!ShouldTrackBhop() || !ShouldRunDetections()) {
		return;
	}
	const bool hasPendingLanding = !recentLandingEvents.empty() && recentLandingEvents.back().pendingPerf;
	if (BhopDebug() || HyperscrollDebug()) {
		ConsoleMessage("Bhop/Hyperscroll: %s server jump: frame-perfect %s, pending accepted landing %s.", player->GetName(),
					   framePerfect ? "yes" : "no", hasPendingLanding ? "yes" : "no");
	}
	if (framePerfect && hasPendingLanding) {
		recentLandingEvents.back().hasPerfectBhop = true;
		bhopDirty = true;
	}
}

void MovementDetectionService::OnGroundFrame(PlayerRef* player, bool onGround, bool wasOnGround, f64 curtime)
{
	if (!ShouldTrackBhop()) {
		return;
	}
	if (onGround && !wasOnGround) {
		CreateLandEvent(player);
	}
	(void)curtime;
}

void MovementDetectionService::OnChangeMoveTypeFrame(PlayerRef* player, int oldMoveType, int newMoveType, f64 curtime)
{
	if (!ShouldTrackBhop()) {
		return;
	}
	if (oldMoveType != MOVETYPE_WALK || newMoveType != MOVETYPE_WALK) {
		lastValidMoveTypeTime = static_cast<f32>(curtime);
	}
	(void)player;
}

void MovementDetectionService::CheckLandingEvents(PlayerRef* player)
{
	if (!ShouldTrackBhop() || !bhopDirty || player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	bhopDirty = false;
	while (recentLandingEvents.size() > windowSize) {
		recentLandingEvents.pop_front();
	}
	if (recentLandingEvents.size() < minSampleCount) {
		return;
	}

	u32 numPerfs = 0;
	u32 totalChainEligibleEvents = 0;
	u32 maxPerfChain = 0;
	u32 currentPerfChain = 0;
	u32 maxPatternPerfScore = 0;
	u32 currentPatternPerfScore = 0;
	std::unordered_map<u32, u32> patterns;
	u32 mostCommonPattern = 0;
	u32 mostCommonPatternCount = 0;
	u32 totalPatternOccurrences = 0;
	u32 weightedPatternSum = 0;
	const f32 currentTime = static_cast<f32>(engine::CurTime());
	for (const auto& event : recentLandingEvents) {
		const f32 patternAge = currentTime - event.landingTime;
		if (!std::isfinite(patternAge) || patternAge < jumpPatternWindow) {
			bhopDirty |= std::isfinite(patternAge) && patternAge >= 0.0f;
		} else if (event.numJumpAfter > 0 || event.numJumpBefore > 0) {
			const u32 pattern = event.numJumpBefore + event.numJumpAfter;
			const u32 count = ++patterns[pattern];
			totalPatternOccurrences++;
			weightedPatternSum += pattern;
			if (count > mostCommonPatternCount) {
				mostCommonPatternCount = count;
				mostCommonPattern = pattern;
			}
		}
		if (!event.shouldCountTowardsPerfChains) {
			continue;
		}
		totalChainEligibleEvents++;
		if (event.hasPerfectBhop) {
			numPerfs++;
			currentPerfChain++;
			maxPerfChain = (std::max)(maxPerfChain, currentPerfChain);
			currentPatternPerfScore++;
			maxPatternPerfScore = (std::max)(maxPatternPerfScore, currentPatternPerfScore);
		} else {
			currentPerfChain = 0;
			currentPatternPerfScore = currentPatternPerfScore > 0 ? currentPatternPerfScore - 1 : 0;
		}
	}
	const f32 averagePattern = totalPatternOccurrences > 0 ? static_cast<f32>(weightedPatternSum) / static_cast<f32>(totalPatternOccurrences) : 0.0f;
	if (BhopDebug()) {
		ConsoleMessage("Bhop: %s evaluation: %zu landings, %u eligible, %u perfect, longest perfect chain %u, best pattern score %u, %u completed "
					   "patterns, dominant pattern %u repeated %u times.",
					   player->GetName(), recentLandingEvents.size(), totalChainEligibleEvents, numPerfs, maxPerfChain, maxPatternPerfScore,
					   totalPatternOccurrences, mostCommonPattern, mostCommonPatternCount);
	}

	// Hard consecutive perf chain check.
	if (maxPerfChain >= numConsecutivePerfsForInfraction && settings::IsDetectionEnabled(DetectionType::Bhop)) {
		MarkInfraction(player, Infraction::Type::BhopHack,
					   localization::Format("evidence.bhop.perfect_chain",
											"The player made {perfect} frame-perfect bunny hops in one uninterrupted chain, out of {landings} checked "
											"landings.",
											{{"perfect", FormatString("%u", maxPerfChain)}, {"landings", FormatString("%u", totalChainEligibleEvents)}}));
		recentLandingEvents.clear();
		bhopDirty = false;
		return;
	}

	// Pattern-based bhop infraction after a decaying perfect-jump score.
	if (maxPatternPerfScore >= perfScoreForPatternCheck && totalPatternOccurrences >= perfScoreForPatternCheck
		&& mostCommonPatternCount >= static_cast<u32>(totalPatternOccurrences * repetitivePatternThreshold)
		&& mostCommonPattern < lowPatternThreshold && settings::IsDetectionEnabled(DetectionType::Bhop)) {
		MarkInfraction(player, Infraction::Type::BhopHack,
					   localization::Format("evidence.bhop.repeated_pattern",
											"The player repeated the same short jump-button pattern on {repeated} of {patterns} completed landings. The "
											"common pattern used {inputs} jump presses around each landing, while the overall average was {average}.",
											{{"repeated", FormatString("%u", mostCommonPatternCount)},
											 {"patterns", FormatString("%u", totalPatternOccurrences)},
											 {"inputs", FormatString("%u", mostCommonPattern)},
											 {"average", FormatString("%.2f", static_cast<double>(averagePattern))}}));
		recentLandingEvents.clear();
		bhopDirty = false;
		return;
	}

	// Hyperscroll check
	const f32 perfectRatio = totalChainEligibleEvents > 0 ? static_cast<f32>(numPerfs) / static_cast<f32>(totalChainEligibleEvents) : 0.0f;
	if (HyperscrollDebug()) {
		ConsoleMessage("Hyperscroll: %s evaluation: %u completed patterns averaged %.2f jump presses; %u of %u eligible landings were frame-perfect "
					   "(%.2f%%).",
					   player->GetName(), totalPatternOccurrences, static_cast<double>(averagePattern), numPerfs, totalChainEligibleEvents,
					   static_cast<double>(perfectRatio * 100.0f));
	}
	if (averagePattern >= highPatternThreshold && perfectRatio > perfRatioForHyperscrollInfraction
		&& totalPatternOccurrences >= minSampleCount && totalChainEligibleEvents >= minSampleCount
		&& settings::IsDetectionEnabled(DetectionType::Hyperscroll)) {
		MarkInfraction(player, Infraction::Type::Hyperscroll,
					   localization::Format("evidence.hyperscroll",
											"Across {patterns} completed landings, the player sent an average of {average} jump presses around each "
											"landing. Of {landings} checked landings, {perfect} were frame-perfect ({ratio}%).",
											{{"average", FormatString("%.2f", static_cast<double>(averagePattern))},
											 {"patterns", FormatString("%u", totalPatternOccurrences)},
											 {"perfect", FormatString("%u", numPerfs)},
											 {"landings", FormatString("%u", totalChainEligibleEvents)},
											 {"ratio", FormatString("%.2f", static_cast<double>(perfectRatio * 100.0f))}}));
		recentLandingEvents.clear();
		bhopDirty = false;
	}
}

// ---------------------------------------------------------------------------------------------
// Autostrafe jump statistics (jumps.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr f32 minJumpDurationForDetection = 0.6f;
constexpr f32 minSyncForDetection = 0.7f;
constexpr size_t numJumpsWindowSize = 20;
constexpr int baseSuspiciousJumpsThreshold = 15;
constexpr f32 baseStrafesPerSecondThreshold = 20.0f;
constexpr f32 minStrafeDuration = 0.02f;
constexpr f32 realStrafePerSecondThreshold = 18.0f;
constexpr f32 maxStrafesPerSecondThreshold = 30.0f;
constexpr int minSuspiciousJumpsThreshold = 5;
constexpr f32 gainEfficiencyThreshold = 0.95f;
constexpr f32 strafePerSecondThresholdForGainEff = 6.0f;
constexpr f32 jumpDistanceThresholdForGainEff = 240.0f;
} // namespace

void MovementDetectionService::OnJumpFinish(PlayerRef* player, const JumpStats& jump)
{
	if (player->IsFakeClient() || player->IsCSTV()) {
		return;
	}
	if (!ShouldRunDetections()) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: %s jump ignored: inherited movement detections are currently unavailable.", player->GetName());
		}
		recentJumpStatuses.clear();
		return;
	}
	if (!settings::IsDetectionEnabled(DetectionType::Autostrafe)) {
		recentJumpStatuses.clear();
		return;
	}
	if (!jump.valid) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: %s jump rejected: jump data is invalid.", player->GetName());
		}
		return;
	}
	if (jump.airtime < minJumpDurationForDetection) {
		if (AutostrafeDebug()) {
			ConsoleMessage("Autostrafe: %s jump rejected: %.3f seconds in air is below the %.3f-second minimum.", player->GetName(),
						   static_cast<double>(jump.airtime), static_cast<double>(minJumpDurationForDetection));
		}
		return;
	}

	// Rejected jumps are skipped rather than treated as normal or used to erase earlier valid evidence.
	if (jump.sync <= minSyncForDetection) {
		recentJumpStatuses.push_back(JumpStatus::Normal);
	} else if (static_cast<f32>(jump.strafeCount) / jump.airtime > maxStrafesPerSecondThreshold) {
		recentJumpStatuses.push_back(JumpStatus::VeryHighStrafeCount);
	} else if (static_cast<f32>(jump.strafeCount) / jump.airtime > baseStrafesPerSecondThreshold) {
		recentJumpStatuses.push_back(JumpStatus::HighStrafeCount);
	} else {
		// The original counted strafes longer than MIN_STRAFE_DURATION; the ported jump statistics
		// only carry the total, so the effective count equals the total (the sub-20 ms strafes the
		// original dropped are the ones that would otherwise have raised this count).
		const f32 effectiveStrafesPerSecond = static_cast<f32>(jump.strafeCount) / jump.airtime;
		if (effectiveStrafesPerSecond > realStrafePerSecondThreshold) {
			recentJumpStatuses.push_back(JumpStatus::HighStrafeCount);
		} else if (effectiveStrafesPerSecond > strafePerSecondThresholdForGainEff
				   && (jump.gainEfficiency > gainEfficiencyThreshold && jump.distance > jumpDistanceThresholdForGainEff)) {
			recentJumpStatuses.push_back(JumpStatus::HighEfficiency);
		} else {
			recentJumpStatuses.push_back(JumpStatus::Normal);
		}
	}
	// Keep only the recent N jumps.
	while (recentJumpStatuses.size() > numJumpsWindowSize) {
		recentJumpStatuses.pop_front();
	}
	// Count suspicious jumps.
	int suspiciousJumpCount = 0;
	int numVeryHighStrafeJumps = 0;
	for (auto status : recentJumpStatuses) {
		if (status != JumpStatus::Normal) {
			suspiciousJumpCount++;
		}
		if (status == JumpStatus::VeryHighStrafeCount) {
			numVeryHighStrafeJumps++;
		}
	}
	if (AutostrafeDebug()) {
		ConsoleMessage("Autostrafe: %s jump evaluated: %.1f%% sync, %.2f strafes/s, %.1f%% gain efficiency, %.2f units; %d of %zu recent jumps are "
					   "suspicious.",
					   player->GetName(), static_cast<double>(jump.sync * 100.0f), static_cast<double>(static_cast<f32>(jump.strafeCount) / jump.airtime),
					   static_cast<double>(jump.gainEfficiency * 100.0f), static_cast<double>(jump.distance), suspiciousJumpCount,
					   recentJumpStatuses.size());
	}
	if (suspiciousJumpCount >= baseSuspiciousJumpsThreshold
		|| (suspiciousJumpCount >= minSuspiciousJumpsThreshold && numVeryHighStrafeJumps > 0)) {
		const size_t evaluatedJumpCount = recentJumpStatuses.size();
		MarkInfraction(player, Infraction::Type::StrafeHack,
					   localization::Format("evidence.autostrafe.jumps",
											"Of the latest {evaluated} valid jumps, {suspicious} showed air-strafing speed or efficiency patterns "
											"associated with automation ({ratio}%).",
											{{"suspicious", FormatString("%d", suspiciousJumpCount)},
											 {"evaluated", FormatString("%zu", evaluatedJumpCount)},
											 {"ratio", FormatString("%.2f", static_cast<double>(static_cast<f32>(suspiciousJumpCount)
																								 / static_cast<f32>(evaluatedJumpCount) * 100.0f))}}));
		recentJumpStatuses.clear();
	}
	(void)minStrafeDuration;
}

} // namespace detection
} // namespace cs2ac
