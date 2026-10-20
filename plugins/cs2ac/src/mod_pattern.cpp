// Report-only triggerbot statistics that survive "humanised" delays (fork
// trigger_pattern_module.cpp, packages v3/v5/v9).
//
// Three independent measurements, each logged to telemetry:
//   1. Click hold time (synthetic clicks are held for microseconds).
//   2. Reaction-delay shape when an enemy walks into a held crosshair.
//   3. Share of combat presses that start with the crosshair already on the enemy.
//
// Nothing here punishes on its own data: it raises one report per map per signal for extreme,
// sustained values; cs2ac_kick_detections may still escalate TRIGGER PATTERN to a kick.

#include "cs2ac.hpp"
#include "detection_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace cs2ac {
namespace detection {

namespace {

constexpr f32 tickMilliseconds = ENGINE_FIXED_TICK_INTERVAL * 1000.0f;
constexpr f32 tapLimitMs = 300.0f; // longer holds are sprays, not taps
constexpr std::size_t tapWindow = 60;
constexpr std::size_t tapMinimum = 40;
constexpr std::size_t reactionWindow = 60;
constexpr std::size_t reactionMinimum = 30;
constexpr int reactionMaximumTicks = 64; // a "reaction" longer than 1 s is not one
constexpr std::size_t combatWindow = 60;
constexpr std::size_t combatMinimum = 40;
constexpr f32 nearMissDegrees = 4.0f;
constexpr std::size_t fastCombatMinimum = 10;
constexpr f32 fastOnTargetShare = 0.9f;
constexpr f32 nearMissMaximumDistance = 3000.0f;
constexpr auto summaryInterval = std::chrono::seconds(120);

bool DebugEnabled()
{
	return settings::DebugFlag("cs2ac_trigger_pattern_debug");
}

template <typename T>
void PushBounded(std::deque<T>& values, T value, std::size_t limit)
{
	values.push_back(value);
	while (values.size() > limit) {
		values.pop_front();
	}
}

std::string IntList(const std::vector<int>& values)
{
	std::string out = "[";
	for (std::size_t index = 0; index < values.size(); ++index) {
		out += (index ? "," : "") + std::to_string(values[index]);
	}
	return out + "]";
}

std::string FloatList(const std::vector<f32>& values)
{
	std::string out = "[";
	char buffer[32];
	for (std::size_t index = 0; index < values.size(); ++index) {
		std::snprintf(buffer, sizeof(buffer), "%s%.2f", index ? "," : "", static_cast<double>(values[index]));
		out += buffer;
	}
	return out + "]";
}

// Chest point of the nearest living, visible enemy within `maxDegrees` of the aim line.
bool VisibleEnemyNearAim(PlayerRef* player, const Vector& eye, const QAngle& angles, f32 maxDegrees, f32* degreesOut)
{
	if (!player || !g_pPlayerManager || !IsFinite(eye) || !IsFinite(angles)) {
		return false;
	}
	const Vector forward = AimForward(angles);
	f32 best = maxDegrees;
	bool found = false;
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		auto* target = g_pPlayerManager->ToPlayer(index);
		if (!target || target == player) {
			continue;
		}
		if (!target->IsAlive() || !AreOpponents(engine::PlayerTeam(player->index), engine::PlayerTeam(target->index))) {
			continue;
		}
		Vector targetEye;
		if (!engine::ReadEyeOrigin(target->index, targetEye)) {
			continue;
		}
		const Vector chest(targetEye.x, targetEye.y, targetEye.z - 20.0f);
		const f32 dx = chest.x - eye.x;
		const f32 dy = chest.y - eye.y;
		const f32 dz = chest.z - eye.z;
		const f32 distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (!std::isfinite(distance) || distance < 1.0f || distance > nearMissMaximumDistance) {
			continue;
		}
		const f32 cosine = (forward.x * dx + forward.y * dy + forward.z * dz) / distance;
		const f32 degrees = std::acos(std::clamp(cosine, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
		if (degrees > best) {
			continue;
		}
		engine::TraceResult trace;
		if (engine::TraceLine(eye, chest, player->index, trace) && trace.hit && trace.hitEntityIndex != target->index) {
			continue; // a wall or another player is in the way
		}
		best = degrees;
		found = true;
	}
	if (found && degreesOut) {
		*degreesOut = best;
	}
	return found;
}

} // namespace

void TriggerPatternModule::Load(AnnounceCallback announceCallback)
{
	announce = std::move(announceCallback);
	Reset();
}

void TriggerPatternModule::Unload()
{
	Reset();
	announce = nullptr;
}

void TriggerPatternModule::Reset()
{
	playerData = {};
}

void TriggerPatternModule::OnClientDisconnect(PlayerRef* player)
{
	if (player && player->index >= 1 && player->index <= MAXPLAYERS) {
		auto& data = playerData[static_cast<std::size_t>(player->index)];
		if (data.dirty) {
			WriteSummary(player, data);
		}
		data = {};
	}
}

void TriggerPatternModule::RecordHold(TriggerPatternPlayerData& data, f32 milliseconds, int pressCommand)
{
	if (!std::isfinite(milliseconds) || milliseconds < 0.0f) {
		return;
	}
	data.holdPending = true;
	data.pendingHoldMs = milliseconds;
	data.pendingHoldPressCommand = pressCommand;
	++data.holdBuckets[TriggerPatternHoldBucket(milliseconds)];
	if (milliseconds < tapLimitMs) {
		PushBounded(data.recentTapsMs, milliseconds, tapWindow);
		data.newTapsMs.push_back(milliseconds);
	}
	data.dirty = true;
}

void TriggerPatternModule::TrackClicks(TriggerPatternPlayerData& data, const UsercmdView& command)
{
	if (data.lastCommandNumber >= 0 && command.commandNumber != data.lastCommandNumber + 1) {
		data.pressTime = -1.0; // dropped or replayed commands: the open press can't be timed
	}
	data.lastCommandNumber = command.commandNumber;

	bool sawSubtickAttack = false;
	for (const SubtickMove& step : command.subtickMoves) {
		if (!step.hasButton || step.button != IN_ATTACK) {
			continue;
		}
		const f32 when = step.when;
		if (!std::isfinite(when) || when < 0.0f || when > 1.0f) {
			continue;
		}
		sawSubtickAttack = true;
		const f64 time = static_cast<f64>(command.commandNumber) + when;
		if (step.pressed) {
			data.pressTime = time;
			data.pressCommand = command.commandNumber;
		} else if (data.pressTime >= 0.0) {
			RecordHold(data, static_cast<f32>((time - data.pressTime) * tickMilliseconds), data.pressCommand);
			data.pressTime = -1.0;
		}
	}
	if (sawSubtickAttack) {
		return;
	}

	// No subtick detail: fall back to the per-tick button state.
	switch (InButtonState(command.buttonState1, command.buttonState2, command.buttonState3, IN_ATTACK)) {
	case kInButtonUpDownUp:
	case kInButtonDownUpDownUp:
		// Pressed and released inside one tick with no subtick record.
		++data.subtickLessTaps;
		RecordHold(data, 0.0f, command.commandNumber);
		data.pressTime = -1.0;
		break;
	case kInButtonUpDown:
		data.pressTime = static_cast<f64>(command.commandNumber) + 0.5;
		data.pressCommand = command.commandNumber;
		break;
	case kInButtonDownUp:
		if (data.pressTime >= 0.0) {
			RecordHold(data, static_cast<f32>((command.commandNumber + 0.5 - data.pressTime) * tickMilliseconds), data.pressCommand);
		}
		data.pressTime = -1.0;
		break;
	default:
		break;
	}
}

void TriggerPatternModule::OnCommand(PlayerRef* player, const UsercmdView& command, int currentTick, const Vector& eye, const QAngle& angles,
									 int contactTarget, bool freshAttack, int contactStartTick, bool targetDriven, bool smokeUncertain)
{
	if (!settings::BoolSetting("cs2ac_trigger_pattern_enabled", true) || !IsEligibleHuman(player) || !command.hasBase) {
		return;
	}
	auto& data = playerData[static_cast<std::size_t>(player->index)];
	TrackClicks(data, command);

	if (freshAttack) {
		data.classifiedCommand = command.commandNumber;
		data.pressClass = -1;
	}
	if (freshAttack && !smokeUncertain) {
		if (contactTarget >= 1) {
			data.pressClass = 1;
			++data.onTargetPresses;
			PushBounded(data.recentCombatPresses, true, combatWindow);
			data.dirty = true;
			if (contactStartTick >= 0) {
				const int reaction = currentTick - contactStartTick;
				if (reaction >= 0 && reaction <= reactionMaximumTicks) {
					PushBounded(targetDriven ? data.reactionsTargetDriven : data.reactionsAimDriven, reaction, reactionWindow);
					(targetDriven ? data.newReactionsTargetDriven : data.newReactionsAimDriven).push_back(reaction);
					if (DebugEnabled()) {
						ConsoleMessage("[CS2AC Trigger Pattern] %s fired %d ticks after contact (%s).", player->GetName(), reaction,
									   targetDriven ? "held angle" : "aim moved");
					}
				}
			}
		} else {
			f32 degrees = 0.0f;
			if (VisibleEnemyNearAim(player, eye, angles, nearMissDegrees, &degrees)) {
				data.pressClass = 0;
				++data.nearMissPresses;
				PushBounded(data.recentCombatPresses, false, combatWindow);
				data.dirty = true;
				if (DebugEnabled()) {
					ConsoleMessage("[CS2AC Trigger Pattern] %s fired %.2f degrees beside a visible enemy.", player->GetName(),
								   static_cast<double>(degrees));
				}
			}
		}
	}
	PairFastTap(data);
	Evaluate(player, data);
}

void TriggerPatternModule::PairFastTap(TriggerPatternPlayerData& data)
{
	if (!data.holdPending || data.pendingHoldPressCommand != data.classifiedCommand) {
		// The press was not classified (no fresh attack that command); a later tap replaces it.
		return;
	}
	data.holdPending = false;
	if (data.pendingHoldMs >= settings::FloatSetting("cs2ac_trigger_pattern_fast_tap_ms", 20.0f) || data.pressClass < 0) {
		return;
	}
	const bool onTarget = data.pressClass == 1;
	PushBounded(data.recentFastCombat, onTarget, combatWindow);
	++(onTarget ? data.fastOnTarget : data.fastNearMiss);
	data.dirty = true;
}

void TriggerPatternModule::Evaluate(PlayerRef* player, TriggerPatternPlayerData& data)
{
	if (!announce) {
		return;
	}

	if (!data.reportedClicks && data.recentTapsMs.size() >= tapMinimum) {
		const f32 fastTapMs = settings::FloatSetting("cs2ac_trigger_pattern_fast_tap_ms", 20.0f);
		const auto fast = std::count_if(data.recentTapsMs.begin(), data.recentTapsMs.end(), [&](f32 ms) { return ms < fastTapMs; });
		const f32 share = static_cast<f32>(fast) / static_cast<f32>(data.recentTapsMs.size());
		// Instant taps alone also come from attack bound to the mouse wheel (seen live). A
		// triggerbot's instant taps additionally start on the enemy almost every time.
		const auto fastOn = std::count(data.recentFastCombat.begin(), data.recentFastCombat.end(), true);
		const bool enoughCombat = data.recentFastCombat.size() >= fastCombatMinimum;
		const f32 fastOnShare = enoughCombat ? static_cast<f32>(fastOn) / static_cast<f32>(data.recentFastCombat.size()) : 0.0f;
		if (share >= settings::FloatSetting("cs2ac_trigger_pattern_fast_click_share", 0.6f) && enoughCombat && fastOnShare >= fastOnTargetShare) {
			data.reportedClicks = true;
			announce("TRIGGER PATTERN", player,
					 localization::Format(
						 "evidence.trigger_pattern.clicks",
						 "{fast} of the last {total} taps were held for less than {limit} ms, and {on} of {combat} such taps at a nearby "
						 "visible enemy started with the crosshair already on that enemy. Synthetic clicks are held for microseconds "
						 "and a triggerbot only fires on contact.",
						 {{"fast", std::to_string(fast)},
						  {"total", std::to_string(data.recentTapsMs.size())},
						  {"on", std::to_string(fastOn)},
						  {"combat", std::to_string(data.recentFastCombat.size())},
						  {"limit", std::to_string(static_cast<int>(fastTapMs))}}));
		}
	}

	if (!data.reportedDelay && data.reactionsTargetDriven.size() >= reactionMinimum) {
		const std::vector<int> values(data.reactionsTargetDriven.begin(), data.reactionsTargetDriven.end());
		const int median = TriggerPatternMedian(values);
		// Very fast reactions are the existing Triggerbot detector's job.
		if (median >= 3) {
			const auto close = std::count_if(values.begin(), values.end(), [&](int ticks) { return std::abs(ticks - median) <= 1; });
			const f32 concentration = static_cast<f32>(close) / static_cast<f32>(values.size());
			if (concentration >= settings::FloatSetting("cs2ac_trigger_pattern_delay_concentration", 0.8f)) {
				data.reportedDelay = true;
				char medianMs[16];
				std::snprintf(medianMs, sizeof(medianMs), "%.0f", static_cast<double>(median * tickMilliseconds));
				announce("TRIGGER PATTERN", player,
						 localization::Format("evidence.trigger_pattern.delay",
											  "{close} of {total} shots at enemies entering a held crosshair came {median} ms after contact, "
											  "within one tick of each other. Human reaction times vary far more.",
											  {{"close", std::to_string(close)}, {"total", std::to_string(values.size())}, {"median", medianMs}}));
			}
		}
	}

	if (!data.reportedOnTarget && data.recentCombatPresses.size() >= combatMinimum) {
		const auto onTarget = std::count(data.recentCombatPresses.begin(), data.recentCombatPresses.end(), true);
		const f32 share = static_cast<f32>(onTarget) / static_cast<f32>(data.recentCombatPresses.size());
		if (share >= settings::FloatSetting("cs2ac_trigger_pattern_on_target_share", 0.97f)) {
			data.reportedOnTarget = true;
			announce("TRIGGER PATTERN", player,
					 localization::Format("evidence.trigger_pattern.on_target",
										  "{on} of the last {total} shots at nearby visible enemies started with the crosshair already on "
										  "the enemy; none or almost none were fired just beside one.",
										  {{"on", std::to_string(onTarget)}, {"total", std::to_string(data.recentCombatPresses.size())}}));
		}
	}
}

void TriggerPatternModule::OnGameFrame()
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

void TriggerPatternModule::WriteSummary(PlayerRef* player, TriggerPatternPlayerData& data)
{
	std::string holds = "[";
	for (std::size_t index = 0; index < data.holdBuckets.size(); ++index) {
		holds += (index ? "," : "") + std::to_string(data.holdBuckets[index]);
	}
	holds += "]";
	std::string fields;
	fields += ",\"hold_buckets_ms\":\"<2,<8,<20,<40,<80,<150,<300,300+\"";
	fields += ",\"hold_counts\":" + holds;
	fields += ",\"subtickless_taps\":" + std::to_string(data.subtickLessTaps);
	fields += ",\"taps_ms\":" + FloatList(data.newTapsMs);
	fields += ",\"reaction_ticks_held\":" + IntList(data.newReactionsTargetDriven);
	fields += ",\"reaction_ticks_aimed\":" + IntList(data.newReactionsAimDriven);
	fields += ",\"on_target\":" + std::to_string(data.onTargetPresses);
	fields += ",\"near_miss\":" + std::to_string(data.nearMissPresses);
	fields += ",\"fast_on_target\":" + std::to_string(data.fastOnTarget);
	fields += ",\"fast_near_miss\":" + std::to_string(data.fastNearMiss);
	detection_log::WriteTelemetry("trigger_stats", player, fields);
	// Counters are per summary window; the rolling windows keep feeding Evaluate().
	data.holdBuckets.fill(0);
	data.subtickLessTaps = 0;
	data.onTargetPresses = 0;
	data.nearMissPresses = 0;
	data.fastOnTarget = 0;
	data.fastNearMiss = 0;
	data.newTapsMs.clear();
	data.newReactionsTargetDriven.clear();
	data.newReactionsAimDriven.clear();
	data.dirty = false;
}

} // namespace detection
} // namespace cs2ac
