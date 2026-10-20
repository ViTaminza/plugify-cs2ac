// Input-history time integrity: BACKTRACK, TICK MANIPULATION, INTERP BACKTRACK telemetry and
// MOUSE MISMATCH (fork v3-v9.1 tick_integrity_module.cpp).
//
// Every CS2 usercmd carries input-history entries with the client's render time and interpolation
// info. For an honest client that time only moves forward. A shot entry that dips below both
// neighbours is the lag-compensation rewrite of a backtrack cheat; a shot entry with
// player_tick_count == 0 is rapid-fire tick forgery. Both use the same tolerant protobuf reader as
// the rest of the port (UsercmdView::inputHistory).
//
// Report-only telemetry is always written (tick_stats / backtrack_shot / interp_shot / tick_forgery)
// through detection_log; the decisions themselves are pure functions in decisions.cpp.

#include "cs2ac.hpp"
#include "detection_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace cs2ac {
namespace detection {

namespace {

constexpr std::size_t recentCommands = 8;
constexpr int backtrackWindowTicks = 120 * 64;
constexpr int graceTicks = 5 * 64;          // ignore the first seconds after the player starts sending commands
constexpr int playerTickTolerance = 4 * 64; // |player_tick_count - server tick| above this is not a clock
constexpr int zeroTickWindowTicks = 10 * 64;
constexpr std::size_t zeroTickShotsNeeded = 3;
constexpr int interpRowsPerPlayer = 10;
constexpr auto summaryInterval = std::chrono::seconds(120);

bool DebugEnabled()
{
	return settings::DebugFlag("cs2ac_tick_integrity_debug");
}

f64 RenderTime(const InputHistoryEntry& entry)
{
	return TickRenderTime(entry.hasRenderTickCount, entry.renderTickCount, entry.hasRenderTickFraction, entry.renderTickFraction);
}

f64 InterpTime(const InputHistoryEntry& entry)
{
	if (!entry.hasSvInterp0) {
		return kTickNone;
	}
	return TickInterpTime(entry.svInterp0);
}

f64 Max(const std::deque<f64>& values)
{
	f64 best = kTickNone;
	for (const f64 value : values) {
		best = (std::max)(best, value);
	}
	return best;
}

template <typename T>
void Expire(std::deque<T>& values, int now, int window)
{
	while (!values.empty() && now - values.front() > window) {
		values.pop_front();
	}
}

} // namespace

void TickIntegrityModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
	Reset();
}

void TickIntegrityModule::Unload()
{
	Reset();
	announce = nullptr;
}

void TickIntegrityModule::Reset()
{
	playerData = {};
}

void TickIntegrityModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		auto& data = playerData[static_cast<std::size_t>(player->index)];
		if (data.dirty) {
			WriteSummary(player, data);
		}
		data = {};
	}
}

void TickIntegrityModule::OnSetupMove(PlayerRef* player, const UsercmdView& command, int currentTick)
{
	if (!settings::BoolSetting("cs2ac_tick_integrity_enabled", true) || !IsEligibleHuman(player)) {
		return;
	}
	auto& data = playerData[static_cast<std::size_t>(player->index)];
	if (!player->IsAlive()) {
		data.recentRender.clear();
		data.recentInterp.clear();
		data.pending = false;
		data.angleKnown = false;
		return;
	}
	if (command.commandNumber <= data.lastCommandNumber) {
		return;
	}
	data.lastCommandNumber = command.commandNumber;
	TrackAngleInput(data, command);
	CheckMouseMismatch(player, data);
	if (data.firstTick < 0) {
		data.firstTick = currentTick;
	}
	const bool graceOver = currentTick - data.firstTick > graceTicks;

	const bool hasAttack = command.hasAttack1StartHistoryIndex && command.attack1StartHistoryIndex >= 0
						   && command.attack1StartHistoryIndex < static_cast<int>(command.inputHistory.size());
	const int attackIndex = hasAttack ? command.attack1StartHistoryIndex : -1;

	f64 beforeRender = kTickNone;
	f64 beforeInterp = kTickNone;
	f64 afterRender = std::numeric_limits<f64>::max();
	f64 afterInterp = std::numeric_limits<f64>::max();
	f64 commandMaxRender = kTickNone;
	f64 commandMaxInterp = kTickNone;
	f64 commandMinRender = std::numeric_limits<f64>::max();
	f64 commandMinInterp = std::numeric_limits<f64>::max();
	bool zeroPlayerTick = false;
	bool farPlayerTick = false;
	int farDelta = 0;

	for (int index = 0; index < static_cast<int>(command.inputHistory.size()); ++index) {
		const InputHistoryEntry& entry = command.inputHistory[static_cast<std::size_t>(index)];
		if (graceOver && entry.hasPlayerTickCount) {
			const int playerTick = entry.playerTickCount;
			if (playerTick == 0) {
				zeroPlayerTick = true;
			} else if (std::abs(playerTick - currentTick) > playerTickTolerance) {
				farPlayerTick = true;
				farDelta = playerTick - currentTick;
			}
		}
		if (hasAttack && index == attackIndex) {
			continue;
		}
		const f64 render = RenderTime(entry);
		const f64 interp = InterpTime(entry);
		if (render > 0.0) {
			commandMaxRender = (std::max)(commandMaxRender, render);
			commandMinRender = (std::min)(commandMinRender, render);
		}
		if (interp > 0.0) {
			commandMaxInterp = (std::max)(commandMaxInterp, interp);
			commandMinInterp = (std::min)(commandMinInterp, interp);
		}
		if (hasAttack && index < attackIndex) {
			beforeRender = (std::max)(beforeRender, render);
			beforeInterp = (std::max)(beforeInterp, interp);
		} else if (hasAttack && index > attackIndex) {
			if (render > 0.0) {
				afterRender = (std::min)(afterRender, render);
			}
			if (interp > 0.0) {
				afterInterp = (std::min)(afterInterp, interp);
			}
		}
	}

	// A shot from the previous command waits for this command's entries.
	if (data.pending) {
		ResolveShot(player, data, commandMinRender < std::numeric_limits<f64>::max() ? commandMinRender : kTickNone,
					commandMinInterp < std::numeric_limits<f64>::max() ? commandMinInterp : kTickNone, currentTick);
	}

	if (hasAttack) {
		const InputHistoryEntry& shot = command.inputHistory[static_cast<std::size_t>(attackIndex)];
		data.pendingRender = RenderTime(shot);
		data.pendingInterp = InterpTime(shot);
		// Rewriting cheats set the shot entry to a whole older tick and drop its interpolation info;
		// an honest entry carries a fraction and sv_interp0.
		const bool wholeTick = shot.hasRenderTickCount && (!shot.hasRenderTickFraction || shot.renderTickFraction == 0.0f);
		data.pendingForged = TickRewriteFingerprint(wholeTick, data.pendingInterp);
		data.pendingInterpInfo = {};
		if (data.pendingRender > 0.0 && data.pendingInterp > 0.0) {
			auto& info = data.pendingInterpInfo;
			info.known = true;
			info.offset = data.pendingRender - data.pendingInterp;
			info.src0 = shot.svInterp0.srcTick;
			info.dst0 = shot.svInterp0.hasDstTick ? shot.svInterp0.dstTick : -1;
			info.frac0 = shot.svInterp0.hasFrac ? shot.svInterp0.frac : 0.0f;
			if (shot.hasSvInterp1) {
				info.src1 = shot.svInterp1.hasSrcTick ? shot.svInterp1.srcTick : -1;
				info.dst1 = shot.svInterp1.hasDstTick ? shot.svInterp1.dstTick : -1;
				info.frac1 = shot.svInterp1.hasFrac ? shot.svInterp1.frac : 0.0f;
			}
			if (shot.hasClInterp && shot.clInterp.hasFrac) {
				info.clFrac = shot.clInterp.frac;
			}
		}
		data.pendingPrevRender = (std::max)(Max(data.recentRender), beforeRender);
		data.pendingPrevInterp = (std::max)(Max(data.recentInterp), beforeInterp);
		data.pendingTick = currentTick;
		data.pending = data.pendingRender > 0.0 || data.pendingInterp > 0.0;
		if (data.pending && (afterRender < std::numeric_limits<f64>::max() || afterInterp < std::numeric_limits<f64>::max())) {
			ResolveShot(player, data, afterRender < std::numeric_limits<f64>::max() ? afterRender : kTickNone,
						afterInterp < std::numeric_limits<f64>::max() ? afterInterp : kTickNone, currentTick);
		}
	}

	if (commandMaxRender > 0.0) {
		data.recentRender.push_back(commandMaxRender);
		while (data.recentRender.size() > recentCommands) {
			data.recentRender.pop_front();
		}
	}
	if (commandMaxInterp > 0.0) {
		data.recentInterp.push_back(commandMaxInterp);
		while (data.recentInterp.size() > recentCommands) {
			data.recentInterp.pop_front();
		}
	}

	if (farPlayerTick) {
		if (data.farTicks == 0) {
			data.minTickDelta = data.maxTickDelta = farDelta;
		}
		++data.farTicks;
		data.minTickDelta = (std::min)(data.minTickDelta, farDelta);
		data.maxTickDelta = (std::max)(data.maxTickDelta, farDelta);
		data.dirty = true;
	}

	// Rapid-fire forging zeroes the tick on the commands that shoot.
	if (zeroPlayerTick && hasAttack) {
		++data.zeroTickShots;
		data.dirty = true;
		data.zeroTickCommands.push_back(currentTick);
		Expire(data.zeroTickCommands, currentTick, zeroTickWindowTicks);
		if (DebugEnabled()) {
			ConsoleMessage("[CS2AC Tick] %s shot with player_tick_count 0 at server tick %d.", player->GetName(), currentTick);
		}
		if (!data.reportedTicks && announce && static_cast<int>(data.zeroTickCommands.size()) >= static_cast<int>(zeroTickShotsNeeded)) {
			data.reportedTicks = true;
			detection_log::WriteTelemetry("tick_forgery", player,
										  ",\"zero_tick_shots\":" + std::to_string(data.zeroTickCommands.size())
											  + ",\"server_tick\":" + std::to_string(currentTick));
			announce("TICK MANIPULATION", player,
					 localization::Format("evidence.tick_manipulation",
										  "{count} shots within 10 seconds were sent with a player tick count of exactly zero. A normal client "
										  "always sends its current tick; rapid-fire cheats overwrite it with zero.",
										  {{"count", std::to_string(data.zeroTickCommands.size())}}));
		}
	}
}

void TickIntegrityModule::ResolveShot(PlayerRef* player, TickIntegrityPlayerData& data, f64 nextRender, f64 nextInterp, int currentTick)
{
	data.pending = false;
	const f64 renderDip = TickDip(data.pendingPrevRender, data.pendingRender, nextRender);
	const f64 interpDip = TickDip(data.pendingPrevInterp, data.pendingInterp, nextInterp);
	CheckInterpOffset(player, data, renderDip, nextRender, currentTick);
	if (std::isnan(renderDip) && std::isnan(interpDip)) {
		return; // not enough context around this shot
	}
	const f64 dip = std::isnan(renderDip) ? interpDip : std::isnan(interpDip) ? renderDip : (std::max)(renderDip, interpDip);
	++data.shots;
	data.dirty = true;
	const f32 dipTicks = static_cast<f32>((std::max)(0.0, dip));
	++data.dipBuckets[TickDipBucket(dipTicks)];
	data.maxDip = (std::max)(data.maxDip, dipTicks);

	const f32 minimumTicks = settings::FloatSetting("cs2ac_backtrack_min_ticks", 2.0f);
	if (dipTicks < minimumTicks) {
		return;
	}
	// The client's clock must be continuous around the shot; a jump, or a step backwards, means a
	// stall, not a rewrite.
	const bool renderDipped = !std::isnan(renderDip) && renderDip >= dip - 1e-6;
	const f64 gap = renderDipped ? nextRender - data.pendingPrevRender : nextInterp - data.pendingPrevInterp;
	const f32 maximumGap = settings::FloatSetting("cs2ac_backtrack_max_gap", 4.0f);
	if (gap < 0.0 || gap > maximumGap) {
		++data.hitchSkips;
		if (DebugEnabled()) {
			ConsoleMessage("[CS2AC Tick] %s: %.2f-tick dip ignored, clock moved %.2f ticks around the shot.", player->GetName(),
						   static_cast<double>(dipTicks), gap);
		}
		return;
	}
	// The rewritten shot entry is a whole tick without sv_interp0 while the entries around it keep
	// their fractions; a clock on whole ticks everywhere proves nothing.
	const bool forged = data.pendingForged && TickHasFraction(data.pendingPrevRender) && TickHasFraction(nextRender);

	char fields[384];
	std::snprintf(fields, sizeof(fields),
				  ",\"server_tick\":%d,\"dip_ticks\":%.3f,\"render_prev\":%.3f,\"render_shot\":%.3f,\"render_next\":%.3f,"
				  "\"interp_prev\":%.3f,\"interp_shot\":%.3f,\"interp_next\":%.3f,\"gap_ticks\":%.3f,\"forged\":%s",
				  data.pendingTick, static_cast<double>(dipTicks), data.pendingPrevRender, data.pendingRender, nextRender,
				  data.pendingPrevInterp, data.pendingInterp, nextInterp, gap, forged ? "true" : "false");
	detection_log::WriteTelemetry("backtrack_shot", player, fields);
	if (DebugEnabled()) {
		ConsoleMessage("[CS2AC Tick] %s: shot at tick %d dipped %.2f ticks into the past (render %.2f/%.2f/%.2f, interp %.2f/%.2f/%.2f, forged %s).",
					   player->GetName(), data.pendingTick, static_cast<double>(dipTicks), data.pendingPrevRender, data.pendingRender, nextRender,
					   data.pendingPrevInterp, data.pendingInterp, nextInterp, forged ? "yes" : "no");
	}

	if (!forged && settings::BoolSetting("cs2ac_backtrack_require_forged", true)) {
		++data.unforgedDips;
		return;
	}
	if (!BacktrackDipCounts(dipTicks, minimumTicks, gap, maximumGap, forged, settings::BoolSetting("cs2ac_backtrack_require_forged", true),
							TickHasFraction(data.pendingPrevRender), TickHasFraction(nextRender))) {
		return;
	}
	data.backtrackTicks.push_back(data.pendingTick);
	Expire(data.backtrackTicks, currentTick, backtrackWindowTicks);

	const int needed = (std::max)(1, settings::IntSetting("cs2ac_backtrack_shots", 3));
	if (data.reportedBacktrack || !announce || static_cast<int>(data.backtrackTicks.size()) < needed) {
		return;
	}
	data.reportedBacktrack = true;
	char amount[16];
	std::snprintf(amount, sizeof(amount), "%.1f", static_cast<double>(dipTicks));
	announce("BACKTRACK", player,
			 localization::Format("evidence.backtrack",
								  "{count} shots within 120 seconds were sent with a time up to {amount} ticks older than the inputs sent "
								  "immediately before and after them. A normal client's clock only moves forward; backtrack cheats rewind "
								  "the shot to hit an enemy's past position.",
								  {{"count", std::to_string(data.backtrackTicks.size())}, {"amount", amount}}));
}

void TickIntegrityModule::CheckInterpOffset(PlayerRef* player, TickIntegrityPlayerData& data, f64 renderDip, f64 nextRender, int currentTick)
{
	const ShotInterpInfo& info = data.pendingInterpInfo;
	if (!info.known || std::isnan(renderDip)) {
		return;
	}
	// Only shots on a continuous render clock: the shot entry did not dip itself and the client did
	// not stall around it.
	const f64 renderGap = nextRender - data.pendingPrevRender;
	if (renderDip >= 0.5 || renderGap < 0.0 || renderGap > settings::FloatSetting("cs2ac_backtrack_max_gap", 4.0f)) {
		return;
	}
	const f32 offset = static_cast<f32>(info.offset);
	++data.interpOffsetBuckets[TickInterpOffsetBucket(offset)];
	data.maxInterpOffset = (std::max)(data.maxInterpOffset, offset);
	data.dirty = true;
	if (offset < 3.0f) {
		return;
	}

	if (data.interpRowsLogged < interpRowsPerPlayer) {
		++data.interpRowsLogged;
		char fields[384];
		std::snprintf(fields, sizeof(fields),
					  ",\"server_tick\":%d,\"offset_ticks\":%.3f,\"render_prev\":%.3f,\"render_shot\":%.3f,\"render_next\":%.3f,"
					  "\"interp_shot\":%.3f,\"interp0\":[%d,%d,%.4f],\"interp1\":[%d,%d,%.4f],\"cl_interp_frac\":%.4f",
					  data.pendingTick, static_cast<double>(offset), data.pendingPrevRender, data.pendingRender, nextRender, data.pendingInterp,
					  info.src0, info.dst0, static_cast<double>(info.frac0), info.src1, info.dst1, static_cast<double>(info.frac1),
					  static_cast<double>(info.clFrac));
		detection_log::WriteTelemetry("interp_shot", player, fields);
	}
	if (DebugEnabled()) {
		ConsoleMessage("[CS2AC Tick] %s: shot at tick %d has sv_interp0 %.2f ticks behind its render time.", player->GetName(), data.pendingTick,
					   static_cast<double>(offset));
	}

	if (offset < settings::FloatSetting("cs2ac_backtrack_interp_min_ticks", 4.0f)) {
		return;
	}
	data.interpBacktrackTicks.push_back(data.pendingTick);
	Expire(data.interpBacktrackTicks, currentTick, backtrackWindowTicks);
	const int needed = (std::max)(1, settings::IntSetting("cs2ac_backtrack_shots", 3));
	if (data.reportedInterpBacktrack || !announce || !settings::BoolSetting("cs2ac_backtrack_interp_report", false)
		|| static_cast<int>(data.interpBacktrackTicks.size()) < needed) {
		return;
	}
	data.reportedInterpBacktrack = true;
	char amount[16];
	std::snprintf(amount, sizeof(amount), "%.1f", static_cast<double>(offset));
	announce("INTERP BACKTRACK", player,
			 localization::Format("evidence.interp_backtrack",
								  "{count} shots within 120 seconds carried interpolation data up to {amount} ticks older than the shot's own "
								  "render time while the client's clock stayed continuous. An honest client keeps them one or two ticks apart; "
								  "backtrack cheats move only the interpolation data of the shot.",
								  {{"count", std::to_string(data.interpBacktrackTicks.size())}, {"amount", amount}}));
}

void TickIntegrityModule::CheckMouseMismatch(PlayerRef* player, TickIntegrityPlayerData& data)
{
	const int needed = settings::IntSetting("cs2ac_mouse_mismatch_samples", 100);
	if (data.reportedMouseMismatch || !announce || needed <= 0 || data.sessionYawSamples < needed) {
		return;
	}
	const f32 agreement = static_cast<f32>(data.sessionYawAgree) / static_cast<f32>(data.sessionYawSamples);
	if (agreement >= settings::FloatSetting("cs2ac_mouse_mismatch_agreement", 0.9f)) {
		return;
	}
	data.reportedMouseMismatch = true;
	char share[16];
	std::snprintf(share, sizeof(share), "%.0f", static_cast<double>(100.0f * agreement));
	announce("MOUSE MISMATCH", player,
			 localization::Format("evidence.mousemismatch",
								  "Only {agree}% of {samples} view turns went the way the mouse moved; for honest players it is 99% or "
								  "more. Cheats that write the view angles turn the camera regardless of the mouse.",
								  {{"agree", share}, {"samples", std::to_string(data.sessionYawSamples)}}));
}

void TickIntegrityModule::TrackAngleInput(TickIntegrityPlayerData& data, const UsercmdView& command)
{
	if (!command.hasBase || !command.hasViewAngles) {
		data.angleKnown = false;
		return;
	}
	const f32 pitch = command.viewAngles.x;
	const f32 yaw = command.viewAngles.y;
	if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
		data.angleKnown = false;
		return;
	}
	const unsigned int serverChanges = command.hasConsumedServerAngleChanges ? command.consumedServerAngleChanges : 0;
	// Only consecutive commands, and only when the server did not set the angles in between.
	if (data.angleKnown && command.commandNumber == data.angleCommand + 1 && serverChanges == data.angleServerChanges) {
		const f32 yawChange = std::fmod(yaw - data.angleYaw + 540.0f, 360.0f) - 180.0f;
		const f32 pitchChange = pitch - data.anglePitch;
		const f32 rotation = std::sqrt(yawChange * yawChange + pitchChange * pitchChange);
		f32 subtickYaw = 0.0f;
		f32 subtickPitch = 0.0f;
		for (const SubtickMove& move : command.subtickMoves) {
			if (move.hasYawDelta && std::isfinite(move.yawDelta)) {
				subtickYaw += move.yawDelta;
			}
			if (move.hasPitchDelta && std::isfinite(move.pitchDelta)) {
				subtickPitch += move.pitchDelta;
			}
		}
		const bool noInput = command.mouseDx == 0 && command.mouseDy == 0 && std::abs(subtickYaw) < 1e-4f && std::abs(subtickPitch) < 1e-4f;
		++data.angleCommands;
		if (rotation >= 0.5f) {
			++data.rotatingCommands;
			if (noInput) {
				const std::size_t bucket = rotation < 2.0f ? 0 : rotation < 5.0f ? 1 : rotation < 15.0f ? 2 : 3;
				++data.rotationWithoutInput[bucket];
				data.maxRotationWithoutInput = (std::max)(data.maxRotationWithoutInput, rotation);
			}
		}
		if (command.mouseDx != 0 && std::abs(yawChange) >= 0.05f) {
			// Moving the mouse right (positive mousedx) turns the view right, which lowers yaw.
			++data.yawSignSamples;
			++data.sessionYawSamples;
			if ((yawChange < 0.0f) == (command.mouseDx > 0)) {
				++data.yawSignAgree;
				++data.sessionYawAgree;
			}
		}
		data.dirty = true;
	}
	data.angleKnown = true;
	data.angleCommand = command.commandNumber;
	data.angleYaw = yaw;
	data.anglePitch = pitch;
	data.angleServerChanges = serverChanges;
}

void TickIntegrityModule::OnGameFrame()
{
	if (!g_pPlayerManager) {
		return;
	}
	const auto now = Clock::now();
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto& data = playerData[static_cast<std::size_t>(index)];
		if (!data.dirty || now < data.nextSummary) {
			continue;
		}
		if (auto* player = g_pPlayerManager->ToPlayer(index)) {
			WriteSummary(player, data);
		}
		data.nextSummary = now + summaryInterval;
	}
}

void TickIntegrityModule::WriteSummary(PlayerRef* player, TickIntegrityPlayerData& data)
{
	char fields[1024];
	std::snprintf(fields, sizeof(fields),
				  ",\"shots\":%d,\"dip_buckets_ticks\":\"<0.5,<1,<2,<4,4+\",\"dip_counts\":[%d,%d,%d,%d,%d],\"max_dip\":%.3f,"
				  "\"hitch_skips\":%d,\"unforged_dips\":%d,\"zero_tick_shots\":%d,\"far_ticks\":%d,\"far_delta_min\":%d,\"far_delta_max\":%d,"
				  "\"interp_offset_buckets_ticks\":\"<1,<2,<3,<4,<6,6+\",\"interp_offset_counts\":[%d,%d,%d,%d,%d,%d],\"max_interp_offset\":%.3f,"
				  "\"angle_cmds\":%d,\"rotating_cmds\":%d,\"no_input_rotation_buckets_deg\":\"<2,<5,<15,15+\","
				  "\"no_input_rotation_counts\":[%d,%d,%d,%d],\"max_no_input_rotation\":%.2f,\"yaw_sign_samples\":%d,\"yaw_sign_agree\":%d",
				  data.shots, data.dipBuckets[0], data.dipBuckets[1], data.dipBuckets[2], data.dipBuckets[3], data.dipBuckets[4],
				  static_cast<double>(data.maxDip), data.hitchSkips, data.unforgedDips, data.zeroTickShots, data.farTicks, data.minTickDelta,
				  data.maxTickDelta, data.interpOffsetBuckets[0], data.interpOffsetBuckets[1], data.interpOffsetBuckets[2],
				  data.interpOffsetBuckets[3], data.interpOffsetBuckets[4], data.interpOffsetBuckets[5], static_cast<double>(data.maxInterpOffset),
				  data.angleCommands, data.rotatingCommands, data.rotationWithoutInput[0], data.rotationWithoutInput[1],
				  data.rotationWithoutInput[2], data.rotationWithoutInput[3], static_cast<double>(data.maxRotationWithoutInput),
				  data.yawSignSamples, data.yawSignAgree);
	detection_log::WriteTelemetry("tick_stats", player, fields);
	data.shots = 0;
	data.dipBuckets.fill(0);
	data.maxDip = 0.0f;
	data.hitchSkips = 0;
	data.unforgedDips = 0;
	data.interpOffsetBuckets.fill(0);
	data.maxInterpOffset = 0.0f;
	data.angleCommands = 0;
	data.rotatingCommands = 0;
	data.rotationWithoutInput.fill(0);
	data.maxRotationWithoutInput = 0.0f;
	data.yawSignSamples = 0;
	data.yawSignAgree = 0;
	data.zeroTickShots = 0;
	data.farTicks = 0;
	data.minTickDelta = 0;
	data.maxTickDelta = 0;
	data.dirty = false;
}

} // namespace detection
} // namespace cs2ac
