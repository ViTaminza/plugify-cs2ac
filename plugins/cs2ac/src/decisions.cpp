// cs2ac — the pure decisions introduced by the fork packages v3-v11 (tick integrity, SILENTAIM
// scoring, trigger pattern, RCS, punishment policy, mass guard, repeat ban). Everything in this file
// is engine-free so tests/cs2ac_tests.cpp can pin it; the modules that use it live in the plugin
// translation units.

#include "core.hpp"

#include <cstring>

namespace cs2ac {
namespace detection {

// ---------------------------------------------------------------------------------------------
// Detection lists (fork cs2ac.cpp IsListed): comma-separated, trimmed, case-insensitive.
// ---------------------------------------------------------------------------------------------

bool ListContains(const char* list, std::string_view item)
{
	if (!list || item.empty()) {
		return false;
	}
	std::string_view rest(list);
	while (!rest.empty()) {
		const std::size_t comma = rest.find(',');
		std::string_view entry = rest.substr(0, comma);
		rest = comma == std::string_view::npos ? std::string_view {} : rest.substr(comma + 1);
		while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\t')) {
			entry.remove_prefix(1);
		}
		while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\t')) {
			entry.remove_suffix(1);
		}
		if (entry.size() == item.size() && EqualsIgnoreCase(entry, item)) {
			return true;
		}
	}
	return false;
}

bool IsReportOnlyDetection(std::string_view detection)
{
	// Statistical signals still being calibrated: never kick or ban, whatever
	// cs2ac_weak_signal_policy says. TRIGGER PATTERN left this list in v9.
	return EqualsIgnoreCase(detection, "RCS");
}

DetectionSeverity SeverityForDetection(std::string_view detection, bool kickOnly, const char* banList, const char* kickList,
									   bool weakSignalsKick)
{
	if (IsReportOnlyDetection(detection)) {
		return DetectionSeverity::Report;
	}
	if (!kickOnly && ListContains(banList, detection)) {
		return DetectionSeverity::Ban;
	}
	if (ListContains(kickList, detection)) {
		return DetectionSeverity::Kick;
	}
	return weakSignalsKick ? DetectionSeverity::Kick : DetectionSeverity::Report;
}

// ---------------------------------------------------------------------------------------------
// Rolling windows
// ---------------------------------------------------------------------------------------------

bool WindowedCountReached(std::deque<i64>& times, i64 nowSeconds, int threshold, int seconds)
{
	if (threshold <= 0 || seconds <= 0) {
		return false;
	}
	times.push_back(nowSeconds);
	while (!times.empty() && nowSeconds - times.front() > seconds) {
		times.pop_front();
	}
	return static_cast<int>(times.size()) >= threshold;
}

bool ComboReached(std::set<std::string>& kinds, std::string_view detection, const char* ignoreList, int needed)
{
	if (needed <= 0 || detection.empty() || ListContains(ignoreList, detection)) {
		return false;
	}
	kinds.insert(std::string(detection));
	return static_cast<int>(kinds.size()) >= needed;
}

bool MassGuardReached(std::deque<MassGuardHit>& hits, i64 nowSeconds, int windowSeconds, u64 steamId, int limit)
{
	if (limit <= 0 || steamId == 0) {
		return false;
	}
	const i64 window = (std::max)(1, windowSeconds);
	while (!hits.empty() && nowSeconds - hits.front().seconds > window) {
		hits.pop_front();
	}
	bool seen = false;
	for (const MassGuardHit& hit : hits) {
		seen = seen || hit.steamId == steamId;
	}
	if (!seen) {
		hits.push_back({nowSeconds, steamId});
	}
	return static_cast<int>(hits.size()) >= limit;
}

bool RepeatBanReached(std::deque<i64>& kicks, const std::unordered_map<std::string, i64>& findings, std::string_view detection,
					  const char* repeatList, const char* needsOther, i64 nowSeconds, int windowSeconds)
{
	if (detection.empty() || !ListContains(repeatList, detection) || windowSeconds <= 0) {
		return false;
	}
	while (!kicks.empty() && nowSeconds - kicks.front() > windowSeconds) {
		kicks.pop_front();
	}
	if (kicks.empty()) {
		return false;
	}
	if (!ListContains(needsOther, detection)) {
		return true;
	}
	for (const auto& [kind, seen] : findings) {
		if (nowSeconds - seen <= windowSeconds && !EqualsIgnoreCase(kind, detection)
			&& !ListContains("BHOP,NULLS,RCS", kind)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Tick integrity (fork tick_integrity_module.cpp)
// ---------------------------------------------------------------------------------------------

f64 TickRenderTime(bool hasTick, i32 tick, bool hasFraction, f32 fraction)
{
	if (!hasTick) {
		return kTickNone;
	}
	const f64 frac = hasFraction && std::isfinite(fraction) ? static_cast<f64>(fraction) : 0.0;
	const f64 value = static_cast<f64>(tick) + frac;
	return value > 0.0 ? value : kTickNone;
}

f64 TickInterpTime(const InterpInfo& interp)
{
	if (!interp.hasSrcTick) {
		return kTickNone;
	}
	const f64 src = interp.srcTick;
	const f64 dst = interp.hasDstTick ? interp.dstTick : src;
	const f64 fraction = interp.hasFrac && std::isfinite(interp.frac) ? interp.frac : 0.0;
	const f64 value = src + fraction * (dst - src);
	return value > 0.0 ? value : kTickNone;
}

f64 TickDip(f64 previous, f64 shot, f64 next)
{
	if (previous <= 0.0 || shot <= 0.0 || next <= 0.0) {
		return std::numeric_limits<f64>::quiet_NaN();
	}
	return (std::min)(previous - shot, next - shot);
}

bool TickHasFraction(f64 time)
{
	return time > 0.0 && std::fabs(time - std::round(time)) > 1e-3;
}

bool TickRewriteFingerprint(bool wholeTick, f64 interpTime)
{
	// A rewriting cheat sets the shot entry to a whole older tick and drops its interpolation info.
	return wholeTick && interpTime <= 0.0;
}

std::size_t TickDipBucket(f32 dipTicks)
{
	return dipTicks < 0.5f ? 0 : dipTicks < 1.0f ? 1 : dipTicks < 2.0f ? 2 : dipTicks < 4.0f ? 3 : 4;
}

std::size_t TickInterpOffsetBucket(f32 offsetTicks)
{
	return offsetTicks < 1.0f ? 0 : offsetTicks < 2.0f ? 1 : offsetTicks < 3.0f ? 2 : offsetTicks < 4.0f ? 3 : offsetTicks < 6.0f ? 4 : 5;
}

std::size_t TickExcessBucket(f32 value, const f32* limits, std::size_t count)
{
	for (std::size_t index = 0; index < count; ++index) {
		if (value < limits[index]) {
			return index;
		}
	}
	return count;
}

bool BacktrackDipCounts(f32 dipTicks, f32 minTicks, f64 gap, f32 maxGap, bool forged, bool requireForged, bool previousFraction,
						bool nextFraction)
{
	if (dipTicks < minTicks) {
		return false;
	}
	// A jump of the client's clock around the shot is a stall, not a rewrite (v3/v5).
	if (!(gap >= 0.0) || gap > maxGap) {
		return false;
	}
	const bool fingerprint = forged && previousFraction && nextFraction;
	if (!fingerprint && requireForged) {
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// SILENTAIM (fork v7/v7.1)
// ---------------------------------------------------------------------------------------------

int SilentAimPoints(f32 excess, f32 cameraTurn, bool airborne, bool headshot, bool wallbang, bool smokeBonus, bool noscope)
{
	const bool blatant = excess > kSilentAimBlatantExcess;
	const int basePoints = (blatant ? 4 : airborne ? 3 : 2) + static_cast<int>(headshot) + 2 * static_cast<int>(wallbang)
						   + 2 * static_cast<int>(smokeBonus) + static_cast<int>(noscope);
	const bool cameraKnown = std::isfinite(cameraTurn) && cameraTurn >= 0.0f;
	const bool stillCamera = cameraKnown && cameraTurn < kSilentAimStillCameraTurn;
	const bool flickMismatch = cameraKnown && !stillCamera && excess < kSilentAimFlickHalfExcess;
	if (excess < kSilentAimMinimumCountedExcess) {
		return 0; // v7.1: measurement noise still logged, never scored
	}
	if (stillCamera) {
		return basePoints + 1;
	}
	if (flickMismatch) {
		return (basePoints + 1) / 2;
	}
	return basePoints;
}

std::size_t SilentAimExcessBucket(f32 excess)
{
	constexpr f32 limits[] = {0.25f, 0.5f, 1.0f, 2.0f, 5.0f, kSilentAimBlatantExcess};
	return TickExcessBucket(excess, limits, std::size(limits));
}

std::size_t SilentAimCameraBucket(f32 cameraTurn)
{
	constexpr f32 limits[] = {0.5f, 2.0f, 5.0f, 15.0f};
	if (!std::isfinite(cameraTurn) || cameraTurn < 0.0f) {
		return 5; // unknown
	}
	return TickExcessBucket(cameraTurn, limits, std::size(limits));
}

// ---------------------------------------------------------------------------------------------
// TRIGGER PATTERN (fork trigger_pattern_module.cpp)
// ---------------------------------------------------------------------------------------------

std::size_t TriggerPatternHoldBucket(f32 milliseconds)
{
	constexpr f32 limits[] = {2.0f, 8.0f, 20.0f, 40.0f, 80.0f, 150.0f, 300.0f};
	for (std::size_t index = 0; index < std::size(limits); ++index) {
		if (milliseconds < limits[index]) {
			return index;
		}
	}
	return std::size(limits);
}

int TriggerPatternMedian(std::vector<int> values)
{
	if (values.empty()) {
		return 0;
	}
	std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
	return values[values.size() / 2];
}

// ---------------------------------------------------------------------------------------------
// RCS (fork recoil_control_module.cpp)
// ---------------------------------------------------------------------------------------------

bool IsAutomaticWeapon(std::string_view weapon_name)
{
	weapon_name = NormalizeWeapon(weapon_name);
	static constexpr std::string_view weapons[] = {
		"ak47", "aug", "bizon", "cz75a", "famas", "galilar", "m249",  "m4a1",  "m4a1_silencer",
		"mac10", "mp5sd", "mp7", "mp9",  "negev", "p90",     "sg556", "ump45",
	};
	return std::find(std::begin(weapons), std::end(weapons), weapon_name) != std::end(weapons);
}

f32 RecoilAngleDelta(f32 to, f32 from)
{
	return std::remainder(to - from, 360.0f);
}

f32 RecoilCorrelation(const std::vector<f32>& x, const std::vector<f32>& y)
{
	const std::size_t count = (std::min)(x.size(), y.size());
	if (count < 4) {
		return 0.0f;
	}
	f64 meanX = 0.0;
	f64 meanY = 0.0;
	for (std::size_t index = 0; index < count; ++index) {
		meanX += x[index];
		meanY += y[index];
	}
	meanX /= static_cast<f64>(count);
	meanY /= static_cast<f64>(count);
	f64 covariance = 0.0;
	f64 varianceX = 0.0;
	f64 varianceY = 0.0;
	for (std::size_t index = 0; index < count; ++index) {
		const f64 dx = x[index] - meanX;
		const f64 dy = y[index] - meanY;
		covariance += dx * dy;
		varianceX += dx * dx;
		varianceY += dy * dy;
	}
	if (varianceX < 1e-9 || varianceY < 1e-9) {
		return 0.0f;
	}
	return static_cast<f32>(covariance / std::sqrt(varianceX * varianceY));
}

bool RecoilSprayQualifies(bool invalid, int maxShots, std::size_t samples, std::string_view weapon)
{
	constexpr int minimumShots = 8;
	constexpr std::size_t minimumSamples = 16;
	return !invalid && maxShots >= minimumShots && samples >= minimumSamples && IsAutomaticWeapon(weapon);
}

bool RecoilSuspiciousShare(const std::deque<RecoilSprayResult>& recent, f32 threshold, f32 suspiciousShare)
{
	constexpr std::size_t minimumSprays = 10;
	constexpr int suspiciousMaximumLag = 2;
	constexpr f32 suspiciousMinimumGain = 0.5f;
	constexpr f32 suspiciousMaximumGain = 1.3f;
	if (recent.size() < minimumSprays) {
		return false;
	}
	std::size_t machineLike = 0;
	for (const RecoilSprayResult& spray : recent) {
		if (spray.bestCorrelation >= threshold && spray.bestLag >= 0 && spray.bestLag <= suspiciousMaximumLag && spray.gain >= suspiciousMinimumGain
			&& spray.gain <= suspiciousMaximumGain) {
			++machineLike;
		}
	}
	return static_cast<f32>(machineLike) / static_cast<f32>(recent.size()) >= suspiciousShare;
}

// ---------------------------------------------------------------------------------------------
// JSON (fork detection_log.cpp JsonString)
// ---------------------------------------------------------------------------------------------

std::string JsonString(const char* value)
{
	std::string out = "\"";
	for (const unsigned char* current = reinterpret_cast<const unsigned char*>(value ? value : ""); *current; ++current) {
		switch (*current) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (*current < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(*current));
				out += buffer;
			} else {
				out += static_cast<char>(*current);
			}
			break;
		}
	}
	return out + "\"";
}

} // namespace detection
} // namespace cs2ac
