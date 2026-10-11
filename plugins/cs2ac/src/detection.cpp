// cs2ac — the detection pipeline: ShotCorrelator, NetworkSafetyMonitor and DetectionSystem.
//
// Ported almost verbatim from the original's detection/detection_system.cpp and
// network_safety.cpp: the correlation windows (1-tick shot match, 2-tick lifetime), the position
// frame ring (128 frames), the five-second network window with its 100 ms sampling, the
// per-detector enable mask and the exact dispatch order are unchanged. Only the data sources move
// from the engine/SDK structs to engine:: and the parsed UsercmdView/GameEventData.

#include "cs2ac.hpp"

namespace cs2ac {
namespace detection {

namespace {
constexpr size_t shotCommandLimit = 32;
constexpr size_t pendingShotLimit = 8;
constexpr size_t positionHistoryLimit = 128;
constexpr int shotMatchTicks = 1;
constexpr int shotLifetimeTicks = 2;
static_assert(shotLifetimeTicks > shotMatchTicks, "Shots must survive long enough for every accepted event.");

constexpr f32 maximumPingMilliseconds = 150.0f;
constexpr f32 maximumJitterMilliseconds = 25.0f;
constexpr f32 maximumLoss = 0.02f;
constexpr f32 maximumChoke = 0.02f;
constexpr auto networkWindow = std::chrono::seconds(5);
constexpr auto networkSampleInterval = std::chrono::milliseconds(100);
} // namespace

// detection_system.cpp TeammatesAreEnemies() — the engine read lives here (cached once per frame by
// engine::OnFrameRefresh), the cached flag in core.cpp (so the tests can exercise AreOpponents).
bool ReadTeammatesAreEnemies()
{
	return engine::TeammatesAreEnemiesCvar();
}

bool IsEligibleHuman(PlayerRef* player)
{
	return player && player->index >= 1 && player->index <= MAXPLAYERS && !player->IsFakeClient() && !player->IsCSTV();
}

// ---------------------------------------------------------------------------------------------
// ShotCorrelator (detection_system.cpp)
// ---------------------------------------------------------------------------------------------

void ShotCorrelator::AdvanceGeneration(ShotPlayerData& data)
{
	data.commands.clear();
	data.shots.clear();
	data.lastViewCommandNumber = -1;
	data.lastViewBaseAngles = {};
	data.lastViewValid = false;
	if (++data.generation == 0) {
		data.generation = 1;
	}
}

void ShotCorrelator::Reset()
{
	for (auto& data : playerData) {
		AdvanceGeneration(data);
	}
	positionFrames.clear();
	nextShotId = 1;
}

void ShotCorrelator::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		AdvanceGeneration(playerData[static_cast<size_t>(player->index)]);
		for (auto& frame : positionFrames) {
			frame.players[static_cast<size_t>(player->index)] = {};
		}
	}
}

void ShotCorrelator::OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands)
{
	if (!IsEligibleHuman(player) || commands.empty()) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	for (const UsercmdView& command : commands) {
		if (!command.hasBase || !command.hasViewAngles) {
			continue;
		}

		const QAngle baseAngles = command.viewAngles;
		if (!IsFinite(baseAngles)) {
			continue;
		}

		const int attackIndex = command.attack1StartHistoryIndex;
		const bool validAttackIndex = attackIndex >= 0 && attackIndex < static_cast<int>(command.inputHistory.size());
		const bool hasAttackAngles = validAttackIndex && command.inputHistory[static_cast<size_t>(attackIndex)].hasViewAngles;
		QAngle attackAngles;
		if (hasAttackAngles) {
			attackAngles = command.inputHistory[static_cast<size_t>(attackIndex)].viewAngles;
		}

		if (hasAttackAngles && IsFinite(attackAngles)
			&& !std::any_of(data.commands.rbegin(), data.commands.rend(),
							[&](const ShotCommand& stored) { return stored.commandNumber == command.commandNumber; })) {
			ShotCommand captured {};
			captured.commandNumber = command.commandNumber;
			captured.clientTick = command.clientTick;
			captured.serverTick = -1;
			captured.baseAngles = baseAngles;
			captured.attackAngles = attackAngles;
			captured.previousBaseValid = data.lastViewValid && command.commandNumber == data.lastViewCommandNumber + 1;
			if (captured.previousBaseValid) {
				captured.previousBaseAngles = data.lastViewBaseAngles;
			}
			for (const SubtickMove& move : command.subtickMoves) {
				if (move.hasPitchDelta) {
					if (!std::isfinite(move.pitchDelta)) {
						captured.subtickAnglesValid = false;
					} else {
						captured.subtickPitchTravel += std::abs(move.pitchDelta);
					}
				}
				if (move.hasYawDelta) {
					if (!std::isfinite(move.yawDelta)) {
						captured.subtickAnglesValid = false;
					} else {
						captured.subtickYawTravel += std::abs(move.yawDelta);
					}
				}
			}
			if (!std::isfinite(captured.subtickPitchTravel) || !std::isfinite(captured.subtickYawTravel)) {
				captured.subtickAnglesValid = false;
				captured.subtickPitchTravel = 0.0f;
				captured.subtickYawTravel = 0.0f;
			}
			data.commands.push_back(captured);
			while (data.commands.size() > shotCommandLimit) {
				data.commands.pop_front();
			}
		}

		if (!data.lastViewValid || command.commandNumber > data.lastViewCommandNumber) {
			data.lastViewCommandNumber = command.commandNumber;
			data.lastViewBaseAngles = baseAngles;
			data.lastViewValid = true;
		}
	}
}

void ShotCorrelator::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	if (!IsEligibleHuman(player) || !command.hasBase) {
		return;
	}
	auto& data = playerData[static_cast<size_t>(player->index)];
	auto found = std::find_if(data.commands.rbegin(), data.commands.rend(),
							  [&](const ShotCommand& stored) { return stored.commandNumber == command.commandNumber; });
	if (found == data.commands.rend()) {
		return;
	}

	Vector eye;
	if (!engine::ReadEyeOrigin(player->index, eye) || !IsFinite(eye)) {
		return;
	}
	const bool grounded = engine::PlayerOnGroundLastTick(player->index)
						  || (engine::PlayerOnGround(player->index) && engine::PlayerGroundEntity(player->index) >= 0);
	found->serverTick = currentTick;
	found->eyePosition = eye;
	found->airborne = !grounded && engine::PlayerMoveType(player->index) == MOVETYPE_WALK
					  && engine::PlayerActualMoveType(player->index) == MOVETYPE_WALK;
	found->scoped = engine::PlayerScoped(player->index);
	found->simulated = true;
}

void ShotCorrelator::CaptureFrame(int currentTick)
{
	// The frame is filled in place: a default PositionFrame has every member initialized (see
	// core.hpp), so clearing the slot we are about to overwrite is the same frame the previous
	// build copied in from the stack, without the 2.6 KB copy per frame.
	PositionFrame* frame = nullptr;
	if (!positionFrames.empty() && positionFrames.back().serverTick == currentTick) {
		frame = &positionFrames.back();
	} else {
		positionFrames.emplace_back();
		while (positionFrames.size() > positionHistoryLimit) {
			positionFrames.pop_front();
		}
		frame = &positionFrames.back();
	}
	*frame = {};
	frame->serverTick = currentTick;

	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(index) : nullptr;
		// The same conjunction as before, only with "connected" asked first: both answers come from
		// the frame's snapshot (see engine::FrameSnapshotScope), and the decisive one for a slot
		// with no client at all is the first - so the rest of the frame reuses it.
		if (!player || !player->IsConnected() || !player->IsInGame()) {
			continue;
		}
		// The original also skipped a player without a pawn here;
		// a pawn-less client has no position, and the engine accessors would answer their neutral
		// (0,0,0), which the detectors that look a frame up would otherwise read as a real sample.
		if (!engine::PlayerHasPawn(index)) {
			continue;
		}
		const Vector origin = engine::PlayerOrigin(index);
		Vector eye;
		if (!engine::ReadEyeOrigin(index, eye) || !IsFinite(origin) || !IsFinite(eye)) {
			continue;
		}
		if (captureOverride) {
			TrackedPosition injected;
			captureOverride(index, injected);
			if (injected.valid) {
				frame->players[static_cast<size_t>(index)] = injected;
				continue;
			}
		}
		frame->players[static_cast<size_t>(index)] = {origin, eye, engine::PlayerTeam(index), true, engine::PlayerAlive(index),
													  engine::PlayerJustTeleported(index, 2.0f)};
	}
}

void ShotCorrelator::Prune(int currentTick)
{
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& data = playerData[static_cast<size_t>(index)];
		// A slot the correlator has nothing stored for has nothing to prune (erasing an empty range
		// removes nothing), and that is most of the 64 slots on a normal server.
		if (data.commands.empty() && data.shots.empty()) {
			continue;
		}
		data.commands.erase(std::remove_if(data.commands.begin(), data.commands.end(),
										   [&](const ShotCommand& command) {
											   const i64 delta = static_cast<i64>(currentTick) - command.serverTick;
											   return command.simulated && delta > shotLifetimeTicks;
										   }),
							data.commands.end());
		data.shots.erase(std::remove_if(data.shots.begin(), data.shots.end(),
										[&](const ShotRecord& shot) {
											const i64 delta = static_cast<i64>(currentTick) - shot.fireTick;
											return delta > shotLifetimeTicks;
										}),
						 data.shots.end());
	}
}

ShotRecord* ShotCorrelator::OnWeaponFire(const GameEventData& event, PlayerRef* player, int currentTick)
{
	if (!IsEligibleHuman(player)) {
		return nullptr;
	}
	const std::string_view weapon = NormalizeWeapon(event.weapon);
	if (!IsBallisticWeapon(weapon)) {
		return nullptr;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	ShotCommand* match = nullptr;
	int matches = 0;
	for (auto& command : data.commands) {
		const i64 delta = static_cast<i64>(currentTick) - command.serverTick;
		if (!command.simulated || command.consumed || delta < 0 || delta > shotMatchTicks) {
			continue;
		}
		match = &command;
		++matches;
	}
	if (matches != 1) {
		for (auto& command : data.commands) {
			const i64 delta = static_cast<i64>(currentTick) - command.serverTick;
			if (command.simulated && !command.consumed && delta >= 0 && delta <= shotMatchTicks) {
				command.consumed = true;
			}
		}
		return nullptr;
	}

	match->consumed = true;
	ShotRecord shot;
	shot.id = nextShotId++;
	if (nextShotId == 0) {
		nextShotId = 1;
	}
	shot.generation = data.generation;
	shot.playerIndex = player->index;
	shot.commandNumber = match->commandNumber;
	shot.clientTick = match->clientTick;
	shot.serverTick = match->serverTick;
	shot.fireTick = currentTick;
	shot.angles = match->attackAngles;
	shot.baseAngles = match->baseAngles;
	shot.eyePosition = match->eyePosition;
	shot.weapon.assign(weapon);
	shot.fireTime = Clock::now();
	shot.silentSubtickPitchTravel = match->subtickPitchTravel;
	shot.silentSubtickYawTravel = match->subtickYawTravel;
	shot.silentPreviousBaseValid = match->previousBaseValid;
	if (shot.silentPreviousBaseValid) {
		shot.silentPreviousBaseAngles = match->previousBaseAngles;
	}
	shot.silentSubtickAnglesValid = match->subtickAnglesValid;
	shot.airborne = match->airborne;
	shot.scoped = match->scoped;
	data.shots.push_back(std::move(shot));
	while (data.shots.size() > pendingShotLimit) {
		data.shots.pop_front();
	}
	return &data.shots.back();
}

ShotRecord* ShotCorrelator::OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField,
										  std::string& missing)
{
	if (missingField) {
		return nullptr;
	}
	const bool validInaccuracy = std::isfinite(inaccuracy) && inaccuracy >= 0.0f;
	const bool validSpread = std::isfinite(spread) && spread >= 0.0f;
	if (!validInaccuracy || !validSpread) {
		return nullptr;
	}
	auto* player = g_pPlayerManager ? g_pPlayerManager->ToPlayer(playerIndex) : nullptr;
	if (!IsEligibleHuman(player)) {
		return nullptr;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	ShotRecord* match = nullptr;
	int matches = 0;
	for (auto& shot : data.shots) {
		const i64 delta = static_cast<i64>(currentTick) - shot.fireTick;
		if (shot.generation != data.generation || shot.silentFireSeen || delta < 0 || delta > shotMatchTicks) {
			continue;
		}
		match = &shot;
		++matches;
	}
	if (matches != 1) {
		return nullptr;
	}

	match->silentInaccuracy = inaccuracy;
	match->silentSpread = spread;
	match->silentWeaponId = weaponId;
	match->silentFireSeen = true;
	(void)missing;
	return match;
}

ShotRecord* ShotCorrelator::MatchEvent(PlayerRef* player, std::string_view weapon, int currentTick)
{
	if (!IsEligibleHuman(player)) {
		return nullptr;
	}
	weapon = NormalizeWeapon(weapon);
	auto& data = playerData[static_cast<size_t>(player->index)];
	ShotRecord* match = nullptr;
	int matches = 0;
	for (auto& shot : data.shots) {
		const i64 delta = static_cast<i64>(currentTick) - shot.fireTick;
		if (shot.generation != data.generation || delta < 0 || delta > shotMatchTicks
			|| (!weapon.empty() && NormalizeWeapon(shot.weapon) != weapon)) {
			continue;
		}
		match = &shot;
		++matches;
	}
	return matches == 1 ? match : nullptr;
}

ShotRecord* ShotCorrelator::OnPlayerHurt(const GameEventData& event, PlayerRef* victim, int currentTick)
{
	if (!g_pPlayerManager) {
		return nullptr;
	}
	auto* attacker = g_pPlayerManager->ToPlayer(event.attacker);
	if (!victim) {
		victim = g_pPlayerManager->ToPlayer(event.userid);
	}
	if (!IsEligibleHuman(attacker) || !victim || attacker == victim) {
		return nullptr;
	}
	ShotRecord* shot = MatchEvent(attacker, {}, currentTick);
	if (!shot || shot->hurtSeen) {
		return nullptr;
	}
	shot->hurtSeen = true;
	shot->silentHitSeen = true;
	shot->victimIndex = victim->index;
	shot->headshot = event.hitgroup == HITGROUP_HEAD;
	return shot;
}

ShotRecord* ShotCorrelator::OnPlayerDeath(const GameEventData& event, PlayerRef* victim, int currentTick)
{
	if (!g_pPlayerManager) {
		return nullptr;
	}
	auto* attacker = g_pPlayerManager->ToPlayer(event.attacker);
	if (!victim) {
		victim = g_pPlayerManager->ToPlayer(event.userid);
	}
	if (!IsEligibleHuman(attacker) || !victim || attacker == victim) {
		return nullptr;
	}
	ShotRecord* shot = MatchEvent(attacker, event.weapon, currentTick);
	if (!shot || shot->deathSeen) {
		return nullptr;
	}
	shot->deathSeen = true;
	shot->victimIndex = victim->index;
	shot->wallbang = event.penetrated;
	shot->throughSmoke = event.thrusmoke;
	return shot;
}

const PositionFrame* ShotCorrelator::FindFrame(int serverTick) const
{
	for (auto frame = positionFrames.rbegin(); frame != positionFrames.rend(); ++frame) {
		if (frame->serverTick == serverTick) {
			return &*frame;
		}
		if (frame->serverTick < serverTick) {
			break;
		}
	}
	return nullptr;
}

const TrackedPosition* ShotCorrelator::FindPosition(int serverTick, int playerIndex) const
{
	const PositionFrame* frame = FindFrame(serverTick);
	if (!frame || playerIndex < 1 || playerIndex > MAXPLAYERS || !frame->players[static_cast<size_t>(playerIndex)].valid) {
		return nullptr;
	}
	return &frame->players[static_cast<size_t>(playerIndex)];
}

std::deque<ShotRecord>& ShotCorrelator::GetShots(int playerIndex)
{
	if (playerIndex < 1 || playerIndex > MAXPLAYERS) {
		emptyShots.clear();
		return emptyShots;
	}
	return playerData[static_cast<size_t>(playerIndex)].shots;
}

// ---------------------------------------------------------------------------------------------
// NetworkSafetyMonitor (network_safety.cpp)
// ---------------------------------------------------------------------------------------------

void NetworkSafetyMonitor::Reset()
{
	playerData = {};
}

void NetworkSafetyMonitor::Prune(PlayerData& data, Clock::time_point now)
{
	const auto cutoff = now - networkWindow;
	while (!data.samples.empty() && data.samples.front().time < cutoff) {
		data.samples.pop_front();
	}
	while (!data.commandGaps.empty() && data.commandGaps.front() < cutoff) {
		data.commandGaps.pop_front();
	}
}

void NetworkSafetyMonitor::OnGameFrame()
{
	if (!g_pPlayerManager) {
		return;
	}

	const auto now = Clock::now();
	for (u32 index = 1; index <= MAXPLAYERS; ++index) {
		auto* player = g_pPlayerManager->ToPlayer(static_cast<i32>(index));
		if (!player || !player->IsConnected() || !player->IsInGame() || !IsEligibleHuman(player)) {
			continue;
		}

		auto& data = playerData[static_cast<size_t>(index)];
		if (data.nextSample != Clock::time_point {} && now < data.nextSample) {
			continue;
		}
		data.nextSample = now + networkSampleInterval;

		Sample sample;
		sample.time = now;
		const engine::NetStats stats = engine::ClientNetStats(static_cast<int>(index));
		sample.pingMilliseconds = stats.pingSeconds * 1000.0f;
		sample.incomingLoss = stats.incomingLoss;
		sample.outgoingLoss = stats.outgoingLoss;
		sample.incomingChoke = stats.incomingChoke;
		sample.outgoingChoke = stats.outgoingChoke;
		sample.lossChokeAvailable = stats.lossChokeAvailable;
		sample.valid = stats.pingAvailable && std::isfinite(sample.pingMilliseconds) && sample.pingMilliseconds >= 0.0f;
		if (sample.lossChokeAvailable) {
			sample.valid = sample.valid && std::isfinite(sample.incomingLoss) && sample.incomingLoss >= 0.0f && sample.incomingLoss <= 1.0f
						   && std::isfinite(sample.outgoingLoss) && sample.outgoingLoss >= 0.0f && sample.outgoingLoss <= 1.0f
						   && std::isfinite(sample.incomingChoke) && sample.incomingChoke >= 0.0f && sample.incomingChoke <= 1.0f
						   && std::isfinite(sample.outgoingChoke) && sample.outgoingChoke >= 0.0f && sample.outgoingChoke <= 1.0f;
		}
		data.samples.push_back(sample);
		while (data.samples.size() > 64) {
			data.samples.pop_front();
		}
		Prune(data, now);
	}
}

void NetworkSafetyMonitor::OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands)
{
	if (!IsEligibleHuman(player) || commands.empty()) {
		return;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	const auto now = Clock::now();
	Prune(data, now);
	bool discontinuity = false;
	for (const UsercmdView& command : commands) {
		if (command.commandNumber <= data.lastCommandNumber) {
			discontinuity = discontinuity || command.commandNumber < data.lastCommandNumber;
			continue;
		}

		const int clientTick = command.hasBase && command.hasClientTick ? command.clientTick : -1;
		discontinuity = discontinuity || (data.lastCommandNumber >= 0 && command.commandNumber > data.lastCommandNumber + 1)
						|| (clientTick >= 0 && data.lastClientTick >= 0 && (clientTick > data.lastClientTick + 1 || clientTick < data.lastClientTick));
		data.lastCommandNumber = command.commandNumber;
		if (clientTick >= 0) {
			data.lastClientTick = clientTick;
		}
	}
	if (discontinuity) {
		data.commandGaps.push_back(now);
		while (data.commandGaps.size() > 512) {
			data.commandGaps.pop_front();
		}
	}
}

NetworkSafetyEvidence NetworkSafetyMonitor::Evaluate(PlayerRef* player)
{
	NetworkSafetyEvidence evidence;
	if (!player || player->index < 1 || player->index > MAXPLAYERS) {
		evidence.unavailableSamples = 1;
		evidence.vetoed = true;
		return evidence;
	}

	auto& data = playerData[static_cast<size_t>(player->index)];
	Prune(data, Clock::now());
	evidence.commandGaps = static_cast<int>(data.commandGaps.size());

	f32 previousPing = 0.0f;
	f32 totalVariation = 0.0f;
	int validSamples = 0;
	int variationSamples = 0;
	bool readAnything = false;
	for (const auto& sample : data.samples) {
		if (!sample.valid) {
			++evidence.unavailableSamples;
			continue;
		}
		readAnything = true;

		evidence.pingMilliseconds = (std::max)(evidence.pingMilliseconds, sample.pingMilliseconds);
		if (sample.lossChokeAvailable) {
			evidence.incomingLoss = (std::max)(evidence.incomingLoss, sample.incomingLoss);
			evidence.outgoingLoss = (std::max)(evidence.outgoingLoss, sample.outgoingLoss);
			evidence.incomingChoke = (std::max)(evidence.incomingChoke, sample.incomingChoke);
			evidence.outgoingChoke = (std::max)(evidence.outgoingChoke, sample.outgoingChoke);
		}
		if (validSamples > 0) {
			totalVariation += std::fabs(sample.pingMilliseconds - previousPing);
			++variationSamples;
		}
		previousPing = sample.pingMilliseconds;
		++validSamples;
	}
	if (validSamples < 2 && evidence.unavailableSamples == 0) {
		evidence.unavailableSamples = 1;
	}
	if (variationSamples > 0) {
		evidence.jitterMilliseconds = totalVariation / variationSamples;
	}

	// Deviation from the original, documented in PORT_PLAN.md §6: the engine gave the original
	// loss/choke readings too; s2sdk only exposes latency. When the loss/choke source is absent
	// those gates are treated as unknown (they do not veto) so Doubletap/Triggerbot/AntiAim-mismatch
	// keep working, while ping, jitter and the sample-availability gates still apply. The ping and
	// jitter gates are the two that actually change with a bad connection.
	const bool lossChokeMissing = !lossChokeUnavailableNoted;
	(void)lossChokeMissing;
	evidence.vetoed = ShouldVeto(evidence.pingMilliseconds, evidence.jitterMilliseconds, evidence.incomingLoss, evidence.outgoingLoss,
								 evidence.incomingChoke, evidence.outgoingChoke, evidence.unavailableSamples);
	(void)readAnything;
	return evidence;
}

void NetworkSafetyMonitor::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<size_t>(player->index)] = {};
	}
}

// ---------------------------------------------------------------------------------------------
// DetectionSystem (detection_system.cpp)
// ---------------------------------------------------------------------------------------------

void DetectionSystem::Load(AnnounceCallback announce, AnnounceCallback announceNetworkVeto)
{
	networkSafety.Reset();
	shots.Reset();
	doubletap.Load(announce, announceNetworkVeto, &networkSafety);
	triggerbot.Load(announce, &networkSafety);
	silentAim.Load(announce, &shots);
	aimbot.Load(announce, &shots);
	aimlock.Load(announce, &shots);
	dllInjection.Load(announce);
	antiAim.Load(announce, announceNetworkVeto, &networkSafety);
	irregularBehavior.Load(announce);
	inhumanAccuracy.Load(announce, &shots);
	nameChanger.Load(announce);
	settingsMask = settings::GetDetectionMask();
	settingsRevision = settings::GetRevision();
	teammatesAreEnemies = ReadTeammatesAreEnemies();
	SetTeammatesAreEnemies(teammatesAreEnemies);
}

void DetectionSystem::Unload()
{
	doubletap.Unload();
	triggerbot.Unload();
	silentAim.Unload();
	aimbot.Unload();
	aimlock.Unload();
	dllInjection.Unload();
	antiAim.Unload();
	irregularBehavior.Unload();
	inhumanAccuracy.Unload();
	nameChanger.Unload();
	networkSafety.Reset();
	shots.Reset();
	settingsMask = 0;
	settingsRevision = 0;
	teammatesAreEnemies = false;
	SetTeammatesAreEnemies(false);
}

void DetectionSystem::Reset()
{
	doubletap.Reset();
	triggerbot.Reset();
	silentAim.Reset();
	aimbot.Reset();
	aimlock.Reset();
	dllInjection.Reset();
	antiAim.Reset();
	irregularBehavior.Reset();
	inhumanAccuracy.Reset();
	nameChanger.Reset();
	networkSafety.Reset();
	shots.Reset();
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		if (movement[static_cast<size_t>(index)]) {
			movement[static_cast<size_t>(index)]->Reset();
		}
		sampler[static_cast<size_t>(index)] = {};
	}
}

void DetectionSystem::RefreshSettings()
{
	const u64 currentMask = settings::GetDetectionMask();
	const u64 currentRevision = settings::GetRevision();
	const bool currentTeammatesAreEnemies = ReadTeammatesAreEnemies();
	if (currentMask != settingsMask || currentRevision != settingsRevision || currentTeammatesAreEnemies != teammatesAreEnemies) {
		Reset();
		settingsMask = currentMask;
		settingsRevision = currentRevision;
		teammatesAreEnemies = currentTeammatesAreEnemies;
		SetTeammatesAreEnemies(currentTeammatesAreEnemies);
	}
}

void DetectionSystem::OnProcessUsercmds(PlayerRef* player, const std::vector<UsercmdView>& commands)
{
	RefreshSettings();
	if (!settings::IsPluginEnabled()) {
		return;
	}
	// Every consumer below opens with the same gate: an empty batch or a subject that is not an
	// eligible human (not a bot, not the SourceTV bot) makes each of them return without touching
	// its state - the movement block additionally requires IsAlive/a non-bot/a non-spectator.
	// Testing it once here removes the repeated IsFakeClient/IsCSTV reads from the per-usercmd path.
	if (commands.empty() || !IsEligibleHuman(player)) {
		return;
	}
	shots.OnProcessUsercmds(player, commands);
	if (settings::IsDetectionEnabled(DetectionType::Doubletap) || settings::IsDetectionEnabled(DetectionType::AntiAim)
		|| settings::IsDetectionEnabled(DetectionType::Triggerbot)) {
		networkSafety.OnProcessUsercmds(player, commands);
	}
	if (settings::IsDetectionEnabled(DetectionType::Aimbot)) {
		aimbot.OnProcessUsercmds(player, commands);
	}
	if (settings::IsDetectionEnabled(DetectionType::AntiAim)) {
		antiAim.OnProcessUsercmds(player, commands);
	}

	// Movement detectors (the original's MovementDetectionService::OnSetupMove reads the commands
	// from the same ProcessUsercmds hook). ShouldRunDetections is the gate that guarded all of them
	// there: while sv_cheats is on (and testing is not allowed) nothing may be recorded.
	auto* movement = MovementFor(player ? player->index : 0);
	if (movement && player && player->index >= 1 && player->index <= MAXPLAYERS && player->IsAlive() && !player->IsFakeClient()
		&& !player->IsCSTV() && movement->ShouldRunDetections()) {
		for (const UsercmdView& command : commands) {
			movement->SetCurrentCmdNum(command.commandNumber);
			movement->CheckSubtickAbuse(player, command);
			if (settings::IsDetectionEnabled(DetectionType::Nulls)) {
				movement->CreateInputEvents(player, command);
			}
			movement->ParseCommandForJump(player, command);
			if (settings::IsDetectionEnabled(DetectionType::Autostrafe)) {
				movement->DetectOptimization(player, command, engine::FrameTime());
			}
		}
	}
}

void DetectionSystem::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	RefreshSettings();
	if (!settings::IsPluginEnabled()) {
		return;
	}
	// Same gate as OnProcessUsercmds: all five consumers below either ignore a non-eligible subject
	// outright (ShotCorrelator, Triggerbot, Aimbot, Aimlock) or only reset state that such a
	// subject can never have accumulated (AntiAim).
	if (!IsEligibleHuman(player)) {
		return;
	}
	shots.OnSetupMove(player, command, currentTick);
	if (settings::IsDetectionEnabled(DetectionType::Triggerbot)) {
		triggerbot.OnSetupMove(player, command, currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::Aimbot)) {
		aimbot.OnSetupMove(player, command, currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::Aimlock)) {
		aimlock.OnSetupMove(player, command, currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::AntiAim)) {
		antiAim.OnSetupMove(player, command, currentTick);
	}
}

void DetectionSystem::OnGameFrame(int currentTick)
{
	RefreshSettings();
	if (!settings::IsPluginEnabled()) {
		return;
	}
	shots.CaptureFrame(currentTick);
	if (settings::IsDetectionEnabled(DetectionType::Doubletap) || settings::IsDetectionEnabled(DetectionType::AntiAim)
		|| settings::IsDetectionEnabled(DetectionType::Triggerbot)) {
		networkSafety.OnGameFrame();
	}
	if (settings::IsDetectionEnabled(DetectionType::Aimbot)) {
		aimbot.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::Aimlock)) {
		aimlock.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::AntiAim)) {
		antiAim.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::SilentAim)) {
		silentAim.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior)) {
		irregularBehavior.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::InhumanAccuracy)) {
		inhumanAccuracy.OnGameFrame(currentTick);
	}
	if (settings::IsDetectionEnabled(DetectionType::DllInjection)) {
		dllInjection.OnGameFrame();
	}
	shots.Prune(currentTick);
}

void DetectionSystem::OnGameEvent(const GameEventData& event, PlayerRef* player, int currentTick)
{
	RefreshSettings();
	if (!settings::IsPluginEnabled() || event.name.empty()) {
		return;
	}
	// The hook layer passes the event's own user index where it has one (the original resolved the
	// player from IGameEvent::GetPlayerController); the modules then see the same subject.
	if (!player && g_pPlayerManager && event.userid >= 1 && event.userid <= MAXPLAYERS) {
		player = g_pPlayerManager->ToPlayer(event.userid);
	}
	if (settings::IsDetectionEnabled(DetectionType::Triggerbot)) {
		triggerbot.OnGameEvent(event);
	}

	if (settings::IsDetectionEnabled(DetectionType::AntiAim)) {
		antiAim.OnGameEvent(event, player);
	}
	if (event.name == "weapon_fire") {
		if (settings::IsDetectionEnabled(DetectionType::Doubletap)) {
			doubletap.OnWeaponFire(event, player, currentTick);
		}
		if (ShotRecord* shot = shots.OnWeaponFire(event, player, currentTick)) {
			if (settings::IsDetectionEnabled(DetectionType::AntiAim)) {
				antiAim.OnWeaponFire(player, *shot);
			}
			if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior)) {
				irregularBehavior.OnWeaponFire(player, *shot);
			}
		}
	} else if (event.name == "player_hurt") {
		auto* victim = player;
		if (ShotRecord* shot = shots.OnPlayerHurt(event, victim, currentTick)) {
			auto* attacker = g_pPlayerManager->ToPlayer(shot->playerIndex);
			auto* hurtVictim = g_pPlayerManager->ToPlayer(shot->victimIndex);
			if (settings::IsDetectionEnabled(DetectionType::Triggerbot)) {
				triggerbot.OnPlayerHurt(event, attacker, hurtVictim, *shot);
			}
			if (settings::IsDetectionEnabled(DetectionType::Aimbot)) {
				aimbot.OnPlayerHurt(attacker, hurtVictim, *shot);
			}
		}
	} else if (event.name == "player_death") {
		auto* victim = player;
		if (ShotRecord* shot = shots.OnPlayerDeath(event, victim, currentTick)) {
			auto* attacker = g_pPlayerManager->ToPlayer(shot->playerIndex);
			auto* deadVictim = g_pPlayerManager->ToPlayer(shot->victimIndex);
			if (settings::IsDetectionEnabled(DetectionType::IrregularBehavior)) {
				irregularBehavior.OnPlayerDeath(event, attacker, deadVictim, *shot);
			}
		}
	}
}

void DetectionSystem::OnFireBullets(int playerIndex, int currentTick, f32 inaccuracy, f32 spread, u32 weaponId, bool missingField,
									const std::string& missing)
{
	RefreshSettings();
	if (!settings::IsPluginEnabled() || !settings::IsDetectionEnabled(DetectionType::SilentAim)) {
		return;
	}
	std::string missingCopy = missing;
	if (ShotRecord* shot = shots.OnFireBullets(playerIndex, currentTick, inaccuracy, spread, weaponId, missingField, missingCopy)) {
		auto* player = g_pPlayerManager->ToPlayer(shot->playerIndex);
		silentAim.OnShotUpdated(player, *shot);
	}
}

void DetectionSystem::OnClientReady(PlayerRef* player)
{
	RefreshSettings();
	if (settings::IsDetectionEnabled(DetectionType::NameChanger)) {
		nameChanger.OnClientReady(player);
	}
}

void DetectionSystem::OnClientSettingsChanged(PlayerRef* player)
{
	RefreshSettings();
	if (settings::IsDetectionEnabled(DetectionType::NameChanger)) {
		nameChanger.OnClientSettingsChanged(player);
	}
}

void DetectionSystem::OnClientDisconnect(PlayerRef* player)
{
	doubletap.OnClientDisconnect(player);
	triggerbot.OnClientDisconnect(player);
	silentAim.OnClientDisconnect(player);
	aimbot.OnClientDisconnect(player);
	aimlock.OnClientDisconnect(player);
	dllInjection.OnClientDisconnect(player);
	antiAim.OnClientDisconnect(player);
	irregularBehavior.OnClientDisconnect(player);
	inhumanAccuracy.OnClientDisconnect(player);
	nameChanger.OnClientDisconnect(player);
	networkSafety.OnClientDisconnect(player);
	shots.OnClientDisconnect(player);
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		if (movement[static_cast<size_t>(player->index)]) {
			movement[static_cast<size_t>(player->index)]->Reset();
		}
		sampler[static_cast<size_t>(player->index)] = {};
	}
}

MovementDetectionService* DetectionSystem::MovementFor(int playerIndex)
{
	if (playerIndex < 1 || playerIndex > MAXPLAYERS) {
		return nullptr;
	}
	auto& service = movement[static_cast<size_t>(playerIndex)];
	if (!service) {
		service = std::make_unique<MovementDetectionService>();
		service->reportInfraction = [](const char* detection, PlayerRef* player, const localization::Text& evidence, bool kickOnly) {
			g_plugin.HandleDetection(detection, player, evidence, kickOnly);
		};
		service->Reset();
	}
	return service.get();
}

void DetectionSystem::MovementOnClientReady(PlayerRef* player)
{
	if (auto* service = MovementFor(player ? player->index : 0)) {
		service->InitCvarMonitor(player);
	}
}

void DetectionSystem::MovementCvars(PlayerRef* player)
{
	if (auto* service = MovementFor(player ? player->index : 0)) {
		service->DispatchCvarQuery(player);
	}
}

// The port's per-frame movement sampler. The original fed the same signals from its movement hooks:
//   CategorizePosition transition -> landings (OnGroundFrame -> CreateLandEvent)
//   AirMove                       -> air time (OnAirMoveFrame)
//   ProcessMovement[Post]         -> the "did the player move in the air this frame" bookkeeping
//   OnJumpLegacy/Modern[Post]     -> the actual jump (OnServerJump, frame-perfect flag)
//   PhysicsSimulatePost           -> CheckNulls + CheckSuspiciousSubtickCommands + CheckLandingEvents
void DetectionSystem::MovementOnGameFrame(PlayerRef* player, f64 timeInServer)
{
	if (!settings::IsPluginEnabled() || !player || player->index < 1 || player->index > MAXPLAYERS) {
		return;
	}
	auto* service = MovementFor(player->index);
	if (!service) {
		return;
	}
	auto& state = sampler[static_cast<size_t>(player->index)];

	const bool alive = player->IsAlive() && !player->IsFakeClient() && !player->IsCSTV();
	if (!alive) {
		if (state.valid) {
			service->ResetTransientDetectionState();
			state = {};
		}
		return;
	}

	const bool onGround = engine::PlayerOnGround(player->index);
	const int moveType = engine::PlayerMoveType(player->index);
	const f32 frametime = engine::FrameTime();

	// ProcessMovement/OnChangeMoveType equivalent: a non-walking movement type invalidates the
	// landing gates for the next four ticks.
	service->OnChangeMoveTypeFrame(player, state.moveType, moveType, engine::CurTime());

	if (!state.valid) {
		state.onGround = onGround;
		state.moveType = moveType;
		state.valid = true;
	} else {
		// CategorizePosition equivalent.
		if (onGround != state.onGround) {
			service->OnGroundFrame(player, onGround, state.onGround, engine::CurTime());
		}
		state.onGround = onGround;
		state.moveType = moveType;
	}

	// AirMove equivalent: air time only accumulates while airborne and walking.
	service->OnProcessMovementFrame();
	if (!onGround && moveType == MOVETYPE_WALK) {
		service->OnAirMoveFrame(frametime > 0.0f ? frametime : ENGINE_FIXED_TICK_INTERVAL);
	}
	service->OnProcessMovementPostFrame(player);

	// OnJumpLegacy/OnJumpModern equivalent: the jump press the engine actually executed, with the
	// frame-perfect flag the original computed from the modern-jump data / move-delta window.
	const bool jumpUsable = engine::PlayerFramePerfectJump(player->index);
	if (jumpUsable && !state.framePerfectActive) {
		service->OnServerJump(player, true);
	}
	state.framePerfectActive = jumpUsable;

	// PhysicsSimulatePost equivalent. The original skipped these three checks entirely while
	// ShouldRunDetections was false (sv_cheats); the movement bookkeeping above kept running, the
	// same way its movement hooks did.
	if (!service->ShouldRunDetections()) {
		return;
	}
	if (settings::IsDetectionEnabled(DetectionType::Nulls)) {
		service->CheckNulls(player);
	}
	service->CheckSuspiciousSubtickCommands(player, timeInServer);
	service->CheckLandingEvents(player);
}

} // namespace detection
} // namespace cs2ac
