// cs2ac — the Aimbot detector (detection/aimbot_module.cpp).
//
// All thresholds, windows, rules and debug lines are the original's; only the data sources changed
// (UsercmdView instead of the hl2sdk protobuf accessors, the engine:: accessors instead of the SDK
// entity reads). Aimlock and AntiAim live in mod_aimlock.cpp / mod_antiaim.cpp so that each module
// keeps its own constant block, exactly like the original's separate translation units.

#include "cs2ac.hpp"

#include <numeric>

namespace cs2ac {
namespace detection {

namespace {
bool AimbotDebug() { return settings::DebugFlag("cs2ac_aimbot_debug"); }

constexpr size_t commandHistorySize = 128;
constexpr int snapWindowTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 0.5f);
constexpr f32 minimumDistance = 64.0f;
constexpr f32 closeRangeDistance = 100.0f;
constexpr int snapDetectionThreshold = 3;
constexpr int smoothDetectionThreshold = 3;
constexpr auto evidenceWindow = std::chrono::minutes(5);
// A confirmed smoothed-aimbot demo produced 12 damaging curves inside this envelope: 13.66-29.69 degrees of travel,
// 94.9-99.5% error removal, 0.12-0.85 degrees of final error, 94.6-100% path efficiency, and 3-5 shrinking steps.
constexpr f32 smoothMinimumMovement = 13.0f;
constexpr f32 smoothMinimumImprovement = 0.94f;
constexpr f32 smoothMaximumFinalError = 1.0f;
constexpr f32 smoothMinimumPathEfficiency = 0.94f;
constexpr f32 smoothMinimumStep = 0.15f;
constexpr f32 smoothRelativeStep = 0.10f;
constexpr int smoothMinimumSteps = 3;
constexpr f32 bodyHeights[] = {8.0f, 46.0f, 64.0f};

static_assert(MeetsSmoothThresholds(13.66f, 0.949f, 0.85f, 0.946f, 3, true), "the smooth envelope must accept the confirmed demo");
static_assert(!MeetsSmoothThresholds(6.31f, 0.934f, 0.41f, 1.0f, 3, true), "the smooth envelope must reject the confirmed negative");

enum class AimbotRule {
	None,
	Convergence,
	SnapReturn,
	SmoothConvergence,
};

f32 AimError(const Vector& eye, const QAngle& angles, const Vector& target)
{
	Vector direction = target - eye;
	if (!IsFinite(direction) || direction.LengthSqr() < EPSILON) {
		return 0.0f;
	}
	direction.NormalizeInPlace();
	const f32 dot = std::clamp(DotProduct(AimForward(angles), direction), -1.0f, 1.0f);
	return static_cast<f32>(std::acos(dot) * (180.0 / M_PI));
}

f32 NearestBodyAimError(const Vector& eye, const QAngle& angles, const Vector& feet)
{
	f32 best = 180.0f;
	for (f32 height : bodyHeights) {
		best = (std::min)(best, AimError(eye, angles, feet + Vector(0.0f, 0.0f, height)));
	}
	return best;
}
} // namespace

void AimbotModule::Load(AnnounceCallback announceCallback, ShotCorrelator* shotCorrelator)
{
	announce = std::move(announceCallback);
	shots = shotCorrelator;
}

void AimbotModule::Unload()
{
	Reset();
	shots = nullptr;
	announce = nullptr;
}

void AimbotModule::Reset()
{
	playerData = {};
	snapEvidence = {};
	smoothEvidence = {};
}

void AimbotModule::OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands)
{
	if (!IsEligibleHuman(player) || commands.empty()) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	for (const UsercmdView& command : commands) {
		if (!command.hasBase || !command.hasViewAngles) {
			continue;
		}
		if (std::any_of(data.commands.rbegin(), data.commands.rend(),
						[&](const AimCommand& stored) { return stored.commandNumber == command.commandNumber; })) {
			continue;
		}

		QAngle angles = command.viewAngles;
		const int attackIndex = command.attack1StartHistoryIndex;
		if (attackIndex >= 0 && attackIndex < static_cast<int>(command.inputHistory.size())
			&& command.inputHistory[static_cast<size_t>(attackIndex)].hasViewAngles) {
			const QAngle firingAngles = command.inputHistory[static_cast<size_t>(attackIndex)].viewAngles;
			if (IsFinite(firingAngles)) {
				angles = firingAngles;
			}
		}
		if (!IsFinite(angles)) {
			continue;
		}

		AimCommand captured {};
		captured.commandNumber = command.commandNumber;
		captured.clientTick = command.clientTick;
		captured.serverTick = -1;
		captured.angles = angles;
		data.commands.push_back(captured);
		while (data.commands.size() > commandHistorySize) {
			data.commands.pop_front();
		}
	}
}

void AimbotModule::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	if (!IsEligibleHuman(player) || !command.hasBase) {
		return;
	}
	auto& data = playerData[static_cast<size_t>(player->index)];
	auto found = std::find_if(data.commands.rbegin(), data.commands.rend(),
							  [&](const AimCommand& stored) { return stored.commandNumber == command.commandNumber; });
	if (found == data.commands.rend()) {
		return;
	}
	Vector eye;
	if (!engine::ReadEyeOrigin(player->index, eye) || !IsFinite(eye)) {
		return;
	}
	found->serverTick = currentTick;
	found->eyePosition = eye;
	found->simulated = true;
	if (!data.pending.empty()) {
		Evaluate(player, data, currentTick);
	}
}

void AimbotModule::OnGameFrame(int currentTick)
{
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		if (playerData[static_cast<size_t>(index)].pending.empty()) {
			continue;
		}
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		if (!IsEligibleHuman(player)) {
			playerData[static_cast<size_t>(index)] = {};
			continue;
		}
		Evaluate(player, playerData[static_cast<size_t>(index)], currentTick);
	}
}

void AimbotModule::OnPlayerHurt(PlayerRef* attacker, PlayerRef* victim, ShotRecord& shot)
{
	if (!IsEligibleHuman(attacker) || !victim || attacker == victim || shot.playerIndex != attacker->index || shot.aimbotConsumed) {
		return;
	}
	shot.aimbotConsumed = true;
	auto& data = playerData[static_cast<size_t>(attacker->index)];
	if (!data.pending.empty()) {
		Evaluate(attacker, data, shot.fireTick);
	}
	data.pending.push_back({shot.commandNumber, victim->index});
	while (data.pending.size() > 8) {
		data.pending.pop_front();
	}
	if (AimbotDebug()) {
		ConsoleMessage("Aimbot: %s matched damaging shot command %d at server tick %d.", attacker->GetName(), shot.commandNumber, shot.serverTick);
	}
	Evaluate(attacker, data, shot.fireTick);
}

bool AimbotModule::Evaluate(PlayerRef* attacker, AimbotPlayerData& data, int currentTick)
{
	if (data.pending.empty() || !shots) {
		return true;
	}
	const int pendingShot = data.pending.front().commandNumber;
	const int victimIndex = data.pending.front().victimIndex;
	auto clearPending = [&]() {
		if (!data.pending.empty()) {
			data.pending.pop_front();
		}
	};

	auto shot = std::find_if(data.commands.begin(), data.commands.end(),
							 [&](const AimCommand& command) { return command.commandNumber == pendingShot; });
	if (shot == data.commands.end() || !shot->simulated || victimIndex < 1 || victimIndex > MAXPLAYERS) {
		clearPending();
		return true;
	}

	const TrackedPosition* shotTarget = shots->FindPosition(shot->serverTick, victimIndex);
	const TrackedPosition* shotAttacker = shots->FindPosition(shot->serverTick, attacker->index);
	if (!shotTarget || !shotAttacker) {
		if (currentTick <= shot->serverTick) {
			return false;
		}
		if (AimbotDebug()) {
			ConsoleMessage("Aimbot: %s rejected because target history is missing for server tick %d.", attacker->GetName(), shot->serverTick);
		}
		clearPending();
		return true;
	}
	if (shotTarget->teleported || shotAttacker->teleported) {
		if (AimbotDebug()) {
			ConsoleMessage("Aimbot: %s rejected because a player teleported inside the snap window.", attacker->GetName());
		}
		clearPending();
		return true;
	}
	if (!AreOpponents(shotAttacker->team, shotTarget->team)) {
		if (AimbotDebug()) {
			ConsoleMessage("Aimbot: %s rejected because the damaging shot was not against an enemy.", attacker->GetName());
		}
		clearPending();
		return true;
	}
	const f32 distance = (shotTarget->origin - shotAttacker->eyePosition).Length();
	if (!std::isfinite(distance) || distance < minimumDistance) {
		if (AimbotDebug()) {
			ConsoleMessage("Aimbot: %s rejected because target distance %.1f is below %.0f.", attacker->GetName(), static_cast<double>(distance),
						   static_cast<double>(minimumDistance));
		}
		clearPending();
		return true;
	}
	// Close-range aim snaps cover a much larger angular change for the same physical movement, so a
	// valid target position is not enough on its own. Require a tighter convergence and a clearer
	// snap-return at close range (fork import).
	const bool closeRange = distance < closeRangeDistance;

	bool suspicious = false;
	f32 largestSnap = 0.0f;
	f32 bestBefore = 0.0f;
	f32 bestAfter = 0.0f;
	f32 largestMeasuredSnap = -1.0f;
	f32 measuredBefore = 0.0f;
	f32 measuredAfter = 0.0f;
	int measuredMovements = 0;
	const char* measurementFailure = "no continuous adjacent command history was available";
	bool reusedSnap = false;
	bool commandDiscontinuity = false;
	AimbotRule matchedRule = AimbotRule::None;

	struct SmoothPoint {
		AimCommand* command;
		f32 error;
	};

	int smoothBodyPoint = 0;
	f32 smoothShotError = 180.0f;
	for (int bodyPoint = 0; bodyPoint < static_cast<int>(std::size(bodyHeights)); ++bodyPoint) {
		const f32 error = AimError(shotAttacker->eyePosition, shot->angles, shotTarget->origin + Vector(0.0f, 0.0f, bodyHeights[bodyPoint]));
		if (error < smoothShotError) {
			smoothShotError = error;
			smoothBodyPoint = bodyPoint;
		}
	}
	std::vector<SmoothPoint> smoothPoints {{&*shot, smoothShotError}};
	f32 smoothMovement = 0.0f;
	f32 smoothBefore = 0.0f;
	f32 smoothAfter = 0.0f;
	f32 smoothEfficiency = 0.0f;
	int smoothSteps = 0;
	auto findCommand = [&](int commandNumber) -> AimCommand* {
		auto found = std::find_if(data.commands.begin(), data.commands.end(),
								  [&](AimCommand& command) { return command.commandNumber == commandNumber && command.simulated; });
		return found == data.commands.end() ? nullptr : &*found;
	};
	AimCommand* newer = &*shot;
	while (newer->commandNumber > (std::numeric_limits<int>::min)()) {
		AimCommand* older = findCommand(newer->commandNumber - 1);
		if (!older) {
			measurementFailure = "the previous simulated command was missing";
			break;
		}
		const i64 serverGap = static_cast<i64>(newer->serverTick) - older->serverTick;
		if (static_cast<i64>(newer->clientTick) - older->clientTick != 1 || serverGap < 0 || serverGap > 1) {
			// A dropped, batched or replayed command makes this one pair unmeasurable. Skip only that
			// pair and keep walking through the continuously simulated history; discontinuity is
			// supporting context and must never be the only reason for a ban.
			commandDiscontinuity = true;
			newer = older;
			continue;
		}
		if (static_cast<i64>(shot->serverTick) - older->serverTick > snapWindowTicks) {
			measurementFailure = "the preceding command was outside the 0.5-second snap window";
			break;
		}

		const TrackedPosition* olderTarget = shots->FindPosition(older->serverTick, victimIndex);
		const TrackedPosition* newerTarget = shots->FindPosition(newer->serverTick, victimIndex);
		const TrackedPosition* olderAttacker = shots->FindPosition(older->serverTick, attacker->index);
		const TrackedPosition* newerAttacker = shots->FindPosition(newer->serverTick, attacker->index);
		if (!olderTarget || !newerTarget || !olderAttacker || !newerAttacker || olderTarget->teleported || newerTarget->teleported
			|| olderAttacker->teleported || newerAttacker->teleported) {
			measurementFailure = "matching historical player positions were missing or interrupted by a teleport";
			break;
		}
		const f32 snap = AngularDistance(older->angles, newer->angles);
		const f32 before = NearestBodyAimError(olderAttacker->eyePosition, older->angles, olderTarget->origin);
		const f32 after = NearestBodyAimError(newerAttacker->eyePosition, newer->angles, newerTarget->origin);
		if (!std::isfinite(snap) || !std::isfinite(before) || !std::isfinite(after)) {
			measurementFailure = "an angle or target-error measurement was invalid";
			break;
		}
		const f32 smoothError =
			AimError(olderAttacker->eyePosition, older->angles, olderTarget->origin + Vector(0.0f, 0.0f, bodyHeights[smoothBodyPoint]));
		if (!std::isfinite(smoothError)) {
			measurementFailure = "a fixed-body target-error measurement was invalid";
			break;
		}
		smoothPoints.push_back({older, smoothError});
		++measuredMovements;
		if (snap > largestMeasuredSnap) {
			largestMeasuredSnap = snap;
			measuredBefore = before;
			measuredAfter = after;
		}
		const bool converged = closeRange ? (before >= 30.0f && snap > 12.0f && after < before * 0.1f)
										  : ((before >= 20.0f && snap > 10.0f && after < before * 0.2f)
											 || (before >= 30.0f && snap > 5.0f && after < before * 0.1f));
		const bool fresh = !data.hasCountedIncident || newer->commandNumber > data.lastCountedIncidentCommand;
		if (converged && fresh) {
			suspicious = true;
			matchedRule = AimbotRule::Convergence;
			if (snap > largestSnap) {
				largestSnap = snap;
				bestBefore = before;
				bestAfter = after;
			}
		} else if (converged) {
			reusedSnap = true;
		}
		newer = older;
	}

	AimCommand* previous = pendingShot > (std::numeric_limits<int>::min)() ? findCommand(pendingShot - 1) : nullptr;
	AimCommand* next = pendingShot < (std::numeric_limits<int>::max)() ? findCommand(pendingShot + 1) : nullptr;
	if (!suspicious && !next && static_cast<i64>(currentTick) - shot->serverTick <= 1) {
		return false;
	}
	if (previous && next && shot->commandNumber - previous->commandNumber == 1 && next->commandNumber - shot->commandNumber == 1
		&& static_cast<i64>(shot->clientTick) - previous->clientTick == 1
		&& static_cast<i64>(next->clientTick) - shot->clientTick == 1
		&& static_cast<i64>(shot->serverTick) - previous->serverTick >= 0
		&& static_cast<i64>(shot->serverTick) - previous->serverTick <= 1
		&& static_cast<i64>(next->serverTick) - shot->serverTick >= 0
		&& static_cast<i64>(next->serverTick) - shot->serverTick <= 1) {
		const f32 surrounding = AngularDistance(previous->angles, next->angles);
		const f32 snap = AngularDistance(previous->angles, shot->angles);
		const bool fresh = !data.hasCountedIncident || shot->commandNumber > data.lastCountedIncidentCommand;
		const f32 surroundingLimit = closeRange ? 8.0f : 10.0f;
		const f32 snapMinimum = closeRange ? 12.0f : 5.0f;
		if (fresh && std::isfinite(surrounding) && std::isfinite(snap) && surrounding < surroundingLimit && snap > snapMinimum
			&& snap > surrounding * 5.0f) {
			if (!suspicious) {
				matchedRule = AimbotRule::SnapReturn;
				largestSnap = snap;
			}
			suspicious = true;
		}
	}

	// The smooth-curve rule compares consecutive points, so a command gap would make two
	// non-adjacent commands look like one step. Disable it when the history was discontinuous; the
	// pairwise convergence rule stays valid.
	if (!suspicious && !commandDiscontinuity && smoothPoints.size() >= static_cast<size_t>(smoothMinimumSteps + 1)) {
		std::reverse(smoothPoints.begin(), smoothPoints.end());
		std::vector<f32> movements;
		movements.reserve(smoothPoints.size() - 1);
		for (size_t index = 1; index < smoothPoints.size(); ++index) {
			movements.push_back(AngularDistance(smoothPoints[index - 1].command->angles, smoothPoints[index].command->angles));
		}
		const auto largest = std::max_element(movements.begin(), movements.end());
		const size_t start = static_cast<size_t>(std::distance(movements.begin(), largest));
		const f32 path = std::accumulate(movements.begin() + static_cast<std::ptrdiff_t>(start), movements.end(), 0.0f);
		smoothMovement = AngularDistance(smoothPoints[start].command->angles, smoothPoints.back().command->angles);
		smoothBefore = smoothPoints[start].error;
		smoothAfter = smoothPoints.back().error;
		smoothEfficiency = path > EPSILON ? smoothMovement / path : 0.0f;
		const f32 meaningfulMinimum = (std::max)(smoothMinimumStep, *largest * smoothRelativeStep);
		f32 previousMeaningful = std::numeric_limits<f32>::infinity();
		bool shrinking = true;
		for (size_t index = start; index < movements.size(); ++index) {
			if (movements[index] < meaningfulMinimum) {
				continue;
			}
			if (movements[index] >= previousMeaningful) {
				shrinking = false;
			}
			previousMeaningful = movements[index];
			++smoothSteps;
		}
		const f32 improvement = smoothBefore > EPSILON ? 1.0f - smoothAfter / smoothBefore : 0.0f;
		const bool fresh = !data.hasCountedIncident || shot->commandNumber > data.lastCountedIncidentCommand;
		if (fresh && MeetsSmoothThresholds(smoothMovement, improvement, smoothAfter, smoothEfficiency, smoothSteps, shrinking)) {
			suspicious = true;
			matchedRule = AimbotRule::SmoothConvergence;
			largestSnap = smoothMovement;
			bestBefore = smoothBefore;
			bestAfter = smoothAfter;
		} else if (AimbotDebug()) {
			ConsoleMessage("Aimbot: %s smooth curve rejected: movement %.2f/%.2f, target error %.2f -> %.2f/%.2f (%.1f%%/%.0f%% closer), "
						   "path %.1f%%/%.0f%%, steps %d/%d, shrinking %s.",
						   attacker->GetName(), static_cast<double>(smoothMovement), static_cast<double>(smoothMinimumMovement),
						   static_cast<double>(smoothBefore), static_cast<double>(smoothAfter), static_cast<double>(smoothMaximumFinalError),
						   static_cast<double>(improvement * 100.0f), static_cast<double>(smoothMinimumImprovement * 100.0f),
						   static_cast<double>(smoothEfficiency * 100.0f), static_cast<double>(smoothMinimumPathEfficiency * 100.0f), smoothSteps,
						   smoothMinimumSteps, shrinking ? "yes" : "no");
		}
	}

	const int incidentCommand = shot->commandNumber;
	clearPending();
	if (!suspicious) {
		if (reusedSnap) {
			if (AimbotDebug()) {
				ConsoleMessage("Aimbot: %s ignored a snap that already counted for an earlier damaging shot.", attacker->GetName());
			}
		} else if (AimbotDebug()) {
			if (commandDiscontinuity) {
				ConsoleMessage("Aimbot: %s noted a command discontinuity while scanning; it is context only and was not scored.",
							   attacker->GetName());
			}
			if (measuredMovements == 0) {
				ConsoleMessage("Aimbot: %s damaging shot rejected because %s.", attacker->GetName(), measurementFailure);
			} else {
				const f32 improvement = measuredBefore > EPSILON ? (1.0f - measuredAfter / measuredBefore) * 100.0f : 0.0f;
				const f32 requiredImprovement = largestMeasuredSnap > 10.0f ? 80.0f : 90.0f;
				ConsoleMessage("Aimbot: %s damaging shot rejected after checking %d adjacent movements: the largest one-command movement was %.2f "
							   "degrees with target error %.2f -> %.2f (%.1f%% closer); this movement requires more than %.0f%% improvement.",
							   attacker->GetName(), measuredMovements, static_cast<double>(largestMeasuredSnap), static_cast<double>(measuredBefore),
							   static_cast<double>(measuredAfter), static_cast<double>(improvement), static_cast<double>(requiredImprovement));
			}
		}
		return true;
	}

	data.lastCountedIncidentCommand = incidentCommand;
	data.hasCountedIncident = true;
	const auto now = Clock::now();
	auto purge = [&](std::deque<Clock::time_point>& incidents) {
		while (!incidents.empty() && now - incidents.front() >= evidenceWindow) {
			incidents.pop_front();
		}
	};
	auto& snapIncidents = snapEvidence[static_cast<size_t>(attacker->index)];
	auto& smoothIncidents = smoothEvidence[static_cast<size_t>(attacker->index)];
	purge(snapIncidents);
	purge(smoothIncidents);
	auto& incidents = matchedRule == AimbotRule::SmoothConvergence ? smoothIncidents : snapIncidents;
	incidents.push_back(now);
	const int threshold = matchedRule == AimbotRule::SmoothConvergence ? smoothDetectionThreshold : snapDetectionThreshold;
	if (AimbotDebug()) {
		ConsoleMessage("Aimbot: %s counted rule %d evidence %d/%d (snap %.2f, target error %.2f -> %.2f).", attacker->GetName(),
					   static_cast<int>(matchedRule), static_cast<int>(incidents.size()), threshold, static_cast<double>(largestSnap),
					   static_cast<double>(bestBefore), static_cast<double>(bestAfter));
	}
	if (incidents.size() >= static_cast<size_t>(threshold)) {
		if (announce) {
			const auto values = localization::Arguments {{"incidents", FormatString("%zu", incidents.size())},
														 {"snap", FormatString("%.2f", static_cast<double>(largestSnap))},
														 {"before", FormatString("%.2f", static_cast<double>(bestBefore))},
														 {"after", FormatString("%.2f", static_cast<double>(bestAfter))}};
			const localization::Text details =
				matchedRule == AimbotRule::SnapReturn
					? localization::Format("evidence.aimbot.snap_return",
										   "CS2AC found {incidents} damaging shots where the aim jumped during the shot and immediately returned. "
										   "The latest jump was {snap} degrees.",
										   values)
				: matchedRule == AimbotRule::SmoothConvergence
					? localization::Format("evidence.aimbot.smooth",
										   "CS2AC found {incidents} damaging shots where the aim followed an unusually clean curve onto the enemy. "
										   "On the latest shot, the aim moved {snap} degrees and its distance from the enemy fell from {before} to "
										   "{after} degrees.",
										   values)
					: localization::Format("evidence.aimbot.convergence",
										   "CS2AC found {incidents} damaging shots where the aim moved onto the enemy in one sudden step. On the "
										   "latest shot, the aim moved {snap} degrees and its distance from the enemy fell from {before} to {after} "
										   "degrees.",
										   values);
			announce("AIMBOT", attacker, details);
		}
		snapIncidents.clear();
		smoothIncidents.clear();
	}
	return true;
}

void AimbotModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
		snapEvidence[static_cast<size_t>(player->index)].clear();
		smoothEvidence[static_cast<size_t>(player->index)].clear();
	}
}

} // namespace detection
} // namespace cs2ac
