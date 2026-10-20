// cs2ac — the shot detectors: Doubletap, Triggerbot, Silentaim, Irregular Behavior, Inhuman
// Accuracy.
//
// Ported from the original's detection/{doubletap,triggerbot,silent_aim,
// irregular_behavior,inhuman_accuracy}_module.cpp with identical thresholds, windows and weights.
// The crosshair trace (Triggerbot) goes through s2sdk::TraceLine instead of INavPhysicsInterface.

#include "cs2ac.hpp"
#include "detection_log.h"

namespace cs2ac {
namespace detection {

namespace {
bool DoubletapDebug() { return settings::DebugFlag("cs2ac_doubletap_debug"); }
bool TriggerbotDebug() { return settings::DebugFlag("cs2ac_triggerbot_debug"); }
bool SilentAimDebug() { return settings::DebugFlag("cs2ac_silentaim_debug"); }
bool IrregularDebug() { return settings::DebugFlag("cs2ac_irregular_debug"); }
bool AccuracyDebug() { return settings::DebugFlag("cs2ac_inhuman_accuracy_debug"); }
} // namespace

// ---------------------------------------------------------------------------------------------
// Doubletap (doubletap_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int doubletapDetectionThreshold = 2;

void MergeNetworkEvidence(NetworkSafetyEvidence& combined, const NetworkSafetyEvidence& current)
{
	combined.pingMilliseconds = (std::max)(combined.pingMilliseconds, current.pingMilliseconds);
	combined.jitterMilliseconds = (std::max)(combined.jitterMilliseconds, current.jitterMilliseconds);
	combined.incomingLoss = (std::max)(combined.incomingLoss, current.incomingLoss);
	combined.outgoingLoss = (std::max)(combined.outgoingLoss, current.outgoingLoss);
	combined.incomingChoke = (std::max)(combined.incomingChoke, current.incomingChoke);
	combined.outgoingChoke = (std::max)(combined.outgoingChoke, current.outgoingChoke);
	combined.commandGaps = (std::max)(combined.commandGaps, current.commandGaps);
	combined.unavailableSamples = (std::max)(combined.unavailableSamples, current.unavailableSamples);
	combined.vetoed = combined.vetoed || current.vetoed;
}
} // namespace

void DoubletapModule::Load(AnnounceCallback announceCallback, AnnounceCallback networkVetoCallback, NetworkSafetyMonitor* networkSafetyMonitor)
{
	announce = std::move(announceCallback);
	announceNetworkVeto = std::move(networkVetoCallback);
	networkSafety = networkSafetyMonitor;
}

void DoubletapModule::Unload()
{
	Reset();
	announce = nullptr;
	announceNetworkVeto = nullptr;
	networkSafety = nullptr;
}

void DoubletapModule::Reset()
{
	playerData = {};
}

void DoubletapModule::OnWeaponFire(const GameEventData& event, PlayerRef* player, int currentTick)
{
	if (!IsEligibleHuman(player)) {
		return;
	}

	auto& previous = playerData[static_cast<size_t>(player->index)];
	const std::string_view weapon = NormalizeWeapon(event.weapon);
	if (!IsBallisticWeapon(weapon)) {
		previous.serverTick = -1;
		previous.weapon.clear();
		return;
	}
	if (previous.serverTick < 0) {
		if (DoubletapDebug()) {
			ConsoleMessage("Doubletap: %s stored the first %.*s fire at server tick %d.", player->GetName(), static_cast<int>(weapon.size()),
						   weapon.data(), currentTick);
		}
		previous.serverTick = currentTick;
		previous.weapon.assign(weapon);
		return;
	}

	const i64 delta = static_cast<i64>(currentTick) - previous.serverTick;
	if (delta < 0 || delta > 1 || previous.weapon != weapon) {
		if (DoubletapDebug()) {
			ConsoleMessage("Doubletap: %s fired %.*s %lld server ticks after %s. Rejected.", player->GetName(), static_cast<int>(weapon.size()),
						   weapon.data(), static_cast<long long>(delta), previous.weapon.c_str());
		}
		previous.serverTick = currentTick;
		previous.weapon.assign(weapon);
		return;
	}

	previous.serverTick = currentTick;
	const int incidents = ++previous.incidents;
	NetworkSafetyEvidence network;
	if (networkSafety) {
		network = networkSafety->Evaluate(player);
	} else {
		network.unavailableSamples = 1;
		network.vetoed = true;
	}
	MergeNetworkEvidence(previous.networkEvidence, network);
	if (DoubletapDebug()) {
		ConsoleMessage("Doubletap: %s matched pair %d/%d at %lld server tick%s apart.", player->GetName(), incidents, doubletapDetectionThreshold,
					   static_cast<long long>(delta), delta == 1 ? "" : "s");
	}
	if (network.vetoed && DoubletapDebug()) {
		ConsoleMessage("Doubletap: %s's pair crossed a network safety gate: %.1f ms ping, %.1f ms jitter, %.1f/%.1f%% loss, %.1f/%.1f%% choke, "
					   "%d command gaps, %d unavailable samples.",
					   player->GetName(), static_cast<double>(network.pingMilliseconds), static_cast<double>(network.jitterMilliseconds),
					   static_cast<double>(network.incomingLoss * 100.0f), static_cast<double>(network.outgoingLoss * 100.0f),
					   static_cast<double>(network.incomingChoke * 100.0f), static_cast<double>(network.outgoingChoke * 100.0f), network.commandGaps,
					   network.unavailableSamples);
	}
	if (incidents < doubletapDetectionThreshold) {
		return;
	}
	previous.incidents = 0;

	const NetworkSafetyEvidence networkEvidence = previous.networkEvidence;
	previous.networkEvidence = {};
	if (networkEvidence.vetoed) {
		if (announceNetworkVeto) {
			const auto details = localization::Format("evidence.doubletap.network_unstable",
													  "The same gun fired twice almost at once on {pairs} separate occasions. The latest pair was "
													  "{ticks} server ticks apart.",
													  {{"pairs", FormatString("%d", doubletapDetectionThreshold)},
													   {"ticks", FormatString("%lld", static_cast<long long>(delta))}});
			announceNetworkVeto("DOUBLETAP", player, AddNetworkSafetyDetails(details, networkEvidence));
		}
		return;
	}

	if (announce) {
		announce("DOUBLETAP", player,
				 localization::Format(delta == 1 ? "evidence.doubletap.one_tick" : "evidence.doubletap.zero_ticks",
									  delta == 1 ? "The same gun fired twice within 1 server tick on {pairs} separate occasions. That is faster than the "
												   "weapon's normal firing cycle allows."
												 : "The same gun fired twice within the same server tick on {pairs} separate occasions. That is faster "
												   "than the weapon's normal firing cycle allows.",
									  {{"pairs", FormatString("%d", doubletapDetectionThreshold)}}));
	}
}

void DoubletapModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
	}
}

// ---------------------------------------------------------------------------------------------
// Triggerbot (triggerbot_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr size_t triggerbotHistoryLimit = 30;
constexpr size_t pendingLimit = 8;
constexpr int freshContactTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE);
constexpr f32 heldAngleMaximumDegrees = 0.5f;
constexpr f32 traceLength = 8192.0f;
constexpr f32 smokeHalfExtent = 320.0f;
constexpr auto evidenceWindow = std::chrono::minutes(5);
constexpr auto smokeLifetime = std::chrono::milliseconds(22500);

static_assert(ScoreReactions(std::array<int, 5> {0, 1, 0, 1, 0}, 5).score == kTriggerbotDetectionThreshold, "five fast reactions must reach the threshold");
static_assert(ScoreReactions(std::array<int, 10> {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, 10).score == 0, "two-tick reactions must not accumulate");
static_assert(ScoreReactions(std::array<int, 4> {1, 2, 3, 4}, 4).score == 0, "mixed normal reactions must not accumulate");
static_assert(ScoreReactions(std::array<int, 4> {10, 20, 30, 40}, 4).score == 0, "slow reactions must not accumulate");
static_assert(ScoreReactions(std::array<int, 3> {3, 3, 1}, 3).score == 2, "one fast reaction is worth two points");
static_assert(ScoreReactions(std::array<int, 3> {0, 0, 3}, 3).score == 1, "a normal reaction decays the score by three");

bool SegmentTouchesSmokeBounds(const Vector& start, const Vector& end, const Vector& center)
{
	const f32 starts[] = {start.x, start.y, start.z};
	const f32 ends[] = {end.x, end.y, end.z};
	const f32 centers[] = {center.x, center.y, center.z};
	f32 minimumFraction = 0.0f;
	f32 maximumFraction = 1.0f;
	for (size_t axis = 0; axis < 3; ++axis) {
		const f32 delta = ends[axis] - starts[axis];
		const f32 minimum = centers[axis] - smokeHalfExtent;
		const f32 maximum = centers[axis] + smokeHalfExtent;
		if (std::fabs(delta) < EPSILON) {
			if (starts[axis] < minimum || starts[axis] > maximum) {
				return false;
			}
			continue;
		}
		f32 first = (minimum - starts[axis]) / delta;
		f32 second = (maximum - starts[axis]) / delta;
		if (first > second) {
			std::swap(first, second);
		}
		minimumFraction = (std::max)(minimumFraction, first);
		maximumFraction = (std::min)(maximumFraction, second);
		if (minimumFraction > maximumFraction) {
			return false;
		}
	}
	return true;
}

const char* ContactModeName(TriggerContactMode mode)
{
	return mode == TriggerContactMode::AimDriven ? "aim moved onto target" : "held angle / target entered aim";
}

const char* HitGroupName(int group)
{
	switch (group) {
	case HITGROUP_HEAD: return "head";
	case HITGROUP_CHEST: return "chest";
	case HITGROUP_STOMACH: return "stomach";
	case HITGROUP_LEFTARM:
	case HITGROUP_RIGHTARM: return "arm";
	case HITGROUP_LEFTLEG:
	case HITGROUP_RIGHTLEG: return "leg";
	case HITGROUP_NECK: return "neck";
	default: return "body";
	}
}

struct TraceContact {
	Vector point;
	int targetIndex {-1};
};

// triggerbot_module.cpp TraceCrosshair: the original traced with CTraceFilterPlayerMovementCS and
// asked the hit entity whether it is a live opponent pawn.
TraceContact TraceCrosshair(PlayerRef* player, const Vector& eye, const QAngle& angles)
{
	TraceContact contact;
	if (!player || !IsFinite(eye) || !IsFinite(angles)) {
		return contact;
	}

	const Vector end = eye + AimForward(angles) * traceLength;
	engine::TraceResult trace;
	if (!engine::TraceLine(eye, end, player->index, trace) || !trace.hit) {
		return contact;
	}

	auto* target = g_pPlayerManager ? g_pPlayerManager->ToPlayer(trace.hitEntityIndex) : nullptr;
	if (!target || !target->IsAlive() || !AreOpponents(engine::PlayerTeam(player->index), engine::PlayerTeam(target->index))) {
		return contact;
	}
	contact.targetIndex = target->index;
	contact.point = trace.endPosition;
	return contact;
}
} // namespace

void TriggerbotModule::Load(AnnounceCallback announceCallback, NetworkSafetyMonitor* networkSafetyMonitor, TriggerPatternModule* patternModule)
{
	announce = std::move(announceCallback);
	networkSafety = networkSafetyMonitor;
	pattern = patternModule;
	Reset();
}

void TriggerbotModule::Unload()
{
	Reset();
	announce = nullptr;
	networkSafety = nullptr;
	pattern = nullptr;
}

void TriggerbotModule::Reset()
{
	playerData = {};
	for (auto& data : playerData) {
		data.lastContactTick.fill(-1);
	}
	smokes.clear();
	// A reload cannot inventory smoke that already exists, so remain fail-safe for one full smoke lifetime.
	smokeStateUnknownUntil = Clock::now() + smokeLifetime;
}

void TriggerbotModule::OnGameEvent(const GameEventData& event)
{
	if (event.name != "smokegrenade_detonate") {
		return;
	}
	const Vector center(event.smokeX, event.smokeY, event.smokeZ);
	const auto now = Clock::now();
	if (!IsFinite(center)) {
		smokeStateUnknownUntil = now + smokeLifetime;
		return;
	}
	smokes.push_back({center, now + smokeLifetime});
	if (TriggerbotDebug()) {
		ConsoleMessage("Triggerbot: smoke safety volume added at %.1f %.1f %.1f.", static_cast<double>(center.x), static_cast<double>(center.y),
					   static_cast<double>(center.z));
	}
}

void TriggerbotModule::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	if (!IsEligibleHuman(player) || !command.hasBase || !command.hasViewAngles) {
		return;
	}
	if (!player->IsAlive()) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	if (command.commandNumber <= data.lastCommandNumber) {
		return;
	}
	if (data.lastCommandNumber >= 0 && command.commandNumber != data.lastCommandNumber + 1) {
		data.activeTarget = -1;
		data.contactStartTick = -1;
	}
	data.lastCommandNumber = command.commandNumber;

	const int attackIndex = command.attack1StartHistoryIndex;
	const bool freshAttack = attackIndex >= 0 && attackIndex < static_cast<int>(command.inputHistory.size())
							 && command.inputHistory[static_cast<size_t>(attackIndex)].hasViewAngles;
	QAngle angles = command.viewAngles;
	if (freshAttack) {
		angles = command.inputHistory[static_cast<size_t>(attackIndex)].viewAngles;
	}
	if (!IsFinite(angles)) {
		data.activeTarget = -1;
		data.contactStartTick = -1;
		return;
	}

	Vector eye;
	if (!engine::ReadEyeOrigin(player->index, eye)) {
		data.activeTarget = -1;
		data.contactStartTick = -1;
		return;
	}
	const TraceContact contact = TraceCrosshair(player, eye, angles);
	if (contact.targetIndex != data.activeTarget) {
		data.activeTarget = contact.targetIndex;
		data.contactStartTick = -1;
		if (contact.targetIndex >= 1) {
			const int previousContact = data.lastContactTick[static_cast<size_t>(contact.targetIndex)];
			if (previousContact < 0 || static_cast<i64>(currentTick) - previousContact >= freshContactTicks) {
				data.contactStartTick = currentTick;
				const f32 aimMovement = data.lastAnglesValid ? AngularDistance(data.lastAngles, angles) : 0.0f;
				data.activeMode = std::isfinite(aimMovement) && aimMovement > heldAngleMaximumDegrees ? TriggerContactMode::AimDriven
																									  : TriggerContactMode::TargetDriven;
				if (TriggerbotDebug()) {
					if (previousContact < 0) {
						ConsoleMessage("Triggerbot: %s acquired target #%d for the first time (%s).", player->GetName(), contact.targetIndex,
									   ContactModeName(data.activeMode));
					} else {
						ConsoleMessage("Triggerbot: %s acquired target #%d after %lld ticks without contact (%s).", player->GetName(),
									   contact.targetIndex, static_cast<long long>(static_cast<i64>(currentTick) - previousContact),
									   ContactModeName(data.activeMode));
					}
				}
			}
		}
	}
	if (contact.targetIndex >= 1) {
		data.lastContactTick[static_cast<size_t>(contact.targetIndex)] = currentTick;
	}

	const auto now = Clock::now();
	while (!smokes.empty() && smokes.front().expires <= now) {
		smokes.pop_front();
	}
	bool smokeUncertain = now < smokeStateUnknownUntil;
	if (!smokeUncertain && contact.targetIndex >= 1) {
		smokeUncertain = std::any_of(smokes.begin(), smokes.end(),
									 [&](const TriggerbotSmoke& smoke) { return SegmentTouchesSmokeBounds(eye, contact.point, smoke.center); });
	}

	if (freshAttack && contact.targetIndex >= 1 && data.contactStartTick >= 0) {
		const int reactionTicks = currentTick - data.contactStartTick;
		if (!smokeUncertain) {
			data.pending.push_back({command.commandNumber, contact.targetIndex, reactionTicks, data.activeMode});
			while (data.pending.size() > pendingLimit) {
				data.pending.pop_front();
			}
			if (TriggerbotDebug()) {
				ConsoleMessage("Triggerbot: %s pressed attack %d ticks after target #%d contact.", player->GetName(), reactionTicks,
							   contact.targetIndex);
			}
		} else if (TriggerbotDebug()) {
			ConsoleMessage("Triggerbot: %s's fresh attack was ignored because smoke state could affect the aim line.", player->GetName());
		}
	}

	if (pattern) {
		// Reuses this command's crosshair trace; the pattern module adds no per-tick traces.
		pattern->OnCommand(player, command, currentTick, eye, angles, contact.targetIndex, freshAttack, data.contactStartTick,
						   data.activeMode == TriggerContactMode::TargetDriven, smokeUncertain);
	}

	data.lastAngles = angles;
	data.lastAnglesValid = true;
}

void TriggerbotModule::OnPlayerHurt(const GameEventData& event, PlayerRef* attacker, PlayerRef* victim, const ShotRecord& shot)
{
	if (!IsEligibleHuman(attacker) || !victim) {
		return;
	}
	auto& data = playerData[static_cast<size_t>(attacker->index)];
	const auto pending = std::find_if(data.pending.begin(), data.pending.end(),
									  [&](const TriggerbotPending& candidate) { return candidate.commandNumber == shot.commandNumber; });
	if (pending == data.pending.end()) {
		return;
	}
	const TriggerbotPending candidate = *pending;
	data.pending.erase(pending);
	if (candidate.targetIndex != victim->index || shot.victimIndex != victim->index) {
		if (TriggerbotDebug()) {
			ConsoleMessage("Triggerbot: %s's damage was ignored because target #%d was expected, not #%d.", attacker->GetName(),
						   candidate.targetIndex, victim->index);
		}
		return;
	}
	const int damage = event.dmgHealth;
	if (damage <= 0) {
		if (TriggerbotDebug()) {
			ConsoleMessage("Triggerbot: %s's hit on target #%d was ignored because it dealt no health damage.", attacker->GetName(), victim->index);
		}
		return;
	}

	const std::string_view weapon = NormalizeWeapon(shot.weapon);
	if (!IsBallisticWeapon(weapon) || weapon == "revolver" || weapon == "taser") {
		if (TriggerbotDebug()) {
			ConsoleMessage("Triggerbot: %s's %.*s shot was excluded.", attacker->GetName(), static_cast<int>(weapon.size()), weapon.data());
		}
		return;
	}
	if (candidate.reactionTicks < 0) {
		return;
	}

	NetworkSafetyEvidence network;
	if (networkSafety) {
		network = networkSafety->Evaluate(attacker);
	} else {
		network.unavailableSamples = 1;
		network.vetoed = true;
	}
	if (network.vetoed) {
		if (TriggerbotDebug()) {
			ConsoleMessage("Triggerbot: %s's %d-tick damaging shot was ignored by network safety.", attacker->GetName(), candidate.reactionTicks);
		}
		return;
	}

	const auto now = Clock::now();
	while (!data.history.empty() && now - data.history.front().time >= evidenceWindow) {
		data.history.pop_front();
	}
	data.history.push_back({now, candidate.reactionTicks});
	while (data.history.size() > triggerbotHistoryLimit) {
		data.history.pop_front();
	}

	std::array<int, triggerbotHistoryLimit> ticks {};
	for (size_t index = 0; index < data.history.size(); ++index) {
		ticks[index] = data.history[index].reactionTicks;
	}
	const ReactionScore score = ScoreReactions(ticks, data.history.size());
	if (TriggerbotDebug()) {
		ConsoleMessage("Triggerbot: %s recorded a damaging %d-tick shot: score %d/%d across %zu reactions; <=1t %d, 2t %d, normal %d.",
					   attacker->GetName(), candidate.reactionTicks, score.score, kTriggerbotDetectionThreshold, data.history.size(),
					   score.oneTickCount, score.twoTickCount, score.normalCount);
	}
	if (score.score < kTriggerbotDetectionThreshold || !announce) {
		return;
	}

	announce("TRIGGERBOT", attacker,
			 localization::Format(
				 "evidence.triggerbot",
				 "The player repeatedly fired almost immediately after the crosshair first touched an enemy. Across {shots} damaging "
				 "shots, {one_tick} were fired within one game update, {two_tick} within two updates, and {normal} took three or more "
				 "updates, bringing the score to {score}/{threshold}. The latest shot took {latest_ticks} updates, dealt {damage} "
				 "damage to the enemy's {hitgroup}, and happened while {context}; the target was player slot {target}.",
				 {{"shots", FormatString("%zu", data.history.size())},
				  {"score", FormatString("%d", score.score)},
				  {"threshold", FormatString("%d", kTriggerbotDetectionThreshold)},
				  {"one_tick", FormatString("%d", score.oneTickCount)},
				  {"two_tick", FormatString("%d", score.twoTickCount)},
				  {"normal", FormatString("%d", score.normalCount)},
				  {"latest_ticks", FormatString("%d", candidate.reactionTicks)},
				  {"damage", FormatString("%d", damage)},
				  {"context", ContactModeName(candidate.mode)},
				  {"hitgroup", HitGroupName(event.hitgroup)},
				  {"target", FormatString("%d", candidate.targetIndex)}}));
	data.history.clear();
}

void TriggerbotModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
		playerData[static_cast<size_t>(player->index)].lastContactTick.fill(-1);
	}
}

// ---------------------------------------------------------------------------------------------
// Silentaim (silent_aim_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int silentDetectionScore = 10;
constexpr int normalHitDecay = 2;
constexpr auto silentEvidenceWindow = std::chrono::minutes(5);
constexpr f32 minimumAllowance = 1.0f;
constexpr f32 blatantExcess = 22.5f;
// The subtick exemption is only granted when the client-reported view travel is plausible for the
// base-angle change of the same command (fork import).
constexpr f32 subtickTravelMargin = 30.0f;
constexpr f32 subtickTravelFallbackMargin = 90.0f;
// Telemetry only: none of the values below changes the score (fork v6/v9.1).
constexpr auto silentSummaryInterval = std::chrono::seconds(120);
constexpr int silentHitRowLimit = 60;
} // namespace

void SilentAimModule::Load(AnnounceCallback announceCallback, ShotCorrelator* shotCorrelator)
{
	announce = std::move(announceCallback);
	shots = shotCorrelator;
}

void SilentAimModule::Unload()
{
	Reset();
	shots = nullptr;
	announce = nullptr;
}

void SilentAimModule::Reset()
{
	evidence = {};
	stats = {};
}

void SilentAimModule::OnShotUpdated(PlayerRef* player, ShotRecord& shot)
{
	if (!IsEligibleHuman(player) || shot.playerIndex != player->index || shot.silentMeasured || shot.silentConsumed || !shot.silentFireSeen
		|| !IsFinite(shot.baseAngles) || !IsFinite(shot.angles) || !std::isfinite(shot.silentInaccuracy) || !std::isfinite(shot.silentSpread)
		|| shot.silentInaccuracy < 0.0f || shot.silentSpread < 0.0f) {
		return;
	}
	shot.silentMeasured = true;

	if (NormalizeWeapon(shot.weapon) == "taser") {
		shot.silentRejected = true;
		if (SilentAimDebug()) {
			ConsoleMessage("Silentaim: %s shot rejected: tasers are not evaluated.", player->GetName());
		}
		return;
	}
	if (!shot.silentSubtickAnglesValid) {
		shot.silentRejected = true;
		if (SilentAimDebug()) {
			ConsoleMessage("Silentaim: %s shot rejected: its subtick view movement was invalid.", player->GetName());
		}
		return;
	}

	constexpr f32 radiansToDegrees = static_cast<f32>(180.0 / M_PI);
	shot.silentAllowance = (std::max)(minimumAllowance, static_cast<f32>(std::atan(shot.silentInaccuracy + shot.silentSpread) * radiansToDegrees));
	shot.silentDeviation = AngularDistance(shot.baseAngles, shot.angles);
	if (!std::isfinite(shot.silentAllowance) || !std::isfinite(shot.silentDeviation)) {
		shot.silentRejected = true;
		return;
	}

	const f32 pitchToBase = shot.baseAngles.x - shot.angles.x;
	const f32 yawToBase = std::remainder(shot.baseAngles.y - shot.angles.y, 360.0f);
	QAngle movementAdjusted = shot.angles;
	movementAdjusted.x += std::copysign((std::min)(std::abs(pitchToBase), shot.silentSubtickPitchTravel), pitchToBase);
	movementAdjusted.y += std::copysign((std::min)(std::abs(yawToBase), shot.silentSubtickYawTravel), yawToBase);
	shot.silentUnsupportedDeviation = AngularDistance(shot.baseAngles, movementAdjusted);
	const bool hasSubtickMovement = shot.silentSubtickPitchTravel > 0.0f || shot.silentSubtickYawTravel > 0.0f;
	// Do not trust the client-reported delta blindly: only credit it when it is compatible with the
	// base-angle change reported for the same command. A silent-aim client that invents large subtick
	// deltas to justify an impossible firing angle will exceed these bounds and stop being exempt.
	f32 plausiblePitchTravel = shot.silentAllowance + subtickTravelFallbackMargin;
	f32 plausibleYawTravel = shot.silentAllowance + subtickTravelFallbackMargin;
	if (shot.silentPreviousBaseValid && IsFinite(shot.silentPreviousBaseAngles)) {
		const f32 basePitchChange = std::abs(shot.baseAngles.x - shot.silentPreviousBaseAngles.x);
		const f32 baseYawChange = std::abs(std::remainder(shot.baseAngles.y - shot.silentPreviousBaseAngles.y, 360.0f));
		plausiblePitchTravel = basePitchChange + shot.silentAllowance + subtickTravelMargin;
		plausibleYawTravel = baseYawChange + shot.silentAllowance + subtickTravelMargin;
	}
	shot.silentMovementSupported = hasSubtickMovement && std::isfinite(shot.silentUnsupportedDeviation)
								   && shot.silentUnsupportedDeviation <= shot.silentAllowance
								   && shot.silentSubtickPitchTravel <= plausiblePitchTravel && shot.silentSubtickYawTravel <= plausibleYawTravel;

	if (shot.silentPreviousBaseValid && IsFinite(shot.silentPreviousBaseAngles)) {
		// A legitimate firing angle lies on the reported path between adjacent base angles, so the two path legs
		// should not be meaningfully longer than the direct angle between those commands.
		const f32 previousToAttack = AngularDistance(shot.silentPreviousBaseAngles, shot.angles);
		const f32 previousToCurrent = AngularDistance(shot.silentPreviousBaseAngles, shot.baseAngles);
		shot.silentAdjacentPathExcess = (std::max)(0.0f, previousToAttack + shot.silentDeviation - previousToCurrent);
		shot.silentMovementSupported =
			shot.silentMovementSupported || (std::isfinite(shot.silentAdjacentPathExcess) && shot.silentAdjacentPathExcess <= minimumAllowance);
	}

	if (SilentAimDebug()) {
		ConsoleMessage("Silentaim: %s matched command %d to FireBullets weapon %u: deviation %.2f, allowance %.2f, unexplained %.2f, adjacent path "
					   "excess %.2f, subtick travel %.2f pitch/%.2f yaw, inaccuracy %.5f, spread %.5f.",
					   player->GetName(), shot.commandNumber, shot.silentWeaponId, static_cast<double>(shot.silentDeviation),
					   static_cast<double>(shot.silentAllowance), static_cast<double>(shot.silentUnsupportedDeviation),
					   static_cast<double>(shot.silentPreviousBaseValid ? shot.silentAdjacentPathExcess : -1.0f),
					   static_cast<double>(shot.silentSubtickPitchTravel), static_cast<double>(shot.silentSubtickYawTravel),
					   static_cast<double>(shot.silentInaccuracy), static_cast<double>(shot.silentSpread));
	}
}

void SilentAimModule::OnGameFrame(int currentTick)
{
	if (!shots) {
		return;
	}
	const auto now = Clock::now();
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		for (auto& shot : shots->GetShots(index)) {
			const i64 age = static_cast<i64>(currentTick) - shot.fireTick;
			if (!shot.silentConsumed && age >= 2) {
				Finalize(player, shot);
			}
		}
		auto& data = stats[static_cast<size_t>(index)];
		if (data.dirty && player && now >= data.nextSummary) {
			WriteSummary(player, data);
			data.nextSummary = now + silentSummaryInterval;
		}
	}
}

void SilentAimModule::Finalize(PlayerRef* player, ShotRecord& shot)
{
	shot.silentConsumed = true;
	if (!IsEligibleHuman(player) || !shot.silentMeasured || !shot.silentHitSeen || shot.silentRejected) {
		return;
	}

	// How far the camera itself turned during the shot command (fork v6/v7). Silent aim leaves the
	// camera where it was; an honest flick that overshoots the target moves it fast.
	const f32 cameraTurn = shot.silentPreviousBaseValid && IsFinite(shot.silentPreviousBaseAngles)
							   ? AngularDistance(shot.silentPreviousBaseAngles, shot.baseAngles)
							   : -1.0f;
	const int cameraBucket = static_cast<int>(SilentAimCameraBucket(cameraTurn));
	auto& stat = stats[static_cast<size_t>(player->index)];
	stat.dirty = true;
	++stat.measuredHits;
	++stat.hitCameraBuckets[static_cast<size_t>(cameraBucket)];

	const auto now = Clock::now();
	auto& incidents = evidence[static_cast<size_t>(player->index)];
	while (!incidents.empty() && now - incidents.front().time >= silentEvidenceWindow) {
		incidents.pop_front();
	}

	if (shot.silentDeviation <= shot.silentAllowance || shot.silentMovementSupported) {
		++(shot.silentDeviation <= shot.silentAllowance ? stat.normalHits : stat.movementExcusedHits);
		int remainingDecay = normalHitDecay;
		while (remainingDecay > 0 && !incidents.empty()) {
			const int applied = (std::min)(remainingDecay, incidents.back().points);
			incidents.back().points -= applied;
			remainingDecay -= applied;
			if (incidents.back().points == 0) {
				incidents.pop_back();
			}
		}
		if (SilentAimDebug()) {
			int total = 0;
			for (const auto& incident : incidents) {
				total += incident.points;
			}
			if (shot.silentMovementSupported && shot.silentDeviation > shot.silentAllowance) {
				ConsoleMessage("Silentaim: %s confirmed hit was ignored: reported view movement explained the %.2f-degree mismatch; score decayed by "
							   "%d to %d/%d.",
							   player->GetName(), static_cast<double>(shot.silentDeviation), normalHitDecay - remainingDecay, total, silentDetectionScore);
			} else {
				ConsoleMessage("Silentaim: %s confirmed hit was normal: %.2f <= %.2f degrees; score decayed by %d to %d/%d.", player->GetName(),
							   static_cast<double>(shot.silentDeviation), static_cast<double>(shot.silentAllowance), normalHitDecay - remainingDecay,
							   total, silentDetectionScore);
			}
		}
		return;
	}

	const f32 excess = shot.silentDeviation - shot.silentAllowance;
	const std::string_view weapon = NormalizeWeapon(shot.weapon);
	const bool noscope = !shot.scoped && (weapon == "awp" || weapon == "ssg08" || weapon == "g3sg1" || weapon == "scar20");
	const bool blatant = excess > blatantExcess;
	// An HE that just blew the smoke open leaves the kill visible, so its smoke adds no weight.
	const bool smokeBonus = shot.throughSmoke && !shot.smokeClearedByHe;
	const bool stillCamera = std::isfinite(cameraTurn) && cameraTurn >= 0.0f && cameraTurn < kSilentAimStillCameraTurn;
	const bool flickMismatch = std::isfinite(cameraTurn) && cameraTurn >= kSilentAimStillCameraTurn && excess < kSilentAimFlickHalfExcess;
	const int points = SilentAimPoints(excess, cameraTurn, shot.airborne, shot.headshot, shot.wallbang, smokeBonus, noscope);
	if (points > 0) {
		incidents.push_back({now, points});
	}

	int total = 0;
	for (const auto& incident : incidents) {
		total += incident.points;
	}
	++stat.unexplainedHits;
	++stat.unexplainedCameraBuckets[static_cast<size_t>(cameraBucket)];
	++stat.excessBuckets[SilentAimExcessBucket(excess)];
	stat.maxExcess = (std::max)(stat.maxExcess, excess);
	if (stat.rowsLogged < silentHitRowLimit) {
		++stat.rowsLogged;
		WriteHit(player, shot, excess, cameraTurn, points, total);
	}
	if (SilentAimDebug()) {
		ConsoleMessage("Silentaim: %s added %d point%s for %.2f degrees; score %d/%d.", player->GetName(), points, points == 1 ? "" : "s",
					   static_cast<double>(shot.silentDeviation), total, silentDetectionScore);
	}
	if (total >= silentDetectionScore) {
		if (announce) {
			localization::Text weightDetails;
			auto addWeightSentence = [&](const char* key, const char* english) {
				const auto sentence = localization::Format(key, english);
				const char* separator = weightDetails.english.empty() ? "" : " ";
				weightDetails = {weightDetails.english + separator + sentence.english, weightDetails.localized + separator + sentence.localized};
			};
			if (blatant) {
				addWeightSentence("evidence.silentaim.weight.blatant", "The unexplained angle was over 22.5 degrees, so the shot started at 4 points.");
			} else if (shot.airborne) {
				addWeightSentence("evidence.silentaim.weight.airborne", "The player was airborne, so the shot started at 3 points.");
			} else {
				addWeightSentence("evidence.silentaim.weight.grounded", "The player was on the ground, so the shot started at 2 points.");
			}
			if (shot.headshot) {
				addWeightSentence("evidence.silentaim.weight.headshot", "A headshot added 1 point.");
			}
			if (shot.wallbang) {
				addWeightSentence("evidence.silentaim.weight.wallbang", "A wallbang added 2 points.");
			}
			if (smokeBonus) {
				addWeightSentence("evidence.silentaim.weight.smoke", "A shot through smoke added 2 points.");
			}
			if (noscope) {
				addWeightSentence("evidence.silentaim.weight.noscope", "A no-scope added 1 point.");
			}
			if (stillCamera) {
				addWeightSentence("evidence.silentaim.weight.stillcamera", "The camera did not move during the shot, so 1 point was added.");
			} else if (flickMismatch) {
				addWeightSentence("evidence.silentaim.weight.flick",
								  "The camera was turning and the mismatch was under 2 degrees, so the shot counted half.");
			}
			auto formatEvidence = [&](const std::string& weight) {
				return localization::Format("evidence.silentaim",
											"The bullet landed {deviation} degrees away from the player's visible aim. The weapon's current inaccuracy "
											"and spread allowed {allowance} degrees, leaving {excess} degrees unexplained. {weight} The rolling score "
											"reached {score}/{threshold}.",
											{{"deviation", FormatString("%.2f", static_cast<double>(shot.silentDeviation))},
											 {"allowance", FormatString("%.2f", static_cast<double>(shot.silentAllowance))},
											 {"excess", FormatString("%.2f", static_cast<double>(excess))},
											 {"weight", weight},
											 {"score", FormatString("%d", total)},
											 {"threshold", FormatString("%d", silentDetectionScore)}});
			};
			const auto englishEvidence = formatEvidence(weightDetails.english);
			const auto localizedEvidence = formatEvidence(weightDetails.localized);
			announce("SILENTAIM", player, {englishEvidence.english, localizedEvidence.localized});
		}
		incidents.clear();
	}
}

void SilentAimModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		evidence[static_cast<size_t>(player->index)].clear();
		auto& data = stats[static_cast<size_t>(player->index)];
		if (data.dirty) {
			WriteSummary(player, data);
		}
		data = {};
	}
}

void SilentAimModule::OnPlayerDeath(const GameEventData& event, bool smokeClearedByHe)
{
	auto* attacker = g_pPlayerManager ? g_pPlayerManager->ToPlayer(event.attacker) : nullptr;
	auto* victim = g_pPlayerManager ? g_pPlayerManager->ToPlayer(event.userid) : nullptr;
	if (!IsEligibleHuman(attacker) || !victim || attacker == victim) {
		return;
	}
	// The pawn can already be gone when the death arrives, so the teams come from the controllers.
	if (engine::PlayerTeam(attacker->index) == engine::PlayerTeam(victim->index)) {
		return;
	}
	const bool headshot = event.headshot;
	const bool throughSmoke = event.thrusmoke;
	const bool wallbang = event.penetrated;
	auto& data = stats[static_cast<size_t>(attacker->index)];
	++data.kills;
	if (headshot) {
		++data.killsHs;
	}
	if (throughSmoke) {
		++data.smokeKills;
		if (smokeClearedByHe) {
			++data.smokeHeKills;
		}
		if (headshot) {
			++data.smokeHs;
		}
	}
	if (wallbang) {
		++data.wallKills;
		if (headshot) {
			++data.wallHs;
		}
	}
	if (event.attackerblind) {
		++data.blindKills;
	}
	if (event.noscope) {
		++data.noscopeKills;
	}
	data.dirty = true;
}

void SilentAimModule::WriteHit(PlayerRef* player, const ShotRecord& shot, f32 excess, f32 cameraTurn, int points, int score)
{
	f32 distance = -1.0f;
	const TrackedPosition* victim = shots ? shots->FindPosition(shot.serverTick, shot.victimIndex) : nullptr;
	if (victim && IsFinite(victim->eyePosition) && IsFinite(shot.eyePosition)) {
		distance = (victim->eyePosition - shot.eyePosition).Length();
	}
	const std::string_view weapon = NormalizeWeapon(shot.weapon);
	char fields[768];
	std::snprintf(fields, sizeof(fields),
				  ",\"weapon\":\"%.*s\",\"deviation\":%.3f,\"allowance\":%.3f,\"excess\":%.3f,\"unsupported\":%.3f,\"path_excess\":%.3f,"
				  "\"camera_turn\":%.3f,\"subtick_pitch\":%.3f,\"subtick_yaw\":%.3f,\"mouse_dx\":%d,\"mouse_dy\":%d,\"distance\":%.0f,"
				  "\"airborne\":%s,\"scoped\":%s,\"headshot\":%s,\"wallbang\":%s,\"smoke\":%s,\"smoke_he\":%s,"
				  "\"points\":%d,\"score\":%d",
				  static_cast<int>(weapon.size()), weapon.data(), static_cast<double>(shot.silentDeviation),
				  static_cast<double>(shot.silentAllowance), static_cast<double>(excess),
				  static_cast<double>(std::isfinite(shot.silentUnsupportedDeviation) ? shot.silentUnsupportedDeviation : -1.0f),
				  static_cast<double>(shot.silentPreviousBaseValid ? shot.silentAdjacentPathExcess : -1.0f),
				  static_cast<double>(std::isfinite(cameraTurn) ? cameraTurn : -1.0f), static_cast<double>(shot.silentSubtickPitchTravel),
				  static_cast<double>(shot.silentSubtickYawTravel), shot.silentMouseDx, shot.silentMouseDy,
				  static_cast<double>(std::isfinite(distance) ? distance : -1.0f), shot.airborne ? "true" : "false", shot.scoped ? "true" : "false",
				  shot.headshot ? "true" : "false", shot.wallbang ? "true" : "false", shot.throughSmoke ? "true" : "false",
				  shot.smokeClearedByHe ? "true" : "false", points, score);
	detection_log::WriteTelemetry("silentaim_hit", player, fields);
}

void SilentAimModule::WriteSummary(PlayerRef* player, SilentAimPlayerStats& data)
{
	char fields[1024];
	std::snprintf(fields, sizeof(fields),
				  ",\"hits\":%d,\"normal\":%d,\"movement_excused\":%d,\"unexplained\":%d,"
				  "\"excess_buckets_deg\":\"<0.25,<0.5,<1,<2,<5,<22.5,22.5+\",\"excess_counts\":[%d,%d,%d,%d,%d,%d,%d],\"max_excess\":%.3f,"
				  "\"camera_buckets_deg\":\"<0.5,<2,<5,<15,15+,unknown\",\"hit_camera_counts\":[%d,%d,%d,%d,%d,%d],"
				  "\"unexplained_camera_counts\":[%d,%d,%d,%d,%d,%d],"
				  "\"kills\":%d,\"kills_hs\":%d,\"smoke_kills\":%d,\"smoke_hs\":%d,\"smoke_he_kills\":%d,\"wall_kills\":%d,\"wall_hs\":%d,\"blind_kills\":%d,"
				  "\"noscope_kills\":%d",
				  data.measuredHits, data.normalHits, data.movementExcusedHits, data.unexplainedHits, data.excessBuckets[0], data.excessBuckets[1],
				  data.excessBuckets[2], data.excessBuckets[3], data.excessBuckets[4], data.excessBuckets[5], data.excessBuckets[6],
				  static_cast<double>(std::isfinite(data.maxExcess) ? data.maxExcess : -1.0f), data.hitCameraBuckets[0], data.hitCameraBuckets[1],
				  data.hitCameraBuckets[2], data.hitCameraBuckets[3], data.hitCameraBuckets[4], data.hitCameraBuckets[5],
				  data.unexplainedCameraBuckets[0], data.unexplainedCameraBuckets[1], data.unexplainedCameraBuckets[2], data.unexplainedCameraBuckets[3],
				  data.unexplainedCameraBuckets[4], data.unexplainedCameraBuckets[5], data.kills, data.killsHs, data.smokeKills, data.smokeHs,
				  data.smokeHeKills, data.wallKills, data.wallHs, data.blindKills, data.noscopeKills);
	detection_log::WriteTelemetry("silentaim_stats", player, fields);
	// Counters are per summary window; the row limit and the schedule carry over.
	const int rowsLogged = data.rowsLogged;
	const auto nextSummary = data.nextSummary;
	data = {};
	data.rowsLogged = rowsLogged;
	data.nextSummary = nextSummary;
}

// ---------------------------------------------------------------------------------------------
// Irregular Behavior (irregular_behavior_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int irregularEvidenceWindowSeconds = 5 * 60;
constexpr f32 irregularMinimumDistance = 10.0f;
constexpr f32 longDistance = 20.0f;
constexpr int irregularDetectionScore = 16;
constexpr int minimumSuccesses = 3;
constexpr int minimumAttempts = 4;
constexpr size_t irregularPendingLimit = 8;

bool IsSniperWeapon(std::string_view weapon)
{
	weapon = NormalizeWeapon(weapon);
	return weapon == "awp" || weapon == "ssg08" || weapon == "scar20" || weapon == "g3sg1";
}

i64 ToSecond(Clock::time_point time)
{
	return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}
} // namespace

void IrregularBehaviorModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
}

void IrregularBehaviorModule::Unload()
{
	Reset();
	announce = nullptr;
}

void IrregularBehaviorModule::Reset()
{
	playerData = {};
}

void IrregularBehaviorModule::OnWeaponFire(PlayerRef* player, const ShotRecord& shot)
{
	if (!IsEligibleHuman(player) || shot.playerIndex != player->index || !IsBallisticWeapon(shot.weapon)) {
		return;
	}
	const bool sniper = IsSniperWeapon(shot.weapon);
	const bool noScope = sniper && !shot.scoped;
	if (!shot.airborne && !noScope) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	while (data.pending.size() >= irregularPendingLimit) {
		const IrregularPending attempt = data.pending.front();
		data.pending.pop_front();
		Finalize(player, data, attempt);
	}
	data.pending.push_back({shot.id, shot.fireTime, shot.fireTick, 0, shot.airborne, noScope});
	if (IrregularDebug()) {
		ConsoleMessage("Irregular Behavior: %s recorded difficult shot %llu (%s%s).", player->GetName(), static_cast<unsigned long long>(shot.id),
					   shot.airborne ? "airborne" : "", shot.airborne && noScope ? " no-scope" : noScope ? "no-scope" : "");
	}
}

void IrregularBehaviorModule::OnPlayerDeath(const GameEventData& event, PlayerRef* attacker, PlayerRef* victim, ShotRecord& shot)
{
	if (!IsEligibleHuman(attacker) || !victim || attacker == victim || shot.playerIndex != attacker->index || shot.irregularConsumed) {
		return;
	}
	if (!AreOpponents(engine::PlayerTeam(attacker->index), engine::PlayerTeam(victim->index))) {
		return;
	}
	shot.irregularConsumed = true;
	auto& data = playerData[static_cast<size_t>(attacker->index)];
	auto attempt = std::find_if(data.pending.begin(), data.pending.end(), [&](const IrregularPending& pending) { return pending.shotId == shot.id; });
	if (attempt == data.pending.end()) {
		return;
	}

	const bool sniper = IsSniperWeapon(shot.weapon);
	const bool airborne = shot.airborne || event.attackerinair;
	const bool noScope = sniper && (!shot.scoped || event.noscope);
	attempt->airborne = airborne;
	attempt->noScope = noScope;

	const f32 distance = event.distance;
	if (!std::isfinite(distance) || distance < irregularMinimumDistance) {
		if (IrregularDebug()) {
			ConsoleMessage("Irregular Behavior: %s difficult kill rejected at %.1f metres.", attacker->GetName(), static_cast<double>(distance));
		}
		return;
	}

	attempt->success = true;
	attempt->points = static_cast<int>(airborne) + static_cast<int>(noScope) + 2 * static_cast<int>(airborne && noScope)
					  + static_cast<int>(distance >= longDistance) + static_cast<int>(event.penetrated) + 2 + static_cast<int>(event.headshot);
	if (IrregularDebug()) {
		ConsoleMessage("Irregular Behavior: %s matched difficult kill %llu worth %d points.", attacker->GetName(),
					   static_cast<unsigned long long>(shot.id), attempt->points);
	}
}

void IrregularBehaviorModule::OnGameFrame(int currentTick)
{
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& data = playerData[static_cast<size_t>(index)];
		// A record with no pending attempt and no evidence has nothing to finalize or prune, and
		// clearing it would write nothing observable - so the eligibility check below is only paid
		// for (and only clears) the slots that hold state, i.e. the ones this loop can act on.
		if (data.pending.empty() && data.evidence.empty()) {
			continue;
		}
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		if (!IsEligibleHuman(player)) {
			data = {};
			continue;
		}
		while (!data.pending.empty() && static_cast<i64>(currentTick) - data.pending.front().fireTick >= 2) {
			const IrregularPending attempt = data.pending.front();
			data.pending.pop_front();
			Finalize(player, data, attempt);
		}
		Prune(data, ToSecond(Clock::now()));
	}
}

void IrregularBehaviorModule::Finalize(PlayerRef* player, IrregularPlayerData& data, const IrregularPending& attempt)
{
	const i64 second = ToSecond(attempt.time);
	Prune(data, ToSecond(Clock::now()));
	if (data.evidence.empty() || data.evidence.back().second != second) {
		data.evidence.push_back({second});
	}
	auto& bucket = data.evidence.back();
	++bucket.attempts;
	bucket.successes += attempt.success;
	bucket.score += attempt.points;
	Evaluate(player, data);
}

void IrregularBehaviorModule::Prune(IrregularPlayerData& data, i64 nowSecond)
{
	while (!data.evidence.empty() && nowSecond - data.evidence.front().second >= irregularEvidenceWindowSeconds) {
		data.evidence.pop_front();
	}
}

void IrregularBehaviorModule::Evaluate(PlayerRef* player, IrregularPlayerData& data)
{
	int attempts = 0;
	int successes = 0;
	int score = 0;
	for (const auto& bucket : data.evidence) {
		attempts += bucket.attempts;
		successes += bucket.successes;
		score += bucket.score;
	}
	if (IrregularDebug()) {
		ConsoleMessage("Irregular Behavior: %s evidence %d/%d points from %d/%d difficult shots.", player->GetName(), score, irregularDetectionScore,
					   successes, attempts);
	}
	if (score < irregularDetectionScore || successes < minimumSuccesses || attempts < minimumAttempts || successes * 2 < attempts) {
		return;
	}
	if (announce) {
		announce("IRREGULAR BEHAVIOR", player,
				 localization::Format("evidence.irregular_behavior",
									  "The player repeatedly attempted airborne shots or sniper shots without scoping. {successes} of {attempts} "
									  "attempts became kills ({accuracy}% success), bringing the score to {score}/{threshold}.",
									  {{"score", FormatString("%d", score)},
									   {"threshold", FormatString("%d", irregularDetectionScore)},
									   {"successes", FormatString("%d", successes)},
									   {"attempts", FormatString("%d", attempts)},
									   {"accuracy", FormatString("%.1f", attempts ? successes * 100.0 / attempts : 0.0)}}));
	}
	data.evidence.clear();
	data.pending.clear();
}

void IrregularBehaviorModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
	}
}

// ---------------------------------------------------------------------------------------------
// Inhuman Accuracy (inhuman_accuracy_module.cpp)
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int accuracyEvidenceWindowSeconds = 5 * 60;
constexpr int accuracyMinimumAttempts = 40;
constexpr int requiredAccuracyPercent = 90;
constexpr f32 accuracyMinimumDistance = 100.0f;
constexpr f32 attemptHalfWidth = 32.0f;
static_assert(requiredAccuracyPercent <= 100, "accuracy is a percentage");

bool IsAccuracyWeapon(std::string_view weapon)
{
	weapon = NormalizeWeapon(weapon);
	static constexpr std::string_view weapons[] = {
		"ak47",    "aug",      "awp",     "bizon", "cz75a",         "deagle", "elite", "famas",        "fiveseven", "g3sg1",
		"galilar", "glock",    "hkp2000", "m4a1",  "m4a1_silencer", "mac10",  "mp5sd", "mp7",          "mp9",       "p250",
		"p90",     "revolver", "scar20",  "sg556", "ssg08",         "tec9",   "ump45", "usp_silencer",
	};
	return std::find(std::begin(weapons), std::end(weapons), weapon) != std::end(weapons);
}

f32 AccuracyAimError(const Vector& eye, const QAngle& angles, const Vector& target)
{
	Vector direction = target - eye;
	if (!IsFinite(direction) || direction.LengthSqr() < EPSILON) {
		return 180.0f;
	}
	direction.NormalizeInPlace();
	const f32 dot = std::clamp(DotProduct(AimForward(angles), direction), -1.0f, 1.0f);
	return static_cast<f32>(std::acos(dot) * (180.0 / M_PI));
}

f32 AccuracyNearestBodyAimError(const Vector& eye, const QAngle& angles, const Vector& feet)
{
	static constexpr f32 bodyHeights[] = {8.0f, 46.0f, 64.0f};
	f32 best = 180.0f;
	for (f32 height : bodyHeights) {
		best = (std::min)(best, AccuracyAimError(eye, angles, feet + Vector(0.0f, 0.0f, height)));
	}
	return best;
}
} // namespace

void InhumanAccuracyModule::Load(AnnounceCallback announceCallback, ShotCorrelator* shotCorrelator)
{
	announce = std::move(announceCallback);
	shots = shotCorrelator;
}

void InhumanAccuracyModule::Unload()
{
	Reset();
	shots = nullptr;
	announce = nullptr;
}

void InhumanAccuracyModule::Reset()
{
	playerData = {};
}

void InhumanAccuracyModule::OnGameFrame(int currentTick)
{
	if (!shots) {
		return;
	}
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& data = playerData[static_cast<size_t>(index)];
		auto& correlatorShots = shots->GetShots(index);
		// Nothing stored for this slot (no evidence of its own and no shot to finalize): clearing
		// the record and pruning it would both write nothing observable, so the eligibility check
		// below is only paid for the slots the loop can act on.
		if (data.evidence.empty() && correlatorShots.empty()) {
			continue;
		}
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		if (!IsEligibleHuman(player)) {
			data = {};
			continue;
		}

		for (auto& shot : correlatorShots) {
			const i64 age = static_cast<i64>(currentTick) - shot.fireTick;
			if (!shot.accuracyConsumed && age >= 2) {
				shot.accuracyConsumed = true;
				Finalize(player, shot);
			}
		}
		Prune(data, ToSecond(Clock::now()));
	}
}

void InhumanAccuracyModule::Finalize(PlayerRef* player, ShotRecord& shot)
{
	if (!IsAccuracyWeapon(shot.weapon) || !IsFinite(shot.angles) || !IsFinite(shot.eyePosition) || !shots) {
		return;
	}
	const PositionFrame* frame = shots->FindFrame(shot.serverTick);
	if (!frame) {
		if (AccuracyDebug()) {
			ConsoleMessage("Inhuman Accuracy: %s shot %llu rejected because its historical frame is missing.", player->GetName(),
						   static_cast<unsigned long long>(shot.id));
		}
		return;
	}
	const auto& attacker = frame->players[static_cast<size_t>(player->index)];
	if (!attacker.valid || attacker.teleported) {
		return;
	}

	int matchedTarget = -1;
	f32 matchedError = 180.0f;
	for (int targetIndex = 1; targetIndex <= MAXPLAYERS; ++targetIndex) {
		const auto& target = frame->players[static_cast<size_t>(targetIndex)];
		if ((shot.hurtSeen && targetIndex != shot.victimIndex) || targetIndex == player->index || !target.valid || target.teleported
			|| !AreOpponents(attacker.team, target.team) || (!shot.hurtSeen && !target.alive)) {
			continue;
		}
		const f32 distance = (target.origin - shot.eyePosition).Length();
		const f32 error = AccuracyNearestBodyAimError(shot.eyePosition, shot.angles, target.origin);
		if (!std::isfinite(distance) || distance < accuracyMinimumDistance || !std::isfinite(error)) {
			continue;
		}
		const f32 aimTolerance = static_cast<f32>(std::atan2(attemptHalfWidth, distance) * (180.0 / M_PI));
		if (error > aimTolerance) {
			continue;
		}
		if (error < matchedError) {
			matchedTarget = targetIndex;
			matchedError = error;
		}
	}
	if (matchedTarget == -1) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	const i64 second = ToSecond(shot.fireTime);
	Prune(data, ToSecond(Clock::now()));
	if (data.evidence.empty() || data.evidence.back().second != second) {
		data.evidence.push_back({second});
	}
	++data.evidence.back().attempts;
	data.evidence.back().hits += shot.hurtSeen;

	int attempts = 0;
	int hits = 0;
	for (const auto& bucket : data.evidence) {
		attempts += bucket.attempts;
		hits += bucket.hits;
	}
	if (AccuracyDebug()) {
		ConsoleMessage("Inhuman Accuracy: %s shot %llu counted at %.3f degrees: %d/%d hits.", player->GetName(),
					   static_cast<unsigned long long>(shot.id), static_cast<double>(matchedError), hits, attempts);
	}
	if (attempts < accuracyMinimumAttempts || hits * 100 < attempts * requiredAccuracyPercent) {
		return;
	}
	if (announce) {
		announce("INHUMAN ACCURACY", player,
				 localization::Format("evidence.inhuman_accuracy",
									  "CS2AC counted {attempts} shots fired while the crosshair was already inside a player-sized area around an "
									  "enemy at least {distance} game units away. {hits} dealt damage ({accuracy}% accuracy); {required}% was "
									  "required.",
									  {{"hits", FormatString("%d", hits)},
									   {"attempts", FormatString("%d", attempts)},
									   {"accuracy", FormatString("%.1f", attempts ? hits * 100.0 / attempts : 0.0)},
									   {"distance", FormatString("%.0f", static_cast<double>(accuracyMinimumDistance))},
									   {"required", FormatString("%d", requiredAccuracyPercent)}}));
	}
	data.evidence.clear();
}

void InhumanAccuracyModule::Prune(AccuracyPlayerData& data, i64 nowSecond)
{
	while (!data.evidence.empty() && nowSecond - data.evidence.front().second >= accuracyEvidenceWindowSeconds) {
		data.evidence.pop_front();
	}
}

void InhumanAccuracyModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
	}
}

} // namespace detection
} // namespace cs2ac
