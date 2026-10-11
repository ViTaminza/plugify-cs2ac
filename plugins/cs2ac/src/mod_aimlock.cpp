// cs2ac — the Aimlock detector (detection/aimlock_module.cpp).
//
// Identical thresholds and rules; the visual-lag estimate reads s2sdk's latency plus the client's
// cl_interp_ratio cvar (the original read INetChannelInfo::GetEngineLatency and the engine's
// GetClientConVarValue).

#include "cs2ac.hpp"

namespace cs2ac {
namespace detection {

namespace {
bool AimlockDebug() { return settings::DebugFlag("cs2ac_aimlock_debug"); }

constexpr int trackingTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 2.0f);
constexpr int rearmTicks = static_cast<int>(ENGINE_FIXED_TICK_RATE * 0.5f);
constexpr int lagSearchRadius = 2;
constexpr f32 playerHalfWidth = 16.0f; // The CS2 player hull is 32 units wide; aim is measured from its center.
constexpr f32 aimlockMinimumDistance = 200.0f;
constexpr f32 minimumTargetTravel = 128.0f; // Four player widths, converted to degrees at the episode's starting distance.
constexpr f32 maximumInterpolationTicks = 19.0f;
constexpr int aimlockDetectionThreshold = 3;
constexpr auto aimlockEvidenceWindow = std::chrono::minutes(5);
constexpr f32 aimlockBodyHeights[] = {8.0f, 46.0f, 64.0f};

static_assert(MeetsCoverage(123, 129), "the 19/20 coverage rule must accept 123 of 129");
static_assert(!MeetsCoverage(122, 129), "the 19/20 coverage rule must reject 122 of 129");
static_assert(std::tuple_size<decltype(AimlockTrack::hypotheses)>::value == lagSearchRadius * 2 + 1, "one hypothesis per fixed delay");

struct Candidate {
	QAngle bearing;
	f32 error {180.0f};
	int targetIndex {-1};
	int bodyPoint {-1};
	int lagTicks {};
	bool valid {};
};

struct LagEstimate {
	int ticks {};
	f32 roundTripMilliseconds {};
	f32 interpolationTicks {};
	bool valid {};
};

QAngle Bearing(const Vector& eye, const Vector& target)
{
	const Vector delta = target - eye;
	constexpr f32 radiansToDegrees = static_cast<f32>(180.0 / M_PI);
	const f32 horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
	return {-std::atan2(delta.z, horizontal) * radiansToDegrees, std::atan2(delta.y, delta.x) * radiansToDegrees, 0.0f};
}

void ClearTrack(AimlockPlayerData& data)
{
	data.track = {};
}

void ClearRuntime(AimlockPlayerData& data)
{
	data = {};
}

// aimlock_module.cpp EstimateVisualLag, on the port's data sources.
LagEstimate EstimateVisualLag(PlayerRef* player)
{
	LagEstimate estimate;
	if (!player) {
		return estimate;
	}
	const engine::NetStats stats = engine::ClientNetStats(player->index);
	if (!stats.pingAvailable) {
		return estimate;
	}
	const char* interpolationValue = engine::ClientCvarValue(player->index, "cl_interp_ratio");
	if (!IsNumeric(interpolationValue)) {
		return estimate;
	}

	const f32 roundTripSeconds = stats.pingSeconds;
	f32 interpolationTicks = static_cast<f32>(std::strtod(interpolationValue, nullptr));
	if (!std::isfinite(roundTripSeconds) || roundTripSeconds < 0.0f || roundTripSeconds > 2.0f || !std::isfinite(interpolationTicks)
		|| interpolationTicks < 0.0f || interpolationTicks > maximumInterpolationTicks) {
		return estimate;
	}
	if (interpolationTicks == 0.0f) {
		interpolationTicks = 1.0f;
	}

	// The target snapshot travelled to the client before this aim command travelled back to the server, so use the full RTT.
	estimate.ticks = (std::max)(0, static_cast<int>(std::lround(roundTripSeconds * ENGINE_FIXED_TICK_RATE + interpolationTicks)));
	estimate.roundTripMilliseconds = roundTripSeconds * 1000.0f;
	estimate.interpolationTicks = interpolationTicks;
	estimate.valid = true;
	return estimate;
}

const AimlockLagHypothesis* BestHypothesis(const AimlockTrack& track, bool requireEvidence)
{
	const AimlockLagHypothesis* best = nullptr;
	for (int index = 0; index < track.hypothesisCount; ++index) {
		const auto& hypothesis = track.hypotheses[static_cast<size_t>(index)];
		if (!hypothesis.valid
			|| (requireEvidence
				&& (!MeetsCoverage(hypothesis.onTargetSamples, track.samples)
					|| hypothesis.maximumTargetDisplacement < hypothesis.requiredTargetDisplacement))) {
			continue;
		}
		if (!best || hypothesis.onTargetSamples > best->onTargetSamples) {
			best = &hypothesis;
		}
	}
	return best;
}

bool MeasureTarget(const AimlockSample& sample, const TrackedPosition& target, int bodyPoint, const Vector& aimForward, QAngle& bearing, f32& error,
				   f32& maximumError, f32* requiredTargetDisplacement)
{
	if (bodyPoint < 0 || bodyPoint >= static_cast<int>(std::size(aimlockBodyHeights))) {
		return false;
	}

	const Vector targetPoint = target.origin + Vector(0.0f, 0.0f, aimlockBodyHeights[bodyPoint]);
	Vector direction = targetPoint - sample.eyePosition;
	const f32 targetDistance = direction.Length();
	if (!IsFinite(targetPoint) || !IsFinite(direction) || !std::isfinite(targetDistance) || targetDistance < EPSILON) {
		return false;
	}
	direction.NormalizeInPlace();
	const f32 dot = std::clamp(DotProduct(aimForward, direction), -1.0f, 1.0f);
	error = static_cast<f32>(std::acos(dot) * (180.0 / M_PI));
	maximumError = static_cast<f32>(std::atan2(playerHalfWidth, targetDistance) * (180.0 / M_PI));
	if (requiredTargetDisplacement) {
		*requiredTargetDisplacement = static_cast<f32>(std::atan2(minimumTargetTravel, targetDistance) * (180.0 / M_PI));
	}
	bearing = Bearing(sample.eyePosition, targetPoint);
	return std::isfinite(error) && std::isfinite(maximumError) && (!requiredTargetDisplacement || std::isfinite(*requiredTargetDisplacement))
		   && IsFinite(bearing);
}

bool EvaluateTarget(const AimlockSample& sample, const PositionFrame& currentFrame, const PositionFrame& historicalFrame, int observerIndex,
					int targetIndex, int bodyPoint, const Vector& aimForward, QAngle& bearing, f32& error, f32& maximumError,
					f32* requiredTargetDisplacement)
{
	if (observerIndex < 1 || observerIndex > MAXPLAYERS || targetIndex < 1 || targetIndex > MAXPLAYERS || !IsFinite(sample.eyePosition)
		|| !IsFinite(sample.angles)) {
		return false;
	}

	const auto& observer = currentFrame.players[static_cast<size_t>(observerIndex)];
	const auto& currentTarget = currentFrame.players[static_cast<size_t>(targetIndex)];
	const auto& target = historicalFrame.players[static_cast<size_t>(targetIndex)];
	if (!observer.valid || !observer.alive || observer.teleported || !currentTarget.valid || !currentTarget.alive || currentTarget.teleported
		|| !AreOpponents(observer.team, currentTarget.team) || !target.valid || !target.alive || target.teleported
		|| !AreOpponents(observer.team, target.team) || (target.origin - sample.eyePosition).Length() < aimlockMinimumDistance) {
		return false;
	}

	return MeasureTarget(sample, target, bodyPoint, aimForward, bearing, error, maximumError, requiredTargetDisplacement);
}

Candidate FindCandidate(const ShotCorrelator* shots, const AimlockSample& sample, const PositionFrame& currentFrame, int observerIndex,
						const LagEstimate& estimate)
{
	Candidate best;
	if (!shots || !estimate.valid || observerIndex < 1 || observerIndex > MAXPLAYERS || !IsFinite(sample.eyePosition) || !IsFinite(sample.angles)) {
		return best;
	}

	const auto& observer = currentFrame.players[static_cast<size_t>(observerIndex)];
	if (!observer.valid || !observer.alive || observer.teleported) {
		return best;
	}
	const Vector aimForward = AimForward(sample.angles);
	int matchedTarget = -1;
	bool ambiguous = false;
	const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
	const int lastLag = estimate.ticks + lagSearchRadius;
	for (int lagTicks = firstLag; lagTicks <= lastLag; ++lagTicks) {
		const PositionFrame* historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
		if (!historicalFrame) {
			continue;
		}
		for (int targetIndex = 1; targetIndex <= MAXPLAYERS; ++targetIndex) {
			if (targetIndex == observerIndex) {
				continue;
			}
			const auto& currentTarget = currentFrame.players[static_cast<size_t>(targetIndex)];
			const auto& target = historicalFrame->players[static_cast<size_t>(targetIndex)];
			if (!currentTarget.valid || !currentTarget.alive || currentTarget.teleported || !AreOpponents(observer.team, currentTarget.team)
				|| !target.valid || !target.alive || target.teleported || !AreOpponents(observer.team, target.team)
				|| (target.origin - sample.eyePosition).Length() < aimlockMinimumDistance) {
				continue;
			}
			for (int bodyPoint = 0; bodyPoint < static_cast<int>(std::size(aimlockBodyHeights)); ++bodyPoint) {
				QAngle bearing;
				f32 error = 180.0f;
				f32 maximumError = 0.0f;
				if (!MeasureTarget(sample, target, bodyPoint, aimForward, bearing, error, maximumError, nullptr) || error > maximumError) {
					continue;
				}

				if (matchedTarget < 0) {
					matchedTarget = targetIndex;
				} else if (matchedTarget != targetIndex) {
					ambiguous = true;
				}
				if (error < best.error) {
					best = {bearing, error, targetIndex, bodyPoint, lagTicks, true};
				}
			}
		}
	}

	best.valid = best.valid && !ambiguous;
	return best;
}
} // namespace

void AimlockModule::Load(AnnounceCallback announceCallback, ShotCorrelator* shotCorrelator)
{
	announce = std::move(announceCallback);
	shots = shotCorrelator;
}

void AimlockModule::Unload()
{
	Reset();
	shots = nullptr;
	announce = nullptr;
}

void AimlockModule::Reset()
{
	playerData = {};
	evidence = {};
}

void AimlockModule::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	if (!IsEligibleHuman(player) || !command.hasBase) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	if (currentTick == data.lastProcessedTick) {
		return;
	}
	if (!command.hasViewAngles) {
		ClearTrack(data);
		data.pending = {};
		return;
	}

	const QAngle angles = command.viewAngles;
	Vector eye;
	if (!IsFinite(angles) || !engine::ReadEyeOrigin(player->index, eye) || !IsFinite(eye)) {
		ClearTrack(data);
		data.pending = {};
		return;
	}

	// SetupMove is the authoritative gate: this is the last command the server will actually simulate for this tick.
	data.pending = {currentTick, angles, eye, true};
}

void AimlockModule::OnGameFrame(int currentTick)
{
	if (!shots) {
		return;
	}

	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& data = playerData[static_cast<size_t>(index)];
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		if (!IsEligibleHuman(player) || !player->IsAlive()) {
			ClearRuntime(data);
			continue;
		}
		if (!data.pending.valid) {
			continue;
		}

		const AimlockSample sample = data.pending;
		data.pending = {};
		if (sample.serverTick != currentTick || sample.serverTick == data.lastProcessedTick || !shots->FindFrame(sample.serverTick)) {
			if (AimlockDebug()) {
				ConsoleMessage("Aimlock: %s tracking reset because the simulated command and world snapshot did not share one new tick.",
							   player->GetName());
			}
			ClearTrack(data);
			continue;
		}
		data.lastProcessedTick = sample.serverTick;
		Evaluate(player, data, sample);
	}
}

void AimlockModule::Evaluate(PlayerRef* player, AimlockPlayerData& data, const AimlockSample& sample)
{
	const PositionFrame* currentFrame = shots ? shots->FindFrame(sample.serverTick) : nullptr;
	if (!currentFrame || !IsFinite(sample.angles) || !IsFinite(sample.eyePosition)) {
		ClearTrack(data);
		return;
	}
	const Vector aimForward = AimForward(sample.angles);

	if (data.latched) {
		bool stillLocked = false;
		const LagEstimate estimate = EstimateVisualLag(player);
		if (estimate.valid) {
			const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
			const int lastLag = estimate.ticks + lagSearchRadius;
			for (int lagTicks = firstLag; lagTicks <= lastLag; ++lagTicks) {
				const PositionFrame* historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
				if (!historicalFrame) {
					continue;
				}
				QAngle bearing;
				f32 error = 180.0f;
				f32 maximumError = 0.0f;
				if (EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, data.latchedTarget, data.latchedBodyPoint, aimForward,
								   bearing, error, maximumError, nullptr)
					&& error <= maximumError) {
					stillLocked = true;
					break;
				}
			}
		}
		if (stillLocked) {
			data.breakStartTick = -1;
			return;
		}
		if (data.breakStartTick < 0) {
			data.breakStartTick = sample.serverTick;
		}
		if (static_cast<i64>(sample.serverTick) - data.breakStartTick < rearmTicks) {
			return;
		}
		data.latched = false;
		data.latchedTarget = -1;
		data.latchedBodyPoint = -1;
		data.breakStartTick = -1;
		if (AimlockDebug()) {
			ConsoleMessage("Aimlock: %s rearmed after a half-second break.", player->GetName());
		}
	}

	auto startTrack = [&]() {
		const LagEstimate estimate = EstimateVisualLag(player);
		const Candidate candidate = FindCandidate(shots, sample, *currentFrame, player->index, estimate);
		if (!candidate.valid) {
			return;
		}

		data.track = {};
		data.track.targetIndex = candidate.targetIndex;
		data.track.bodyPoint = candidate.bodyPoint;
		data.track.startServerTick = sample.serverTick;
		data.track.lastServerTick = sample.serverTick;
		data.track.samples = 1;
		const int firstLag = (std::max)(0, estimate.ticks - lagSearchRadius);
		const int lastLag = estimate.ticks + lagSearchRadius;
		for (int lagTicks = firstLag;
			 lagTicks <= lastLag && data.track.hypothesisCount < static_cast<int>(data.track.hypotheses.size()); ++lagTicks) {
			const PositionFrame* historicalFrame = shots->FindFrame(sample.serverTick - lagTicks);
			if (!historicalFrame) {
				continue;
			}
			QAngle bearing;
			f32 error = 180.0f;
			f32 maximumError = 0.0f;
			f32 requiredTargetDisplacement = 0.0f;
			if (!EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, candidate.targetIndex, candidate.bodyPoint, aimForward,
								bearing, error, maximumError, &requiredTargetDisplacement)) {
				continue;
			}
			data.track.hypotheses[static_cast<size_t>(data.track.hypothesisCount++)] = {bearing, 0.0f, requiredTargetDisplacement, lagTicks,
																						error <= maximumError, true};
		}
		if (data.track.hypothesisCount == 0) {
			ClearTrack(data);
			return;
		}
		if (AimlockDebug()) {
			ConsoleMessage("Aimlock: %s started tracking target %d body point %d across %d fixed visual-delay candidates (%d-%d ticks) at %.3f "
						   "degrees (RTT %.1f ms, interpolation %.1f ticks).",
						   player->GetName(), candidate.targetIndex, candidate.bodyPoint, data.track.hypothesisCount,
						   data.track.hypotheses.front().lagTicks, data.track.hypotheses[static_cast<size_t>(data.track.hypothesisCount - 1)].lagTicks,
						   static_cast<double>(candidate.error), static_cast<double>(estimate.roundTripMilliseconds),
						   static_cast<double>(estimate.interpolationTicks));
		}
	};

	if (data.track.targetIndex < 0) {
		startTrack();
		return;
	}

	const i64 serverTickDelta = static_cast<i64>(sample.serverTick) - data.track.lastServerTick;
	if (serverTickDelta != 1) {
		if (AimlockDebug()) {
			ConsoleMessage("Aimlock: %s tracking reset because the simulated server ticks were not consecutive (%lld).", player->GetName(),
						   static_cast<long long>(serverTickDelta));
		}
		ClearTrack(data);
		startTrack();
		return;
	}

	++data.track.samples;
	int validHypotheses = 0;
	for (int index = 0; index < data.track.hypothesisCount; ++index) {
		auto& hypothesis = data.track.hypotheses[static_cast<size_t>(index)];
		if (!hypothesis.valid) {
			continue;
		}

		QAngle bearing;
		f32 error = 180.0f;
		f32 maximumError = 0.0f;
		const PositionFrame* historicalFrame = shots->FindFrame(sample.serverTick - hypothesis.lagTicks);
		if (!historicalFrame
			|| !EvaluateTarget(sample, *currentFrame, *historicalFrame, player->index, data.track.targetIndex, data.track.bodyPoint, aimForward,
							   bearing, error, maximumError, nullptr)) {
			hypothesis.valid = false;
			continue;
		}
		const f32 displacement = AngularDistance(hypothesis.startBearing, bearing);
		if (!std::isfinite(displacement)) {
			hypothesis.valid = false;
			continue;
		}

		++validHypotheses;
		hypothesis.onTargetSamples += error <= maximumError;
		hypothesis.maximumTargetDisplacement = (std::max)(hypothesis.maximumTargetDisplacement, displacement);
	}
	if (validHypotheses == 0) {
		if (AimlockDebug()) {
			ConsoleMessage("Aimlock: %s tracking reset because the fixed target history was no longer trustworthy.", player->GetName());
		}
		ClearTrack(data);
		startTrack();
		return;
	}

	data.track.lastServerTick = sample.serverTick;

	const int elapsedTicks = sample.serverTick - data.track.startServerTick;
	if (elapsedTicks > 0 && elapsedTicks < trackingTicks && elapsedTicks % rearmTicks == 0 && AimlockDebug()) {
		const AimlockLagHypothesis* best = BestHypothesis(data.track, false);
		if (best) {
			ConsoleMessage("Aimlock: %s episode: %.1f/2.0 seconds, best fixed delay %d ticks kept %d/%d samples within the target width; target moved "
						   "%.1f/%.1f required degrees.",
						   player->GetName(), static_cast<double>(static_cast<f32>(elapsedTicks) / ENGINE_FIXED_TICK_RATE), best->lagTicks,
						   best->onTargetSamples, data.track.samples, static_cast<double>(best->maximumTargetDisplacement),
						   static_cast<double>(best->requiredTargetDisplacement));
		}
	}
	if (elapsedTicks < trackingTicks) {
		return;
	}

	const AimlockLagHypothesis* passing = BestHypothesis(data.track, true);
	if (!passing) {
		if (AimlockDebug()) {
			const AimlockLagHypothesis* best = BestHypothesis(data.track, false);
			if (best) {
				ConsoleMessage("Aimlock: %s episode ended without evidence: best fixed delay %d ticks kept %d/%d samples within the target width; "
							   "target moved %.1f/%.1f required degrees.",
							   player->GetName(), best->lagTicks, best->onTargetSamples, data.track.samples,
							   static_cast<double>(best->maximumTargetDisplacement), static_cast<double>(best->requiredTargetDisplacement));
			}
		}
		ClearTrack(data);
		return;
	}
	AddIncident(player, data, *passing);
}

void AimlockModule::AddIncident(PlayerRef* player, AimlockPlayerData& data, const AimlockLagHypothesis& hypothesis)
{
	const auto now = Clock::now();
	auto& incidents = evidence[static_cast<size_t>(player->index)];
	while (!incidents.empty() && now - incidents.front() >= aimlockEvidenceWindow) {
		incidents.pop_front();
	}
	incidents.push_back(now);
	if (AimlockDebug()) {
		ConsoleMessage("Aimlock: %s added evidence %d/%d after fixed delay %d ticks kept %d/%d precise samples and %.1f/%.1f required degrees of "
					   "target movement.",
					   player->GetName(), static_cast<int>(incidents.size()), aimlockDetectionThreshold, hypothesis.lagTicks,
					   hypothesis.onTargetSamples, data.track.samples, static_cast<double>(hypothesis.maximumTargetDisplacement),
					   static_cast<double>(hypothesis.requiredTargetDisplacement));
	}
	const bool detected = incidents.size() >= aimlockDetectionThreshold;
	if (detected) {
		if (announce) {
			announce("AIMLOCK", player,
					 localization::Format("evidence.aimlock",
										  "CS2AC found {incidents} separate periods of near-perfect enemy tracking. During the latest two-second "
										  "period, the aim stayed inside the enemy's body width for {precise} of {samples} checks while the enemy moved "
										  "far enough across the player's view. The enemy moved {movement} degrees across the view; {required} "
										  "degrees was required at that distance.",
										  {{"incidents", FormatString("%zu", incidents.size())},
										   {"precise", FormatString("%d", hypothesis.onTargetSamples)},
										   {"samples", FormatString("%d", data.track.samples)},
										   {"movement", FormatString("%.1f", static_cast<double>(hypothesis.maximumTargetDisplacement))},
										   {"required", FormatString("%.1f", static_cast<double>(hypothesis.requiredTargetDisplacement))}}));
		}
		incidents.clear();
		data.latched = true;
		data.latchedTarget = data.track.targetIndex;
		data.latchedBodyPoint = data.track.bodyPoint;
		data.breakStartTick = -1;
	}

	ClearTrack(data);
}

void AimlockModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
		evidence[static_cast<size_t>(player->index)].clear();
	}
}

} // namespace detection
} // namespace cs2ac
