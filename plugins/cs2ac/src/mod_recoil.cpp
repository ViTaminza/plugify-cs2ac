// Report-only recoil-control (RCS) statistics (fork v4 recoil_control_module.cpp).
//
// During a spray the server knows both the player's view angles (from the usercmd) and the
// weapon's aim punch. Bullets go to view + weapon_recoil_scale * punch. An RCS cheat moves the view
// against the punch on the same or the next tick, so the tick-to-tick view change mirrors the punch
// change almost exactly; human recoil control lags and is smooth.
//
// Reports are off by default (cs2ac_rcs_report 0); telemetry is always written, including the raw
// per-tick series for the first three sprays of each player per map.

#include "cs2ac.hpp"
#include "detection_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace cs2ac {
namespace detection {

namespace {

constexpr int maximumLag = 8;
constexpr std::size_t maximumSamples = 256;
constexpr f32 minimumPunchTravel = 4.0f; // degrees of scaled punch; below this there is nothing to compensate
constexpr std::size_t recentSprays = 12;
constexpr int rawSpraysPerPlayer = 3;

bool DebugEnabled()
{
	return settings::DebugFlag("cs2ac_rcs_debug");
}

f32 RecoilScale()
{
	const f32 value = engine::ServerCvarFloat("weapon_recoil_scale", 2.0f);
	return std::isfinite(value) && value > 0.0f ? value : 2.0f;
}

} // namespace

void RecoilControlModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
	available = false;
	checked = false;
	Reset();
}

void RecoilControlModule::Unload()
{
	Reset();
	announce = nullptr;
	available = false;
	checked = false;
}

void RecoilControlModule::Reset()
{
	playerData = {};
}

void RecoilControlModule::EnsureAvailability()
{
	if (checked) {
		return;
	}
	// GetSchemaOffset must not run in the plugin-start window: only probe once a map is up.
	if (!engine::GameReady()) {
		return;
	}
	checked = true;
	available = engine::RecoilSchemaAvailable();
	if (!available) {
		ConsoleMessage("RCS statistics are unavailable: a required CCSPlayerPawn/CCSPlayer_AimPunchServices field is missing "
					   "(m_pAimPunchServices, m_iShotsFired, m_predictableBaseAngle, m_unpredictableBaseAngle, their base ticks).");
	} else {
		ConsoleMessage("RCS statistics enabled (CCSPlayer_AimPunchServices).");
	}
}

void RecoilControlModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		playerData[static_cast<std::size_t>(player->index)] = {};
	}
}

void RecoilControlModule::OnWeaponFire(PlayerRef* player, std::string_view weapon)
{
	EnsureAvailability();
	if (!available || !IsEligibleHuman(player)) {
		return;
	}
	auto& data = playerData[static_cast<std::size_t>(player->index)];
	if (data.inSpray && !data.weapon.empty() && data.weapon != weapon) {
		data.invalid = true; // weapon switched mid-spray
	}
	data.weapon.assign(weapon);
}

void RecoilControlModule::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	EnsureAvailability();
	if (!available || !settings::BoolSetting("cs2ac_rcs_enabled", true) || !IsEligibleHuman(player) || !command.hasBase || !command.hasViewAngles) {
		return;
	}
	auto& data = playerData[static_cast<std::size_t>(player->index)];
	if (!player->IsAlive()) {
		data.inSpray = false;
		data.samples.clear();
		return;
	}
	if (command.commandNumber <= data.lastCommandNumber) {
		return;
	}
	const bool consecutive = data.lastCommandNumber < 0 || command.commandNumber == data.lastCommandNumber + 1;
	data.lastCommandNumber = command.commandNumber;

	engine::RecoilData recoil;
	if (!engine::ReadRecoilData(player->index, recoil)) {
		data.invalid = true;
		return;
	}
	if (recoil.shotsFired <= 0) {
		if (data.inSpray) {
			Finish(player, data);
		}
		return;
	}
	if (!data.inSpray) {
		data.inSpray = true;
		data.invalid = false;
		data.maxShots = 0;
		data.samples.clear();
	}
	if (!consecutive) {
		data.invalid = true; // a gap breaks the tick-to-tick series
	}
	if (!engine::PlayerOnGround(player->index)) {
		data.invalid = true; // air sprays are not recoil-controlled
	}
	data.maxShots = (std::max)(data.maxShots, static_cast<int>(recoil.shotsFired));

	const QAngle view = command.viewAngles;
	if (!std::isfinite(view.x) || !std::isfinite(view.y) || !std::isfinite(recoil.punchPitch) || !std::isfinite(recoil.punchYaw)
		|| !std::isfinite(recoil.randomPitch) || !std::isfinite(recoil.randomYaw)) {
		data.invalid = true;
		return;
	}
	if (data.samples.size() < maximumSamples) {
		RecoilSample sample;
		sample.viewPitch = view.x;
		sample.viewYaw = view.y;
		sample.punchPitch = recoil.punchPitch;
		sample.punchYaw = recoil.punchYaw;
		sample.randomPitch = recoil.randomPitch;
		sample.randomYaw = recoil.randomYaw;
		sample.predictableTick = recoil.predictableTick;
		sample.unpredictableTick = recoil.unpredictableTick;
		data.samples.push_back(sample);
	}
	(void)currentTick;
}

void RecoilControlModule::Finish(PlayerRef* player, RecoilPlayerData& data)
{
	data.inSpray = false;
	const std::vector<RecoilSample> samples = std::move(data.samples);
	data.samples.clear();
	if (!RecoilSprayQualifies(data.invalid, data.maxShots, samples.size(), data.weapon)) {
		if (DebugEnabled()) {
			ConsoleMessage("[CS2AC RCS] %s: spray skipped (invalid=%d shots=%d samples=%zu weapon=%s automatic=%d).", player->GetName(),
						   data.invalid ? 1 : 0, data.maxShots, samples.size(), data.weapon.empty() ? "<none>" : data.weapon.c_str(),
						   IsAutomaticWeapon(data.weapon) ? 1 : 0);
		}
		return;
	}

	const f32 scale = RecoilScale();
	const std::size_t count = samples.size();
	// Tick-to-tick changes: index t is the change from sample t-1 to t.
	std::vector<f32> viewPitch(count, 0.0f), viewYaw(count, 0.0f), punchPitch(count, 0.0f), punchYaw(count, 0.0f);
	f32 punchTravel = 0.0f;
	for (std::size_t t = 1; t < count; ++t) {
		viewPitch[t] = RecoilAngleDelta(samples[t].viewPitch, samples[t - 1].viewPitch);
		viewYaw[t] = RecoilAngleDelta(samples[t].viewYaw, samples[t - 1].viewYaw);
		punchPitch[t] = scale * RecoilAngleDelta(samples[t].punchPitch, samples[t - 1].punchPitch);
		punchYaw[t] = scale * RecoilAngleDelta(samples[t].punchYaw, samples[t - 1].punchYaw);
		punchTravel += std::fabs(punchPitch[t]) + std::fabs(punchYaw[t]);
	}
	if (punchTravel < minimumPunchTravel) {
		if (DebugEnabled()) {
			ConsoleMessage("[CS2AC RCS] %s: spray skipped, aim punch barely moved (%.2f degrees). Does CCSPlayer_AimPunchServices update per tick?",
						   player->GetName(), static_cast<double>(punchTravel));
		}
		return;
	}

	std::array<f32, maximumLag + 1> correlations {};
	std::array<f32, maximumLag + 1> gains {};
	for (int lag = 0; lag <= maximumLag; ++lag) {
		std::vector<f32> x;
		std::vector<f32> y;
		for (std::size_t t = static_cast<std::size_t>(1 + lag); t < count; ++t) {
			// Perfect compensation: view change == -(scaled punch change) `lag` ticks earlier.
			x.push_back(viewPitch[t]);
			y.push_back(-punchPitch[t - static_cast<std::size_t>(lag)]);
			x.push_back(viewYaw[t]);
			y.push_back(-punchYaw[t - static_cast<std::size_t>(lag)]);
		}
		correlations[static_cast<std::size_t>(lag)] = RecoilCorrelation(x, y);
		f64 xy = 0.0;
		f64 yy = 0.0;
		for (std::size_t index = 0; index < x.size(); ++index) {
			xy += static_cast<f64>(x[index]) * y[index];
			yy += static_cast<f64>(y[index]) * y[index];
		}
		gains[static_cast<std::size_t>(lag)] = yy > 1e-9 ? static_cast<f32>(xy / yy) : 0.0f;
	}
	const int bestLag = static_cast<int>(std::max_element(correlations.begin(), correlations.end()) - correlations.begin());

	// Same test against the unpredictable (random) part only, and how often each base tick moved.
	std::array<f32, maximumLag + 1> randomCorrelations {};
	for (int lag = 0; lag <= maximumLag; ++lag) {
		std::vector<f32> x;
		std::vector<f32> y;
		for (std::size_t t = static_cast<std::size_t>(1 + lag); t < count; ++t) {
			x.push_back(viewPitch[t]);
			y.push_back(-scale * RecoilAngleDelta(samples[t - static_cast<std::size_t>(lag)].randomPitch,
												  samples[t - static_cast<std::size_t>(lag) - 1].randomPitch));
			x.push_back(viewYaw[t]);
			y.push_back(-scale * RecoilAngleDelta(samples[t - static_cast<std::size_t>(lag)].randomYaw,
												  samples[t - static_cast<std::size_t>(lag) - 1].randomYaw));
		}
		randomCorrelations[static_cast<std::size_t>(lag)] = RecoilCorrelation(x, y);
	}
	int predictableTickChanges = 0;
	int unpredictableTickChanges = 0;
	for (std::size_t t = 1; t < count; ++t) {
		predictableTickChanges += samples[t].predictableTick != samples[t - 1].predictableTick ? 1 : 0;
		unpredictableTickChanges += samples[t].unpredictableTick != samples[t - 1].unpredictableTick ? 1 : 0;
	}

	// Drift of the actual bullet line (view + scaled punch) from the first shot.
	f64 driftSquares = 0.0;
	for (const RecoilSample& sample : samples) {
		const f32 pitch =
			RecoilAngleDelta(sample.viewPitch + scale * sample.punchPitch, samples.front().viewPitch + scale * samples.front().punchPitch);
		const f32 yaw = RecoilAngleDelta(sample.viewYaw + scale * sample.punchYaw, samples.front().viewYaw + scale * samples.front().punchYaw);
		driftSquares += static_cast<f64>(pitch) * pitch + static_cast<f64>(yaw) * yaw;
	}
	const f32 driftRms = static_cast<f32>(std::sqrt(driftSquares / static_cast<f64>(samples.size())));

	const RecoilSprayResult result {correlations[static_cast<std::size_t>(bestLag)], gains[static_cast<std::size_t>(bestLag)], bestLag};
	data.recent.push_back(result);
	while (data.recent.size() > recentSprays) {
		data.recent.pop_front();
	}

	char buffer[512];
	std::string lags = "[";
	for (int lag = 0; lag <= maximumLag; ++lag) {
		std::snprintf(buffer, sizeof(buffer), "%s%.3f", lag ? "," : "", static_cast<double>(correlations[static_cast<std::size_t>(lag)]));
		lags += buffer;
	}
	lags += "]";
	std::snprintf(buffer, sizeof(buffer),
				  ",\"weapon\":%s,\"shots\":%d,\"samples\":%zu,\"best_lag\":%d,\"best_r\":%.3f,\"gain\":%.3f,\"punch_travel\":%.2f,"
				  "\"drift_rms\":%.3f,\"recoil_scale\":%.2f",
				  detection::JsonString(std::string(NormalizeWeapon(data.weapon)).c_str()).c_str(), data.maxShots, samples.size(), bestLag,
				  static_cast<double>(result.bestCorrelation), static_cast<double>(result.gain), static_cast<double>(punchTravel),
				  static_cast<double>(driftRms), static_cast<double>(scale));
	// v4 JSON hotfix: the buffer is reused for the random-lag loop below, so the base object must be
	// captured immediately (before the fix only the last random correlation reached the log).
	const std::string base(buffer);
	std::string randomLags = "[";
	for (int lag = 0; lag <= maximumLag; ++lag) {
		std::snprintf(buffer, sizeof(buffer), "%s%.3f", lag ? "," : "",
					  static_cast<double>(randomCorrelations[static_cast<std::size_t>(lag)]));
		randomLags += buffer;
	}
	randomLags += "]";
	std::string extra = ",\"r_by_lag\":" + lags + ",\"random_r_by_lag\":" + randomLags + ",\"predictable_tick_changes\":"
						+ std::to_string(predictableTickChanges) + ",\"unpredictable_tick_changes\":" + std::to_string(unpredictableTickChanges);
	// Raw per-tick series for the first sprays of each player per map, to calibrate the aim-punch
	// component offline. About 2 KB per spray.
	if (data.rawSpraysLogged < rawSpraysPerPlayer) {
		++data.rawSpraysLogged;
		auto series = [&](auto get) {
			std::string out = "[";
			char value[24];
			for (std::size_t t = 0; t < count; ++t) {
				std::snprintf(value, sizeof(value), "%s%.3f", t ? "," : "", static_cast<double>(get(samples[t])));
				out += value;
			}
			return out + "]";
		};
		extra += ",\"raw_view_pitch\":" + series([](const RecoilSample& sample) { return sample.viewPitch; });
		extra += ",\"raw_view_yaw\":" + series([](const RecoilSample& sample) { return sample.viewYaw; });
		extra += ",\"raw_punch_pitch\":" + series([](const RecoilSample& sample) { return sample.punchPitch; });
		extra += ",\"raw_punch_yaw\":" + series([](const RecoilSample& sample) { return sample.punchYaw; });
		extra += ",\"raw_random_pitch\":" + series([](const RecoilSample& sample) { return sample.randomPitch; });
		extra += ",\"raw_predictable_tick\":" + series([](const RecoilSample& sample) { return static_cast<f32>(sample.predictableTick); });
		extra += ",\"raw_unpredictable_tick\":"
				 + series([](const RecoilSample& sample) { return static_cast<f32>(sample.unpredictableTick); });
	}
	detection_log::WriteTelemetry("rcs_spray", player, base + extra);
	if (DebugEnabled()) {
		ConsoleMessage("[CS2AC RCS] %s sprayed %d shots with %s: best r %.3f at lag %d, gain %.2f, drift %.2f.", player->GetName(), data.maxShots,
					   data.weapon.c_str(), static_cast<double>(result.bestCorrelation), bestLag, static_cast<double>(result.gain),
					   static_cast<double>(driftRms));
	}

	if (data.reported || !announce || !settings::BoolSetting("cs2ac_rcs_report", false)
		|| !RecoilSuspiciousShare(data.recent, settings::FloatSetting("cs2ac_rcs_correlation", 0.9f),
								  settings::FloatSetting("cs2ac_rcs_suspicious_share", 0.8f))) {
		return;
	}
	data.reported = true;
	// Recount for the evidence sentence (the qualifier is the same share test).
	std::size_t machineLike = 0;
	for (const RecoilSprayResult& spray : data.recent) {
		if (spray.bestCorrelation >= settings::FloatSetting("cs2ac_rcs_correlation", 0.9f) && spray.bestLag >= 0 && spray.bestLag <= 2
			&& spray.gain >= 0.5f && spray.gain <= 1.3f) {
			++machineLike;
		}
	}
	announce("RCS", player,
			 localization::Format("evidence.rcs.sprays",
								  "In {machine} of the last {total} sprays the view moved against the recoil almost exactly, tick by tick, "
								  "within {lag} ticks of the recoil itself.",
								  {{"machine", std::to_string(machineLike)}, {"total", std::to_string(data.recent.size())}, {"lag", "2"}}));
}

} // namespace detection
} // namespace cs2ac
