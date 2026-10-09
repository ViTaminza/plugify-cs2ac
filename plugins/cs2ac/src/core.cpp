// cs2ac — engine-free core: math, string helpers, the usercmd protobuf reader and the pure
// scoring helpers the unit tests exercise.
//
// Ported from ref/src/cs2ac-legacy/src/utils/utils.cpp, detection/detection_system.cpp (the
// geometry helpers), movement_analysis/detection/detectors/{nulls,subtick,strafe_optimizer}.cpp
// and the scoring constants of the detection modules. Nothing here may call into s2sdk.

#include "core.hpp"

#include <cctype>
#include <charconv>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace cs2ac {

// ---------------------------------------------------------------------------------------------
// Math (utils/utils.cpp)
// ---------------------------------------------------------------------------------------------

f32 NormalizeDeg(f32 value)
{
	// fmod instead of the original's subtract-until-in-range loops: a client can send an absurd
	// (but finite) angle, and the loop would then take effectively forever on the game thread.
	if (!std::isfinite(value)) {
		return value;
	}
	f32 normalized = std::fmod(value, 360.0f);
	if (normalized > 180.0f) {
		normalized -= 360.0f;
	} else if (normalized < -180.0f) {
		normalized += 360.0f;
	}
	return normalized;
}

f32 GetAngleDifference(f32 source, f32 target, f32 circle, bool relative)
{
	f32 difference = std::fmod(target - source, circle * 2.0f);
	if (difference > circle) {
		difference -= circle * 2.0f;
	} else if (difference < -circle) {
		difference += circle * 2.0f;
	}
	return relative ? difference : std::fabs(difference);
}

// ---------------------------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------------------------

void ReplaceAll(std::string& value, const std::string_view placeholder, const std::string_view replacement)
{
	if (placeholder.empty()) {
		return;
	}
	for (size_t position = 0; (position = value.find(placeholder, position)) != std::string::npos; position += replacement.size()) {
		value.replace(position, placeholder.size(), replacement);
	}
}

std::string SanitizeConsoleText(const char* value)
{
	std::string safe = value ? value : "<unknown>";
	for (char& character : safe) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte < 32 || byte == 127) {
			character = '?';
		}
	}
	return safe;
}

bool IsNumeric(const char* str)
{
	if (!str || !*str) {
		return false;
	}
	char* end = nullptr;
	const double value = std::strtod(str, &end);
	return end && *end == '\0' && std::isfinite(value);
}

std::string FormatStringV(const char* format, va_list arguments)
{
	va_list copy;
	va_copy(copy, arguments);
	const int needed = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);
	if (needed <= 0) {
		return {};
	}
	std::string result(static_cast<size_t>(needed), '\0');
	va_list second;
	va_copy(second, arguments);
	std::vsnprintf(result.data(), result.size() + 1, format, second);
	va_end(second);
	return result;
}

std::string FormatString(const char* format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	std::string result = FormatStringV(format, arguments);
	va_end(arguments);
	return result;
}

std::string ToLower(std::string value)
{
	for (char& character : value) {
		character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}
	return value;
}

std::string ToUpper(std::string value)
{
	for (char& character : value) {
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	return value;
}

bool EqualsIgnoreCase(std::string_view first, std::string_view second)
{
	if (first.size() != second.size()) {
		return false;
	}
	for (size_t index = 0; index < first.size(); ++index) {
		if (std::tolower(static_cast<unsigned char>(first[index])) != std::tolower(static_cast<unsigned char>(second[index]))) {
			return false;
		}
	}
	return true;
}

namespace {
char ConvertColorStringToByte(const char* str, size_t length)
{
	switch (length) {
	case 3:
		if (!std::memcmp(str, "red", length)) {
			return 7;
		}
		break;
	case 4:
		if (!std::memcmp(str, "lime", length)) {
			return 6;
		}
		if (!std::memcmp(str, "grey", length)) {
			return 8;
		}
		break;
	case 7:
		if (!std::memcmp(str, "default", length)) {
			return 1;
		}
		break;
	}
	return 0;
}
} // namespace

// utils/utils_print.cpp utils::CFormat (byte-for-byte the original).
bool CFormat(char* buffer, u64 bufferSize, const char* text)
{
	if (!buffer || bufferSize == 0 || !text) {
		return false;
	}
	char* result = buffer;
	char* const resultEnd = buffer + bufferSize;
	const auto hasEnoughSpace = [&](uintptr_t space) { return static_cast<uintptr_t>(resultEnd - result) > space; };
	if (!hasEnoughSpace(1)) {
		return false;
	}
	*result++ = ' ';
	const char* current = text;
	while (*current) {
		if (*current == '{' && *(current + 1) == '{') {
			if (!hasEnoughSpace(1)) {
				return false;
			}
			current += 2;
			*result++ = '{';
			continue;
		}
		if (*current == '{') {
			const char* start = current + 1;
			const char* end = start;
			while (*end && *end != '}') {
				++end;
			}
			if (*end == '}') {
				const char color = ConvertColorStringToByte(start, static_cast<size_t>(end - start));
				if (color) {
					if (!hasEnoughSpace(1)) {
						return false;
					}
					*result++ = color;
					current = end + 1;
					continue;
				}
			}
			// Not a colour tag: fall through and copy the '{' literally.
		}
		if (!hasEnoughSpace(1)) {
			return false;
		}
		*result++ = *current++;
	}
	if (!hasEnoughSpace(1)) {
		return false;
	}
	*result = '\0';
	return true;
}

// ---------------------------------------------------------------------------------------------
// Usercmd protobuf reader
//
// CSGOUserCmdPB (cs_usercmd.proto) embedded at PlayerCommand+16 (sdk/usercmd.h: vptr + cmdNum +
// 4 unknown bytes, then the message). Field numbers are the wire values of the current layout:
//   CSGOUserCmdPB            base=1  input_history=2  attack1_start_history_index=6
//   CBaseUserCmdPB           command_number=1 client_tick=2 buttons_pb=3 viewangles=4 mousedx=11
//                            mousedy=12 subtick_moves=18
//   CSubtickMoveStep         button=1 pressed=2 when=3 analog_forward_delta=4 analog_left_delta=5
//                            pitch_delta=6 yaw_delta=7
//   CSGOInputHistoryEntryPB  view_angles=2
//   CInButtonStatePB         buttonstate1=1 buttonstate2=2 buttonstate3=3
//   CMsgQAngle               x=1 y=2 z=3
// The reader reports what it actually saw (UsercmdView::fieldsSeen) so cs2ac_usercmd_dump can be
// used to confirm the layout on a live server; unknown fields are skipped by wire type.
// ---------------------------------------------------------------------------------------------

namespace {

// bits of UsercmdView::fieldsSeen
enum : u32 {
	kSeenBase = 1u << 0,
	kSeenClientTick = 1u << 1,
	kSeenViewAngles = 1u << 2,
	kSeenButtons = 1u << 3,
	kSeenSubtick = 1u << 4,
	kSeenHistory = 1u << 5,
	kSeenAttackIndex = 1u << 6,
	kSeenMouse = 1u << 7,
};

class ProtoReader {
public:
	ProtoReader(const uint8_t* data, size_t size) : cursor(data), end(data + size) {}

	[[nodiscard]] bool Eof() const { return cursor >= end; }
	[[nodiscard]] size_t Remaining() const { return static_cast<size_t>(end - cursor); }

	bool Varint(u64& value)
	{
		value = 0;
		for (int shift = 0; shift <= 63; shift += 7) {
			if (cursor >= end) {
				return false;
			}
			const uint8_t byte = *cursor++;
			value |= static_cast<u64>(byte & 0x7f) << shift;
			if ((byte & 0x80) == 0) {
				return true;
			}
		}
		return false;
	}

	bool Bytes(const uint8_t*& data, size_t& length)
	{
		u64 size = 0;
		if (!Varint(size) || size > Remaining()) {
			return false;
		}
		data = cursor;
		length = static_cast<size_t>(size);
		cursor += size;
		return true;
	}

	bool Fixed32(u32& value)
	{
		if (Remaining() < 4) {
			return false;
		}
		std::memcpy(&value, cursor, 4);
		cursor += 4;
		return true;
	}

	bool Float(f32& value)
	{
		u32 raw = 0;
		if (!Fixed32(raw)) {
			return false;
		}
		std::memcpy(&value, &raw, 4);
		return true;
	}

	bool Skip(u32 wireType)
	{
		switch (wireType) {
		case 0: {
			u64 ignored = 0;
			return Varint(ignored);
		}
		case 1:
			if (Remaining() < 8) {
				return false;
			}
			cursor += 8;
			return true;
		case 2: {
			const uint8_t* ignoredData = nullptr;
			size_t ignoredLength = 0;
			return Bytes(ignoredData, ignoredLength);
		}
		case 5:
			if (Remaining() < 4) {
				return false;
			}
			cursor += 4;
			return true;
		default:
			return false; // groups / invalid
		}
	}

private:
	const uint8_t* cursor;
	const uint8_t* end;
};

bool ParseQAngle(const uint8_t* data, size_t size, QAngle& out)
{
	ProtoReader reader(data, size);
	out = {};
	while (!reader.Eof()) {
		u64 tag = 0;
		if (!reader.Varint(tag)) {
			return false;
		}
		const u32 field = static_cast<u32>(tag >> 3);
		const u32 wireType = static_cast<u32>(tag & 0x7);
		if (wireType == 5) {
			f32 value = 0.0f;
			if (!reader.Float(value)) {
				return false;
			}
			if (field == 1) {
				out.x = value;
			} else if (field == 2) {
				out.y = value;
			} else if (field == 3) {
				out.z = value;
			}
		} else if (!reader.Skip(wireType)) {
			return false;
		}
	}
	return true;
}

bool ParseButtons(const uint8_t* data, size_t size, UsercmdView& view)
{
	ProtoReader reader(data, size);
	while (!reader.Eof()) {
		u64 tag = 0;
		if (!reader.Varint(tag)) {
			return false;
		}
		const u32 field = static_cast<u32>(tag >> 3);
		const u32 wireType = static_cast<u32>(tag & 0x7);
		if (wireType == 0) {
			u64 value = 0;
			if (!reader.Varint(value)) {
				return false;
			}
			if (field == 1) {
				view.buttonState1 = value;
			} else if (field == 2) {
				view.buttonState2 = value;
			} else if (field == 3) {
				view.buttonState3 = value;
			}
		} else if (!reader.Skip(wireType)) {
			return false;
		}
	}
	view.fieldsSeen |= kSeenButtons;
	return true;
}

bool ParseSubtickMove(const uint8_t* data, size_t size, SubtickMove& move)
{
	ProtoReader reader(data, size);
	while (!reader.Eof()) {
		u64 tag = 0;
		if (!reader.Varint(tag)) {
			return false;
		}
		const u32 field = static_cast<u32>(tag >> 3);
		const u32 wireType = static_cast<u32>(tag & 0x7);
		switch (wireType) {
		case 0: {
			u64 value = 0;
			if (!reader.Varint(value)) {
				return false;
			}
			if (field == 1) {
				move.hasButton = true;
				move.button = value;
			} else if (field == 2) {
				move.pressed = value != 0;
			}
			break;
		}
		case 5: {
			f32 value = 0.0f;
			if (!reader.Float(value)) {
				return false;
			}
			switch (field) {
			case 3:
				move.when = value;
				break;
			case 4:
				move.hasAnalogForwardDelta = true;
				move.analogForwardDelta = value;
				break;
			case 5:
				move.hasAnalogLeftDelta = true;
				move.analogLeftDelta = value;
				break;
			case 6:
				move.hasPitchDelta = true;
				move.pitchDelta = value;
				break;
			case 7:
				move.hasYawDelta = true;
				move.yawDelta = value;
				break;
			default:
				break;
			}
			break;
		}
		default:
			if (!reader.Skip(wireType)) {
				return false;
			}
			break;
		}
	}
	return true;
}

bool ParseBaseUserCmd(const uint8_t* data, size_t size, UsercmdView& view)
{
	ProtoReader reader(data, size);
	bool ok = true;
	while (!reader.Eof() && ok) {
		u64 tag = 0;
		if (!reader.Varint(tag)) {
			return false;
		}
		const u32 field = static_cast<u32>(tag >> 3);
		const u32 wireType = static_cast<u32>(tag & 0x7);
		if (wireType == 0) {
			u64 value = 0;
			if (!reader.Varint(value)) {
				return false;
			}
			if (field == 1) {
				view.commandNumber = static_cast<i32>(value);
			} else if (field == 2) {
				view.hasClientTick = true;
				view.clientTick = static_cast<i32>(value);
				view.fieldsSeen |= kSeenClientTick;
			} else if (field == 11) {
				view.hasMouseDx = true;
				view.mouseDx = static_cast<i32>(value);
				view.fieldsSeen |= kSeenMouse;
			} else if (field == 12) {
				view.hasMouseDy = true;
				view.mouseDy = static_cast<i32>(value);
				view.fieldsSeen |= kSeenMouse;
			}
		} else if (wireType == 2) {
			const uint8_t* payload = nullptr;
			size_t length = 0;
			if (!reader.Bytes(payload, length)) {
				return false;
			}
			if (field == 3) {
				ok = ParseButtons(payload, length, view);
			} else if (field == 4) {
				QAngle angles;
				if (ParseQAngle(payload, length, angles)) {
					view.hasViewAngles = true;
					view.viewAngles = angles;
					view.fieldsSeen |= kSeenViewAngles;
				}
			} else if (field == 18) {
				SubtickMove move;
				if (ParseSubtickMove(payload, length, move)) {
					view.subtickMoves.push_back(move);
					view.fieldsSeen |= kSeenSubtick;
				}
			}
		} else if (!reader.Skip(wireType)) {
			return false;
		}
	}
	view.hasBase = true;
	view.fieldsSeen |= kSeenBase;
	return true;
}

} // namespace

bool ParseUserCmdMessage(const void* message, size_t available, UsercmdView& view)
{
	view = UsercmdView {};
	if (!message || available == 0) {
		return false;
	}
	// The reader is bounded by the caller's byte count: a message shorter than the object it is
	// embedded in stops at the invalid tag it meets next, and a message at the very end of an array
	// can never read past it.
	const auto* data = static_cast<const uint8_t*>(message);
	ProtoReader reader(data, available);
	bool ok = true;
	while (!reader.Eof() && ok) {
		u64 tag = 0;
		if (!reader.Varint(tag)) {
			break; // ran into padding after the message: stop, keep what we parsed
		}
		const u32 field = static_cast<u32>(tag >> 3);
		const u32 wireType = static_cast<u32>(tag & 0x7);
		if (wireType == 0) {
			u64 value = 0;
			if (!reader.Varint(value)) {
				break;
			}
			if (field == 6) {
				view.attack1StartHistoryIndex = static_cast<i32>(value);
				view.hasAttack1StartHistoryIndex = true;
				view.fieldsSeen |= kSeenAttackIndex;
			}
		} else if (wireType == 2) {
			const uint8_t* payload = nullptr;
			size_t length = 0;
			if (!reader.Bytes(payload, length)) {
				break;
			}
			if (field == 1) {
				ok = ParseBaseUserCmd(payload, length, view);
			} else if (field == 2) {
				InputHistoryEntry entry;
				// CSGOInputHistoryEntryPB.view_angles = 2
				ProtoReader history(payload, length);
				while (!history.Eof()) {
					u64 innerTag = 0;
					if (!history.Varint(innerTag)) {
						break;
					}
					const u32 innerField = static_cast<u32>(innerTag >> 3);
					const u32 innerWire = static_cast<u32>(innerTag & 0x7);
					if (innerField == 2 && innerWire == 2) {
						const uint8_t* anglesPayload = nullptr;
						size_t anglesLength = 0;
						if (!history.Bytes(anglesPayload, anglesLength)) {
							break;
						}
						QAngle angles;
						if (ParseQAngle(anglesPayload, anglesLength, angles)) {
							entry.hasViewAngles = true;
							entry.viewAngles = angles;
						}
					} else if (!history.Skip(innerWire)) {
						break;
					}
				}
				view.inputHistory.push_back(entry);
				view.fieldsSeen |= kSeenHistory;
			}
		} else if (!reader.Skip(wireType)) {
			break;
		}
	}
	return view.hasBase;
}

namespace cvar {

bool IsValidHandle(u64 handle)
{
	if (handle == 0) {
		return false;
	}
	if (handle == std::numeric_limits<u64>::max()) {
		return false;
	}
	// The engine's ConVarRef is a 32-bit handle; 0xFFFFFFFF means "no convar" whichever way it was
	// widened into the 64-bit value s2sdk passes around.
	if (static_cast<u32>(handle) == static_cast<u32>(kInvalidHandle)) {
		return false;
	}
	return true;
}

bool BoolFromString(const char* value)
{
	// The engine's own truth spellings (convars accept these for a bool cvar).
	return value != nullptr
		   && (std::strcmp(value, "1") == 0 || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "yes"));
}

} // namespace cvar

namespace engine {

bool PlausibleUserId(i64 value)
{
	// 0 means "no player" in the engine's events; real user ids are small positive integers
	// (CS2 counts them up per connection and they are what `kick #<userid>` accepts).
	return value >= 1 && value <= 0xFFFF;
}

} // namespace engine

namespace usercmd {

bool IsPlausibleCommandNumber(i32 number)
{
	return number >= 0 && number <= kMaxCommandNumber;
}

bool IsStrictlyIncreasing(const std::vector<i32>& numbers)
{
	for (std::size_t index = 1; index < numbers.size(); ++index) {
		if (numbers[index] <= numbers[index - 1]) {
			return false;
		}
	}
	return true;
}

} // namespace usercmd

namespace movement {

f32 JumpEventTime(i32 tick, f32 fraction)
{
	return static_cast<f32>(tick) + fraction;
}

bool IsFramePerfectJump(i32 pressTick, f32 pressFraction, i32 landTick, f32 landFraction)
{
	return JumpEventTime(pressTick, pressFraction) < JumpEventTime(landTick, landFraction);
}

} // namespace movement

// ---------------------------------------------------------------------------------------------
// Pure detection helpers (detection/*.cpp + movement_analysis/detection/detectors/*.cpp)
// ---------------------------------------------------------------------------------------------

namespace detection {

namespace {
// detection_system.cpp cachedTeammatesAreEnemies: set from mp_teammates_are_enemies by the plugin
// layer (the core stays engine-free).
bool s_teammatesAreEnemies {};
} // namespace

void SetTeammatesAreEnemies(bool value)
{
	s_teammatesAreEnemies = value;
}

bool TeammatesAreEnemies()
{
	return s_teammatesAreEnemies;
}

bool AreOpponents(int firstTeam, int secondTeam)
{
	const bool firstIsPlaying = firstTeam == CS_TEAM_T || firstTeam == CS_TEAM_CT;
	const bool secondIsPlaying = secondTeam == CS_TEAM_T || secondTeam == CS_TEAM_CT;
	return firstIsPlaying && secondIsPlaying && (firstTeam != secondTeam || s_teammatesAreEnemies);
}

std::string_view NormalizeWeapon(std::string_view weapon)
{
	if (weapon.rfind("weapon_", 0) == 0) {
		weapon.remove_prefix(7);
	}
	return weapon;
}

bool IsBallisticWeapon(std::string_view weapon)
{
	weapon = NormalizeWeapon(weapon);
	static constexpr std::string_view weapons[] = {
		"ak47",    "aug",      "awp",      "bizon",         "cz75a", "deagle", "elite", "famas", "fiveseven", "g3sg1",        "galilar", "glock",
		"hkp2000", "m249",     "m4a1",     "m4a1_silencer", "mac10", "mag7",   "mp5sd", "mp7",   "mp9",       "negev",        "nova",    "p250",
		"p90",     "revolver", "sawedoff", "scar20",        "sg556", "ssg08",  "taser", "tec9",  "ump45",     "usp_silencer", "xm1014",
	};
	return std::find(std::begin(weapons), std::end(weapons), weapon) != std::end(weapons);
}

bool IsFinite(const QAngle& angles)
{
	return std::isfinite(angles.x) && std::isfinite(angles.y) && std::isfinite(angles.z);
}

bool IsFinite(const Vector& vector)
{
	return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

Vector AimForward(const QAngle& angles)
{
	constexpr float degreesToRadians = static_cast<float>(M_PI / 180.0);
	const float pitch = angles.x * degreesToRadians;
	const float yaw = angles.y * degreesToRadians;
	const float cosinePitch = std::cos(pitch);
	return {cosinePitch * std::cos(yaw), cosinePitch * std::sin(yaw), -std::sin(pitch)};
}

f32 AngularDistance(const QAngle& first, const QAngle& second)
{
	if (!IsFinite(first) || !IsFinite(second)) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	const float dot = std::clamp(DotProduct(AimForward(first), AimForward(second)), -1.0f, 1.0f);
	return static_cast<float>(std::acos(dot) * (180.0 / M_PI));
}

f32 YawDelta(f32 from, f32 to)
{
	return std::remainder(to - from, 360.0f);
}

f32 LerpF(f32 t, f32 minimum, f32 maximum)
{
	return minimum + (maximum - minimum) * t;
}

u32 LerpU32(f32 t, u32 minimum, u32 maximum)
{
	return static_cast<u32>(static_cast<f32>(minimum) + (static_cast<f32>(maximum) - static_cast<f32>(minimum)) * t);
}

// network_safety.cpp ShouldVeto
bool ShouldVeto(f32 ping, f32 jitter, f32 incomingLoss, f32 outgoingLoss, f32 incomingChoke, f32 outgoingChoke, int unavailableSamples)
{
	constexpr f32 maximumPingMilliseconds = 150.0f;
	constexpr f32 maximumJitterMilliseconds = 25.0f;
	constexpr f32 maximumLoss = 0.02f;
	constexpr f32 maximumChoke = 0.02f;
	return ping >= maximumPingMilliseconds || jitter >= maximumJitterMilliseconds || incomingLoss >= maximumLoss || outgoingLoss >= maximumLoss
		   || incomingChoke >= maximumChoke || outgoingChoke >= maximumChoke || unavailableSamples > 0;
}

// nulls.cpp required-perfect-cstrafes scaling.
namespace {
constexpr f32 kFpsForMinimumSuspicion = 64.0f;
constexpr f32 kFpsForMaximumSuspicion = 256.0f;
constexpr u32 kConsecutiveMinimum = 128;
constexpr u32 kConsecutiveMaximum = 640;
constexpr f32 kUnderlapMedianForgivenessThreshold = 0.02f;
constexpr f32 kUnderlapPercentageThreshold = 0.1f;
} // namespace

u32 NullsRequiredPerfectCstrafes(const NullsTiming& timing, std::size_t eventCount)
{
	if (eventCount == 0) {
		return kConsecutiveMaximum;
	}
	f32 medianFramerate = timing.medianFramerate;
	if (timing.maxFps != 0.0f) {
		medianFramerate = (std::max)(medianFramerate, 1.0f / timing.maxFps);
	}
	if (medianFramerate == 0.0f) {
		medianFramerate = ENGINE_FIXED_TICK_INTERVAL;
	}
	const f32 ratio = std::clamp((1.0f / medianFramerate - kFpsForMinimumSuspicion) / (kFpsForMaximumSuspicion - kFpsForMinimumSuspicion), 0.0f, 1.0f);
	return LerpU32(1.0f - ratio, kConsecutiveMinimum, kConsecutiveMaximum);
}

bool NullsUnderlapForgiven(const NullsTiming& timing)
{
	if (timing.total == 0) {
		return false;
	}
	return timing.underlapMedian >= kUnderlapMedianForgivenessThreshold
		   && (static_cast<f32>(timing.underlapCount) / static_cast<f32>(timing.total)) >= kUnderlapPercentageThreshold;
}

u32 NullsAdjustedRequiredPerfectCstrafes(u32 required, f32 underlapMedian)
{
	const f32 underlapRatio = std::clamp(underlapMedian / kUnderlapMedianForgivenessThreshold, 0.0f, 1.0f);
	return LerpU32(underlapRatio * underlapRatio, required, kConsecutiveMaximum);
}

localization::Text AddNetworkSafetyDetails(localization::Text details, const NetworkSafetyEvidence& evidence)
{
	const auto network = localization::Format("evidence.network_check",
											  "The five-second network check was not stable enough for punishment. It measured {ping} ms ping, "
											  "{jitter} ms jitter, {incoming_loss}/{outgoing_loss}% incoming/outgoing loss, "
											  "{incoming_choke}/{outgoing_choke}% incoming/outgoing choke, {gaps} command gaps, and "
											  "{unavailable} unavailable samples.",
											  {{"ping", FormatString("%.1f", static_cast<double>(evidence.pingMilliseconds))},
											   {"jitter", FormatString("%.1f", static_cast<double>(evidence.jitterMilliseconds))},
											   {"incoming_loss", FormatString("%.1f", static_cast<double>(evidence.incomingLoss * 100.0f))},
											   {"outgoing_loss", FormatString("%.1f", static_cast<double>(evidence.outgoingLoss * 100.0f))},
											   {"incoming_choke", FormatString("%.1f", static_cast<double>(evidence.incomingChoke * 100.0f))},
											   {"outgoing_choke", FormatString("%.1f", static_cast<double>(evidence.outgoingChoke * 100.0f))},
											   {"gaps", FormatString("%d", evidence.commandGaps)},
											   {"unavailable", FormatString("%d", evidence.unavailableSamples)}});
	return {details.english + " " + network.english, details.localized + " " + network.localized};
}

} // namespace detection
} // namespace cs2ac
