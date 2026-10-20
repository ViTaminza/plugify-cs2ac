// cs2ac unit tests — the pure detection math, the scoring rules the original asserted at compile
// time, the cs2ac.cfg parser, the phrase loader and the usercmd protobuf reader.
//
// Everything here is engine-free (cs2ac_core); run by ./test.sh after ./build.sh cs2ac_tests.

#include "core.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace cs2ac;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line)
{
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL(%d): %s\n", line, what);
	}
}

#define CHECK(condition) Check((condition), #condition, __LINE__)

void CheckNear(float value, float expected, float tolerance, const char* what, int line)
{
	++g_checks;
	if (!(std::fabs(value - expected) <= tolerance)) {
		++g_failures;
		std::printf("FAIL(%d): %s: got %.6f, expected %.6f (+-%.6f)\n", line, what, static_cast<double>(value), static_cast<double>(expected),
					static_cast<double>(tolerance));
	}
}

#define CHECK_NEAR(value, expected, tolerance) CheckNear((value), (expected), (tolerance), #value, __LINE__)

// ---------------------------------------------------------------------------------------------
// Angle math (utils/utils.cpp + detection_system.cpp)
// ---------------------------------------------------------------------------------------------

void TestAngleMath()
{
	CHECK_NEAR(NormalizeDeg(190.0f), -170.0f, 0.0001f);
	CHECK_NEAR(NormalizeDeg(-190.0f), 170.0f, 0.0001f);
	CHECK_NEAR(NormalizeDeg(45.0f), 45.0f, 0.0001f);
	// A client can send absurd angles: the helper must stay O(1) (the original subtracted 360 in a
	// loop) and must not turn non-finite input into an infinite loop.
	CHECK_NEAR(NormalizeDeg(1.0e30f), NormalizeDeg(std::fmod(1.0e30f, 360.0f)), 0.0001f);
	CHECK_NEAR(NormalizeDeg(-1.0e30f), NormalizeDeg(std::fmod(-1.0e30f, 360.0f)), 0.0001f);
	CHECK(std::isinf(NormalizeDeg(INFINITY)));
	CHECK(std::isnan(NormalizeDeg(NAN)));

	CHECK_NEAR(GetAngleDifference(10.0f, 350.0f, 180.0f), 20.0f, 0.0001f);
	CHECK_NEAR(GetAngleDifference(10.0f, 350.0f, 180.0f, true), -20.0f, 0.0001f);

	// AimForward: yaw 0 looks down +X, yaw 90 down +Y, pitch -90 straight up.
	const Vector forward = detection::AimForward(QAngle(0.0f, 0.0f, 0.0f));
	CHECK_NEAR(forward.x, 1.0f, 0.0001f);
	CHECK_NEAR(forward.y, 0.0f, 0.0001f);
	CHECK_NEAR(forward.z, 0.0f, 0.0001f);
	const Vector up = detection::AimForward(QAngle(-90.0f, 0.0f, 0.0f));
	CHECK_NEAR(up.z, 1.0f, 0.0001f);

	// AngularDistance is the angle between the two view directions.
	CHECK_NEAR(detection::AngularDistance(QAngle(0.0f, 0.0f, 0.0f), QAngle(0.0f, 0.0f, 0.0f)), 0.0f, 0.0001f);
	CHECK_NEAR(detection::AngularDistance(QAngle(0.0f, 0.0f, 0.0f), QAngle(0.0f, 90.0f, 0.0f)), 90.0f, 0.001f);
	CHECK_NEAR(detection::AngularDistance(QAngle(0.0f, 0.0f, 0.0f), QAngle(0.0f, 180.0f, 0.0f)), 180.0f, 0.001f);
	CHECK(std::isnan(detection::AngularDistance(QAngle(NAN, 0.0f, 0.0f), QAngle(0.0f, 0.0f, 0.0f))));

	// antiaim_module.cpp YawDelta
	CHECK_NEAR(detection::YawDelta(350.0f, 10.0f), 20.0f, 0.0001f);
	CHECK_NEAR(detection::YawDelta(10.0f, 350.0f), -20.0f, 0.0001f);
	CHECK_NEAR(detection::YawDelta(0.0f, 180.0f), 180.0f, 0.0001f);

	CHECK(detection::IsFinite(QAngle(1.0f, 2.0f, 3.0f)));
	CHECK(!detection::IsFinite(QAngle(1.0f, INFINITY, 3.0f)));
	CHECK(detection::IsFinite(Vector(1.0f, 2.0f, 3.0f)));
	CHECK(!detection::IsFinite(Vector(1.0f, 2.0f, NAN)));
}

void TestWeaponTables()
{
	// detection_system.cpp IsBallisticWeapon / NormalizeWeapon
	CHECK(detection::NormalizeWeapon("weapon_ak47") == "ak47");
	CHECK(detection::NormalizeWeapon("ak47") == "ak47");
	CHECK(detection::IsBallisticWeapon("weapon_ak47"));
	CHECK(detection::IsBallisticWeapon("usp_silencer"));
	CHECK(!detection::IsBallisticWeapon("weapon_knife"));
	CHECK(!detection::IsBallisticWeapon("weapon_hegrenade"));
	CHECK(detection::AreOpponents(CS_TEAM_T, CS_TEAM_CT));
	CHECK(!detection::AreOpponents(CS_TEAM_T, CS_TEAM_T));
	CHECK(!detection::AreOpponents(CS_TEAM_SPECTATOR, CS_TEAM_T));
	CHECK(!detection::AreOpponents(CS_TEAM_NONE, CS_TEAM_T));
}

// ---------------------------------------------------------------------------------------------
// The original's compile-time rules, re-run as runtime assertions
// ---------------------------------------------------------------------------------------------

void TestScoringRules()
{
	// triggerbot_module.cpp ScoreReactions
	CHECK(detection::ScoreReactions(std::array<int, 5> {0, 1, 0, 1, 0}, 5).score == 10);
	CHECK(detection::ScoreReactions(std::array<int, 10> {2, 2, 2, 2, 2, 2, 2, 2, 2, 2}, 10).score == 0);
	CHECK(detection::ScoreReactions(std::array<int, 4> {1, 2, 3, 4}, 4).score == 0);
	CHECK(detection::ScoreReactions(std::array<int, 4> {10, 20, 30, 40}, 4).score == 0);
	CHECK(detection::ScoreReactions(std::array<int, 3> {3, 3, 1}, 3).score == 2);
	CHECK(detection::ScoreReactions(std::array<int, 3> {0, 0, 3}, 3).score == 1);
	{
		const auto score = detection::ScoreReactions(std::array<int, 5> {0, 1, 0, 1, 0}, 5);
		CHECK(score.oneTickCount == 5); // every tick of that sample is <= 1
		CHECK(score.twoTickCount == 0);
		CHECK(score.normalCount == 0);
		const auto mixed = detection::ScoreReactions(std::array<int, 4> {0, 2, 3, 1}, 4);
		CHECK(mixed.oneTickCount == 2);
		CHECK(mixed.twoTickCount == 1);
		CHECK(mixed.normalCount == 1);
	}

	// aimbot_module.cpp MeetsSmoothThresholds (the confirmed demo / the confirmed negative)
	CHECK(detection::MeetsSmoothThresholds(13.66f, 0.949f, 0.85f, 0.946f, 3, true));
	CHECK(!detection::MeetsSmoothThresholds(6.31f, 0.934f, 0.41f, 1.0f, 3, true));
	CHECK(!detection::MeetsSmoothThresholds(20.0f, 0.95f, 2.0f, 0.99f, 4, true));      // final error above 1 degree
	CHECK(!detection::MeetsSmoothThresholds(20.0f, 0.95f, 0.5f, 0.99f, 2, true));      // fewer than three steps
	CHECK(!detection::MeetsSmoothThresholds(20.0f, 0.95f, 0.5f, 0.99f, 4, false));     // not shrinking

	// network_safety.cpp ShouldVeto
	CHECK(!detection::ShouldVeto(149.9f, 24.9f, 0.019f, 0.019f, 0.019f, 0.019f, 0));
	CHECK(detection::ShouldVeto(150.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0));
	CHECK(detection::ShouldVeto(0.0f, 25.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0));
	CHECK(detection::ShouldVeto(0.0f, 0.0f, 0.02f, 0.0f, 0.0f, 0.0f, 0));
	CHECK(detection::ShouldVeto(0.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.0f, 0));
	CHECK(detection::ShouldVeto(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1));

	// aimlock_module.cpp MeetsCoverage (19 of 20 samples)
	CHECK(detection::MeetsCoverage(123, 129));
	CHECK(!detection::MeetsCoverage(122, 129));
	CHECK(!detection::MeetsCoverage(0, 0));

	// nulls.cpp scaling
	{
		// Unknown frame rate (the fallback the port uses): the most conservative requirement.
		detection::NullsTiming unknown;
		CHECK(detection::NullsRequiredPerfectCstrafes(unknown, 128) == 640);
		// A 512 FPS client: the lowest requirement (Lerp(0, 128, 640)).
		detection::NullsTiming fast;
		fast.medianFramerate = 1.0f / 512.0f;
		CHECK(detection::NullsRequiredPerfectCstrafes(fast, 128) == 128);
		// A 64 FPS client: the highest requirement the detector uses for a known rate.
		detection::NullsTiming slow;
		slow.medianFramerate = 1.0f / 64.0f;
		CHECK(detection::NullsRequiredPerfectCstrafes(slow, 128) == 640);
		// fps_max caps the rate the client is credited with (a 512 FPS reading with fps_max 64 is
		// scored as 64 FPS, which is the conservative direction).
		detection::NullsTiming clamped;
		clamped.medianFramerate = 1.0f / 512.0f;
		clamped.maxFps = 64.0f;
		CHECK(detection::NullsRequiredPerfectCstrafes(clamped, 128) == 640);
		// Underlap forgiveness needs both the median and the share.
		detection::NullsTiming forgiving;
		forgiving.underlapMedian = 0.03f;
		forgiving.underlapCount = 5;
		forgiving.total = 20;
		CHECK(detection::NullsUnderlapForgiven(forgiving));
		detection::NullsTiming rare;
		rare.underlapMedian = 0.03f;
		rare.underlapCount = 1;
		rare.total = 20;
		CHECK(!detection::NullsUnderlapForgiven(rare));
		// A zero underlap median keeps the base requirement; the maximum median doubles it.
		CHECK(detection::NullsAdjustedRequiredPerfectCstrafes(128, 0.0f) == 128);
		CHECK(detection::NullsAdjustedRequiredPerfectCstrafes(128, 0.02f) == 640);
	}

	// cvars.cpp integrity-check cadence: one queried cvar per cvarCycleInterval / kCvarQueryCount
	// seconds (the delay the original's timer callback returned), so a full round of the queried
	// cvars takes the 1-5 s interval, not one second per cvar.
	for (int index = 1; index <= MAXPLAYERS; ++index) {
		const f64 interval = detection::CvarCycleInterval(index);
		CHECK(interval >= 1.0 && interval < 5.0);
		const f64 delay = detection::CvarQueryDelay(interval);
		CHECK(delay >= 0.1 && delay < 0.5);
		CHECK(std::fabs(delay * static_cast<f64>(detection::kCvarQueryCount) - interval) < 1e-9);
	}

	// detection.cpp Lerp helpers
	CHECK_NEAR(detection::LerpF(0.5f, 10.0f, 20.0f), 15.0f, 0.0001f);
	CHECK(detection::LerpU32(0.0f, 128, 640) == 128);
	CHECK(detection::LerpU32(1.0f, 128, 640) == 640);
}

// ---------------------------------------------------------------------------------------------
// The crash-fix guards: ConVar handle validation and the usercmd layout plausibility gates
// ---------------------------------------------------------------------------------------------

void TestGuardHelpers()
{
	// cvar::IsValidHandle — s2sdk prints "Invalid ConVar handle: 4294967295" for names that its
	// FindConVar could not resolve, so both spellings of that sentinel must be rejected.
	CHECK(!cvar::IsValidHandle(0));
	CHECK(!cvar::IsValidHandle(cvar::kInvalidHandle));                  // 0xFFFFFFFF
	CHECK(!cvar::IsValidHandle(0xFFFFFFFFFFFFFFFFull));                 // sign-extended -1
	CHECK(!cvar::IsValidHandle((1ull << 32) | 0xFFFFFFFFull));          // 0x1_00000000FFFFFFFF
	CHECK(cvar::IsValidHandle(1));
	CHECK(cvar::IsValidHandle(0x7ffee1234567ull));                      // a plausible pointer-sized handle
	CHECK(cvar::IsValidHandle((1ull << 40) | 0xFFFFFFFEull));

	// cs2ac::IsValidHandle — entity handles, not ConVar handles: e.g. index 93,
	// serial 102949 -> 0xC912805D = -921534371, a *valid* handle handed out once the engine's serial
	// counter has passed 65535 (bit 31 set). Every `handle >= 0` test used to drop it.
	CHECK(IsValidHandle(-921534371));
	CHECK(IsValidHandle(0x7FFFFFFF));
	CHECK(IsValidHandle(1));
	CHECK(!IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)

	// cvar::BoolFromString — the truth spellings for a bool convar (the convar table's defaults and
	// the string fallback for a cvar whose engine type is not bool).
	CHECK(cvar::BoolFromString("1"));
	CHECK(cvar::BoolFromString("true"));
	CHECK(cvar::BoolFromString("TRUE"));
	CHECK(cvar::BoolFromString("yes"));
	CHECK(cvar::BoolFromString("Yes"));
	CHECK(!cvar::BoolFromString("0"));
	CHECK(!cvar::BoolFromString("false"));
	CHECK(!cvar::BoolFromString("no"));
	CHECK(!cvar::BoolFromString(""));
	CHECK(!cvar::BoolFromString("2"));
	CHECK(!cvar::BoolFromString("on"));
	CHECK(!cvar::BoolFromString(nullptr));

	// engine::PlausibleUserId — guards the {userid} punishment placeholder: both the userids game
	// events report and the raw CServerSideClientBase::m_UserID read must be small positive numbers,
	// so a wrong gamedata offset cannot substitute a random id into a kick command.
	CHECK(engine::PlausibleUserId(1));
	CHECK(engine::PlausibleUserId(2));
	CHECK(engine::PlausibleUserId(0xFFFF));
	CHECK(!engine::PlausibleUserId(0));
	CHECK(!engine::PlausibleUserId(-1));
	CHECK(!engine::PlausibleUserId(0x10000));
	CHECK(!engine::PlausibleUserId(0x7FFFFFFFFFFFFFFFll));

	// usercmd:: plausibility gates (a wrong PlayerCommand stride must never reach the detectors).
	CHECK(usercmd::IsPlausibleCommandNumber(0));
	CHECK(usercmd::IsPlausibleCommandNumber(1));
	CHECK(usercmd::IsPlausibleCommandNumber(usercmd::kMaxCommandNumber));
	CHECK(!usercmd::IsPlausibleCommandNumber(-1));
	CHECK(!usercmd::IsPlausibleCommandNumber(usercmd::kMaxCommandNumber + 1));
	CHECK(usercmd::IsStrictlyIncreasing({}));
	CHECK(usercmd::IsStrictlyIncreasing({5}));
	CHECK(usercmd::IsStrictlyIncreasing({5, 6, 7}));
	CHECK(usercmd::IsStrictlyIncreasing({5, 9, 40}));                    // gaps are legitimate
	CHECK(!usercmd::IsStrictlyIncreasing({5, 5}));                       // repeated => wrong stride
	CHECK(!usercmd::IsStrictlyIncreasing({5, 4}));
	CHECK(!usercmd::IsStrictlyIncreasing({5, 6, 2}));

	// movement:: — the frame-perfect-jump comparison whose *inputs* used to never arrive (the
	// movement services pointer was read with an entity-handle accessor and always came back null).
	// movement_events.cpp: lastUsableJumpPress < modernLandTime, both tick + fraction.
	CHECK_NEAR(movement::JumpEventTime(4200, 0.25f), 4200.25f, 0.0001f);
	CHECK(movement::IsFramePerfectJump(4200, 0.25f, 4200, 0.75f));       // press before the landing tick rolls over
	CHECK(!movement::IsFramePerfectJump(4200, 0.75f, 4200, 0.25f));
	CHECK(movement::IsFramePerfectJump(4199, 0.5f, 4200, 0.5f));         // a tick earlier is always earlier
	CHECK(!movement::IsFramePerfectJump(4200, 0.5f, 4199, 0.5f));
	CHECK(!movement::IsFramePerfectJump(4200, 0.5f, 4200, 0.5f));        // identical times are not "perfect"
	CHECK(movement::IsFramePerfectJump(4200, -0.1f, 4200, 0.0f));        // fractions may be negative
}

// ---------------------------------------------------------------------------------------------
// FrameMemo — the per-frame sharing engine::PlayerConnected and friends are built on
// ---------------------------------------------------------------------------------------------

// The memo is what turns "every pass asks s2sdk again" into "one read per slot per frame": the
// fetch functor must run exactly once per filled memo, no matter how many readers ask for the
// value, and the record is reset by assigning {} over it (engine::FrameSnapshotScope clears the
// whole per-slot table that way at the start of every frame).
void TestFrameMemo()
{
	int fetches = 0;
	auto fetch = [&] {
		++fetches;
		return fetches * 10;
	};

	FrameMemo<int> memo;
	// Nobody has asked yet: the frame pays for a value only when a pass reads it.
	CHECK(fetches == 0);
	CHECK(memo.Get(fetch) == 10);
	CHECK(memo.filled);
	// Every later reader in the same frame gets the same value and does not fetch again.
	CHECK(memo.Get(fetch) == 10);
	CHECK(memo.Get(fetch) == 10);
	CHECK(fetches == 1);

	// The next frame starts from a cleared record and fetches once more.
	memo = {};
	CHECK(!memo.filled);
	CHECK(memo.Get(fetch) == 20);
	CHECK(memo.Get(fetch) == 20);
	CHECK(fetches == 2);

	// A memo whose value is the type's default still counts as filled: a fetch that answers false
	// (say "this client is not a fake client") must not be repeated by the next reader.
	int boolFetches = 0;
	FrameMemo<bool> flag;
	CHECK(!flag.Get([&] { ++boolFetches; return false; }));
	CHECK(!flag.Get([&] { ++boolFetches; return true; }));
	CHECK(boolFetches == 1);
	CHECK(!flag.value);

	// A struct value round-trips like a scalar (Vector is what the position frame needs).
	FrameMemo<Vector> origin;
	const Vector filled = origin.Get([&] { return Vector {1.0f, 2.0f, 3.0f}; });
	CHECK_NEAR(filled.x, 1.0f, 0.0001f);
	CHECK_NEAR(filled.y, 2.0f, 0.0001f);
	CHECK_NEAR(filled.z, 3.0f, 0.0001f);
	CHECK_NEAR(origin.Get([&] { return Vector {}; }).z, 3.0f, 0.0001f);
}

// ---------------------------------------------------------------------------------------------
// config.cpp — the cs2ac.cfg parser and the command template check
// ---------------------------------------------------------------------------------------------

void TestConfigParser()
{
	const std::string contents =
		"// CS2AC loads this file automatically.\n"
		"\n"
		"cs2ac_enabled 1\n"
		"cs2ac_whitelist \"76561198000000001,76561198000000002\"\n"
		"cs2ac_punishment_command \"css_addban {steamid64} 0 CS2AC: {detection}\" // trailing comment\n"
		"cs2ac_kick_command \"\"\n"
		"cs2ac_language \"en\"\n"
		"cs2ac_config_loaded\n";
	const std::vector<ConfigLine> lines = ParseConfigFile(contents);
	CHECK(lines.size() == 6);
	if (lines.size() == 6) {
		CHECK(lines[0].name == "cs2ac_enabled" && lines[0].value == "1");
		CHECK(lines[1].name == "cs2ac_whitelist" && lines[1].value == "76561198000000001,76561198000000002");
		CHECK(lines[2].name == "cs2ac_punishment_command" && lines[2].value == "css_addban {steamid64} 0 CS2AC: {detection}");
		CHECK(lines[3].name == "cs2ac_kick_command" && lines[3].value.empty());
		CHECK(lines[4].name == "cs2ac_language" && lines[4].value == "en");
		CHECK(lines[5].name == "cs2ac_config_loaded" && lines[5].value.empty());
		CHECK(lines[0].line == 3);
	}

	// The real shipped file parses into its documented markers.
	{
		std::ifstream file(std::string(CS2AC_SOURCE_DIR) + "/assets/cs2ac.cfg", std::ios::binary);
		CHECK(file.good());
		if (file) {
			std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			const auto real = ParseConfigFile(text);
			bool sawMarker = false;
			bool sawAimbot = false;
			for (const auto& line : real) {
				sawMarker |= line.name == "cs2ac_config_loaded";
				sawAimbot |= line.name == "cs2ac_aimbot_enabled" && line.value == "1";
			}
			CHECK(sawMarker);
			CHECK(sawAimbot);
		}
	}

	// settings:: state machine (the same accessors the detectors use).
	settings::Reset();
	CHECK(settings::IsPluginEnabled());
	CHECK(settings::IsDetectionEnabled(DetectionType::Aimbot));
	CHECK(settings::GetEnabledDetectionCount() == static_cast<std::size_t>(DetectionType::Count));
	settings::SetDetectionSetting(DetectionType::Bhop, false);
	CHECK(!settings::IsDetectionEnabled(DetectionType::Bhop));
	CHECK(settings::GetEnabledDetectionCount() == static_cast<std::size_t>(DetectionType::Count) - 1);
	settings::SetEnabled(false);
	CHECK(!settings::IsPluginEnabled());
	CHECK(settings::GetEnabledDetectionCount() == 0);
	settings::SetEnabled(true);
	settings::SetWhitelist("76561198000000002, 76561198000000001,76561198000000002, nonsense, 0");
	CHECK(settings::GetWhitelistCount() == 2);
	CHECK(settings::GetRejectedWhitelistCount() == 2);   // "nonsense" and "0"
	CHECK(settings::GetDuplicateWhitelistCount() == 1);
	CHECK(settings::IsPlayerWhitelisted(76561198000000001ULL));
	CHECK(settings::IsPlayerWhitelisted(76561198000000002ULL));
	CHECK(!settings::IsPlayerWhitelisted(76561198000000003ULL));
	CHECK(!settings::IsPlayerWhitelisted(0));
	settings::SetString("cs2ac_kick_command", "css_kick #{userid} CS2AC: {detection}");
	CHECK(std::string(settings::GetKickCommand()) == "css_kick #{userid} CS2AC: {detection}");
	settings::Reset();
	// A re-Reset installs a fresh table: nothing the previous run changed survives it.
	CHECK(settings::GetWhitelistCount() == 0);
	CHECK(settings::IsPluginEnabled());
	CHECK(settings::IsDetectionEnabled(DetectionType::Bhop));
	CHECK(std::string(settings::GetKickCommand()) == "css_kick #{userid} CS2AC: {detection}");
	CHECK(std::string(settings::RawValue("cs2ac_aimbot_enabled")) == "1");
}

void TestCommandTemplate()
{
	// cs2ac.cpp CheckCommandTemplate — the findings count drives cs2ac_check_config.
	CHECK(CheckCommandTemplate("cs2ac_punishment_command", "css_addban {steamid64} 0 CS2AC: {detection}", true) == 0);
	CHECK(CheckCommandTemplate("cs2ac_kick_command", "css_kick #{userid} CS2AC: {detection}", false) == 0);
	CHECK(CheckCommandTemplate("cs2ac_kick_command", "", false) == 1);                       // empty disables it
	CHECK(CheckCommandTemplate("cs2ac_punishment_command", "css_addban {steamid64} 0", true) == 0);
	CHECK(CheckCommandTemplate("cs2ac_punishment_command", "css_addban 0", true) == 1);      // needs {steamid64}
	CHECK(CheckCommandTemplate("cs2ac_kick_command", "css_kick 1", false) == 1);             // needs {userid}/{steamid64}
	CHECK(CheckCommandTemplate("cs2ac_kick_command", "css_kick {bogus}", false) == 2);       // unknown placeholder + missing id
	CHECK(CheckCommandTemplate("cs2ac_kick_command", "css_kick {userid}", false) == 0);
}

// ---------------------------------------------------------------------------------------------
// localization.cpp — the phrase loader and the fallback rules
// ---------------------------------------------------------------------------------------------

void TestLocalization()
{
	const std::string dir = std::string(CS2AC_SOURCE_DIR) + "/testdata/translations";
	std::filesystem::create_directories(dir);
	{
		std::ofstream file(dir + "/zz-test.txt");
		file << "\"Phrases\"\n{\n"
			 << "\t// a comment line\n"
			 << "\t\"greeting\"\t\"hallo {player}\"\n"
			 << "\t\"mismatch\"\t\"nur {zwei}\"\n"
			 << "\t\"missing\"\t\"\"\n"
			 << "\t\"escaped\"\t\"a\\nb\"\n"
			 << "}\n";
	}
	localization::Reload("zz-test", dir);
	CHECK(localization::LoadedPhraseCount() >= 3);
	CHECK(localization::HasPhrase("greeting"));
	// A phrase with the same placeholders is used.
	CHECK(localization::Get("greeting", "hello {player}") == "hallo {player}");
	// A phrase with a different placeholder set falls back to English.
	CHECK(localization::Get("mismatch", "only {one}") == "only {one}");
	// Missing keys fall back to English; empty values are dropped by the loader.
	CHECK(localization::Get("missing", "the english text") == "the english text");
	CHECK(localization::Get("no.such.key", "fallback") == "fallback");

	// Format applies the arguments to both languages.
	const auto text = localization::Format("greeting", "hello {player}", {{"player", "Ana"}});
	CHECK(text.english == "hello Ana");
	CHECK(text.localized == "hallo Ana");

	// The watermark comes from the built-in table (no file needed).
	localization::Reload("en", dir);
	const auto watermark = localization::Watermark({{"author", "karola3vax"}});
	CHECK(watermark.english.find("karola3vax") != std::string::npos);
	CHECK(watermark.localized.find("karola3vax") != std::string::npos);
	CHECK(watermark.english != watermark.localized || localization::LoadedPhraseCount() == 0);
	localization::Reload("de", dir);
	const auto german = localization::Watermark({{"author", "X"}});
	CHECK(german.localized.find("X") != std::string::npos);
	CHECK(german.english.find("X") != std::string::npos);

	// The shipped English phrase file loads with all its phrases.
	localization::Reload("en", std::string(CS2AC_SOURCE_DIR) + "/translations");
	CHECK(localization::LoadedPhraseCount() > 50);
	CHECK(localization::HasPhrase("evidence.aimbot.smooth"));
	CHECK(localization::Get("evidence.aimbot.smooth", "x {incidents} {snap} {before} {after}").find("{incidents}") != std::string::npos);
	// Every shipped language file parses into a non-empty phrase set.
	std::size_t parsedLanguages = 0;
	std::error_code error;
	const std::filesystem::path shipped(std::string(CS2AC_SOURCE_DIR) + "/translations");
	for (const auto& entry : std::filesystem::directory_iterator(shipped, error)) {
		if (entry.path().extension() != ".txt") {
			continue;
		}
		localization::Reload(entry.path().stem().string().c_str(), shipped.string());
		if (localization::LoadedPhraseCount() > 0) {
			++parsedLanguages;
		}
	}
	CHECK(!error);
	CHECK(parsedLanguages >= 30);
	localization::Shutdown();
}

// ---------------------------------------------------------------------------------------------
// core.cpp — the usercmd protobuf reader
// ---------------------------------------------------------------------------------------------

class ProtoWriter {
public:
	std::vector<uint8_t>& Buffer() { return buffer; }

	void VarintField(uint32_t field, uint64_t value)
	{
		Tag(field, 0);
		Varint(value);
	}

	void FloatField(uint32_t field, float value)
	{
		Tag(field, 5);
		uint32_t raw = 0;
		std::memcpy(&raw, &value, sizeof(raw));
		for (int i = 0; i < 4; ++i) {
			buffer.push_back(static_cast<uint8_t>((raw >> (8 * i)) & 0xff));
		}
	}

	void MessageField(uint32_t field, const std::vector<uint8_t>& payload)
	{
		Tag(field, 2);
		Varint(payload.size());
		buffer.insert(buffer.end(), payload.begin(), payload.end());
	}

	void Varint(uint64_t value)
	{
		while (true) {
			uint8_t byte = static_cast<uint8_t>(value & 0x7f);
			value >>= 7;
			if (value != 0) {
				byte |= 0x80;
			}
			buffer.push_back(byte);
			if (value == 0) {
				break;
			}
		}
	}

private:
	void Tag(uint32_t field, uint32_t wireType) { Varint((static_cast<uint64_t>(field) << 3) | wireType); }
	std::vector<uint8_t> buffer;
};

std::vector<uint8_t> EncodeQAngle(float x, float y, float z)
{
	ProtoWriter writer;
	writer.FloatField(1, x);
	writer.FloatField(2, y);
	writer.FloatField(3, z);
	return writer.Buffer();
}

// The engine embeds CSGOUserCmdPB in a fixed-size object (PlayerCommand: 16 bytes of CUserCmdBase
// plus the message, 144 bytes total), so the reader is always handed a padded buffer it may read to
// the end of. The tests model that: the message goes into a zero-padded 128-byte object.
std::vector<uint8_t> InCommandObject(const std::vector<uint8_t>& message, size_t objectSize = 128)
{
	std::vector<uint8_t> object(objectSize, 0);
	const size_t copy = std::min(message.size(), objectSize);
	std::memcpy(object.data(), message.data(), copy);
	return object;
}

void TestUserCmdReader()
{
	// A message shaped exactly like CSGOUserCmdPB for one command with a shot and subtick moves.
	ProtoWriter buttons;
	buttons.VarintField(1, 0x1234);
	buttons.VarintField(2, IN_ATTACK);
	buttons.VarintField(3, 0);

	ProtoWriter subtickPress;
	subtickPress.VarintField(1, IN_JUMP);
	subtickPress.VarintField(2, 1);
	subtickPress.FloatField(3, 0.25f);
	subtickPress.FloatField(6, 1.5f);
	subtickPress.FloatField(7, -2.5f);

	ProtoWriter subtickAnalog;
	subtickAnalog.FloatField(4, 2.0f);

	ProtoWriter base;
	base.VarintField(1, 4242);           // command_number
	base.VarintField(2, 987654);         // client_tick
	base.MessageField(3, buttons.Buffer());
	base.MessageField(4, EncodeQAngle(1.0f, 2.0f, 3.0f));
	base.VarintField(11, 7);             // mousedx
	base.VarintField(12, -9);            // mousedy
	base.VarintField(13, 2);             // consumed_server_angle_changes
	base.MessageField(18, subtickPress.Buffer());
	base.MessageField(18, subtickAnalog.Buffer());

	ProtoWriter interp0;
	interp0.VarintField(1, 999);         // src_tick
	interp0.VarintField(2, 1001);        // dst_tick
	interp0.FloatField(3, 0.25f);        // frac

	ProtoWriter history;
	history.MessageField(2, EncodeQAngle(10.0f, 20.0f, 0.0f));
	history.VarintField(3, 1000);        // render_tick_count
	history.FloatField(4, 0.5f);         // render_tick_fraction
	history.MessageField(5, interp0.Buffer());
	history.VarintField(8, 987654);      // player_tick_count

	ProtoWriter message;
	message.MessageField(1, base.Buffer());
	message.MessageField(2, history.Buffer());
	message.VarintField(6, 0);           // attack1_start_history_index

	const std::vector<uint8_t> object = InCommandObject(message.Buffer());
	UsercmdView view;
	CHECK(ParseUserCmdMessage(object.data(), object.size(), view));
	CHECK(view.hasBase);
	CHECK(view.commandNumber == 4242);
	CHECK(view.hasClientTick && view.clientTick == 987654);
	CHECK(view.hasViewAngles);
	CHECK_NEAR(view.viewAngles.x, 1.0f, 0.0001f);
	CHECK_NEAR(view.viewAngles.y, 2.0f, 0.0001f);
	CHECK_NEAR(view.viewAngles.z, 3.0f, 0.0001f);
	CHECK(view.buttonState1 == 0x1234);
	CHECK(view.buttonState2 == IN_ATTACK);
	CHECK(view.buttonState3 == 0);
	CHECK(view.hasMouseDx && view.mouseDx == 7);
	CHECK(view.hasMouseDy && view.mouseDy == -9);
	CHECK(view.hasConsumedServerAngleChanges && view.consumedServerAngleChanges == 2);
	CHECK(view.subtickMoves.size() == 2);
	CHECK(view.subtickMoves[0].hasButton && view.subtickMoves[0].button == IN_JUMP);
	CHECK(view.subtickMoves[0].pressed);
	CHECK_NEAR(view.subtickMoves[0].when, 0.25f, 0.0001f);
	CHECK(view.subtickMoves[0].hasPitchDelta);
	CHECK_NEAR(view.subtickMoves[0].pitchDelta, 1.5f, 0.0001f);
	CHECK(view.subtickMoves[0].hasYawDelta);
	CHECK_NEAR(view.subtickMoves[0].yawDelta, -2.5f, 0.0001f);
	CHECK(!view.subtickMoves[1].hasButton);
	CHECK(view.subtickMoves[1].hasAnalogForwardDelta);
	CHECK_NEAR(view.subtickMoves[1].analogForwardDelta, 2.0f, 0.0001f);
	CHECK(view.inputHistory.size() == 1);
	CHECK(view.inputHistory[0].hasViewAngles);
	CHECK_NEAR(view.inputHistory[0].viewAngles.x, 10.0f, 0.0001f);
	CHECK_NEAR(view.inputHistory[0].viewAngles.y, 20.0f, 0.0001f);
	CHECK(view.inputHistory[0].hasRenderTickCount && view.inputHistory[0].renderTickCount == 1000);
	CHECK(view.inputHistory[0].hasRenderTickFraction);
	CHECK_NEAR(view.inputHistory[0].renderTickFraction, 0.5f, 0.0001f);
	CHECK(view.inputHistory[0].hasSvInterp0);
	CHECK(view.inputHistory[0].svInterp0.hasSrcTick && view.inputHistory[0].svInterp0.srcTick == 999);
	CHECK(view.inputHistory[0].svInterp0.hasDstTick && view.inputHistory[0].svInterp0.dstTick == 1001);
	CHECK_NEAR(view.inputHistory[0].svInterp0.frac, 0.25f, 0.0001f);
	CHECK(view.inputHistory[0].hasPlayerTickCount && view.inputHistory[0].playerTickCount == 987654);
	CHECK(view.hasAttack1StartHistoryIndex && view.attack1StartHistoryIndex == 0);

	// Unknown fields (including nested messages and 64-bit values) are skipped, not misread.
	{
		ProtoWriter unknown;
		unknown.VarintField(9, 123456789);
		unknown.FloatField(4, 5.0f);
		ProtoWriter inner;
		inner.VarintField(1, 1);
		unknown.MessageField(7, inner.Buffer());
		unknown.MessageField(1, base.Buffer());           // a valid base
		ProtoWriter historyEntry;                         // CSGOInputHistoryEntryPB: view_angles = 2
		historyEntry.MessageField(2, EncodeQAngle(4.0f, 5.0f, 6.0f));
		unknown.MessageField(2, historyEntry.Buffer());
		ProtoWriter nested;
		nested.VarintField(3, 3);
		unknown.MessageField(9, nested.Buffer());
		const std::vector<uint8_t> unknownObject = InCommandObject(unknown.Buffer());
		UsercmdView skipped;
		CHECK(ParseUserCmdMessage(unknownObject.data(), unknownObject.size(), skipped));
		CHECK(skipped.hasBase);
		CHECK(skipped.commandNumber == 4242);
		CHECK(skipped.inputHistory.size() == 1);
		CHECK_NEAR(skipped.inputHistory[0].viewAngles.y, 5.0f, 0.0001f);
	}

	// A message without a base is rejected (the original's has_base() gate).
	std::printf("cs2ac: usercmd reader: no-base / bounds\n");
	{
		ProtoWriter noBase;
		noBase.VarintField(6, 3);
		const std::vector<uint8_t> noBaseObject = InCommandObject(noBase.Buffer());
		UsercmdView empty;
		CHECK(!ParseUserCmdMessage(noBaseObject.data(), noBaseObject.size(), empty));
	}
	CHECK(!ParseUserCmdMessage(nullptr, 128, view));
	CHECK(!ParseUserCmdMessage(object.data(), 0, view));
	// A truncated object (the last bytes cut off) must not read past what it was given.
	{
		std::vector<uint8_t> truncated(object.begin(), object.begin() + 12);
		UsercmdView partial;
		CHECK(!ParseUserCmdMessage(truncated.data(), truncated.size(), partial));
	}
	// Padding-only objects (no fields at all) parse as "no base".
	{
		const std::vector<uint8_t> zeros(128, 0);
		UsercmdView empty;
		CHECK(!ParseUserCmdMessage(zeros.data(), zeros.size(), empty));
	}
}

// ---------------------------------------------------------------------------------------------
// The ConVar tables the plugin registers (config.cpp) — the surface s2sdk's
// "Only 0 of 47 cs2ac convars could be registered" came from. The engine type of each entry is
// what decides which s2sdk creator is used (bool / string / int32 / float), so the tables are
// guarded here, including the fork's 18 new settings and 22 numeric thresholds/windows.
// ---------------------------------------------------------------------------------------------

void TestConVarTables()
{
	CHECK(settings::kConVarCount == 49);       // the original's 31 settings + 18 fork settings
	CHECK(settings::kDebugConVarCount == 19);  // the original's 16 + tick_integrity/trigger_pattern/rcs
	CHECK(settings::kNumericConVarCount == 22); // fork int/float thresholds and windows

	std::size_t strings = 0;
	std::size_t debug = 0;
	std::string firstName;
	for (const auto& spec : settings::kConVars) {
		CHECK(spec.name != nullptr && std::string(spec.name).rfind("cs2ac_", 0) == 0);
		CHECK(spec.description != nullptr && *spec.description != '\0');
		if (spec.isString) {
			++strings;
		} else {
			// A bool convar's default must be a value the bool parser understands.
			CHECK(cvar::BoolFromString(spec.defaultValue) || std::strcmp(spec.defaultValue, "0") == 0);
		}
		++debug;
	}
	// Punishment/kick command, weak-signal policy, the 11 fork lists, update repository, the 4
	// webhook fields, language, whitelist.
	CHECK(strings == 19);
	CHECK(debug == settings::kConVarCount);
	for (std::size_t index = 1; index < settings::kConVarCount; ++index) {
		CHECK(std::strcmp(settings::kConVars[index].name, settings::kConVars[index - 1].name) != 0);
	}
	for (const auto& spec : settings::kDebugConVars) {
		CHECK(spec.name != nullptr && std::string(spec.name).rfind("cs2ac_", 0) == 0
			  && std::string(spec.name).find("_debug") != std::string::npos);
		CHECK(spec.description != nullptr && *spec.description != '\0');
	}

	// Names, defaults and types spot-checked against the original's settings.cpp (its CConVar
	// declarations) and the debug switches' own declarations.
	CHECK(std::strcmp(settings::kConVars[0].name, "cs2ac_enabled") == 0);
	CHECK(std::strcmp(settings::kConVars[0].defaultValue, "1") == 0 && !settings::kConVars[0].isString);
	CHECK(std::strcmp(settings::kConVars[21].name, "cs2ac_join_watermark") == 0 && !settings::kConVars[21].isString);
	CHECK(std::strcmp(settings::kConVars[21].defaultValue, "0") == 0);
	CHECK(std::strcmp(settings::kConVars[22].name, "cs2ac_auto_update") == 0 && std::strcmp(settings::kConVars[22].defaultValue, "0") == 0);
	CHECK(std::strcmp(settings::kConVars[23].name, "cs2ac_update_repository") == 0 && settings::kConVars[23].isString);
	CHECK(std::strcmp(settings::kConVars[24].name, "cs2ac_punishment_command") == 0);
	CHECK(settings::kConVars[24].isString
		  && std::strcmp(settings::kConVars[24].defaultValue, "css_addban {steamid64} 0 CS2AC: {detection}") == 0);
	CHECK(std::strcmp(settings::kConVars[25].name, "cs2ac_kick_command") == 0);
	CHECK(settings::kConVars[25].isString && std::strcmp(settings::kConVars[25].defaultValue, "css_kick #{userid} CS2AC: {detection}") == 0);
	CHECK(std::strcmp(settings::kConVars[36].name, "cs2ac_webhook_url") == 0 && settings::kConVars[36].isString);
	CHECK(std::strcmp(settings::kConVars[40].name, "cs2ac_language") == 0);
	CHECK(settings::kConVars[40].isString && std::strcmp(settings::kConVars[40].defaultValue, "en") == 0);
	CHECK(std::strcmp(settings::kConVars[42].name, "cs2ac_allow_sv_cheats_testing") == 0 && !settings::kConVars[42].isString);
	CHECK(std::strcmp(settings::kDebugConVars[0].name, "cs2ac_aimbot_debug") == 0);
	CHECK(std::strcmp(settings::kDebugConVars[15].name, "cs2ac_triggerbot_debug") == 0);
	CHECK(std::strcmp(settings::kDebugConVars[16].name, "cs2ac_tick_integrity_debug") == 0);
	CHECK(std::strcmp(settings::kDebugConVars[17].name, "cs2ac_trigger_pattern_debug") == 0);
	CHECK(std::strcmp(settings::kDebugConVars[18].name, "cs2ac_rcs_debug") == 0);

	// Fork defaults that the policy relies on (v8-v11).
	CHECK(std::strcmp(settings::kConVars[27].defaultValue, "AIMBOT,AIMLOCK,SILENTAIM,BACKTRACK,DLL INJECTION") == 0);
	CHECK(std::strcmp(settings::kConVars[28].defaultValue, "TRIGGER PATTERN,MOUSE MISMATCH,ANTIAIM") == 0);
	CHECK(std::strcmp(settings::kConVars[29].defaultValue, "ANTIAIM") == 0);
	CHECK(std::strcmp(settings::kConVars[30].defaultValue, "BHOP,NULLS") == 0);
	CHECK(std::strcmp(settings::kConVars[31].defaultValue, "DESUBTICKING") == 0);
	CHECK(std::strcmp(settings::kConVars[32].defaultValue, "TRIGGER PATTERN,INVALID INPUT") == 0);
	CHECK(std::strcmp(settings::kConVars[33].defaultValue, "INVALID INPUT") == 0);
	CHECK(std::strcmp(settings::kConVars[34].defaultValue, "INVALID INPUT,NAMECHANGER") == 0);
	CHECK(std::strcmp(settings::kConVars[35].defaultValue, "DLL INJECTION") == 0);
	for (const auto& spec : settings::kNumericConVars) {
		CHECK(spec.name != nullptr && std::string(spec.name).rfind("cs2ac_", 0) == 0);
		CHECK(spec.description != nullptr && *spec.description != '\0');
	}

	// The shipped cs2ac.cfg ↔ these tables: every setting line names a registered cvar, and the
	// type it declares is the one engine::SetCvarValue must use when it applies the value to it.
	// The 39 bool lines are exactly the ones that made s2sdk's string setter answer "Invalid cvar
	// type for variable '...'. Expected: 'bool', but got: 'string'" at every server start (and
	// drop the file's value) while the file was applied through it.
	{
		std::ifstream file(std::string(CS2AC_SOURCE_DIR) + "/assets/cs2ac.cfg", std::ios::binary);
		CHECK(file.good());
		if (file) {
			const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			std::size_t booleanLines = 0;
			std::size_t stringLines = 0;
			std::size_t numericLines = 0;
			for (const auto& line : ParseConfigFile(text)) {
				if (EqualsIgnoreCase(line.name, "cs2ac_config_loaded")) {
					continue; // the loader's marker, not a setting
				}
				bool known = false;
				bool isString = false;
				for (const auto& spec : settings::kConVars) {
					if (EqualsIgnoreCase(line.name, spec.name)) {
						known = true;
						isString = spec.isString;
						break;
					}
				}
				for (const auto& spec : settings::kDebugConVars) {
					if (!known && EqualsIgnoreCase(line.name, spec.name)) {
						known = true;
						break;
					}
				}
				bool numeric = false;
				for (const auto& spec : settings::kNumericConVars) {
					if (!known && EqualsIgnoreCase(line.name, spec.name)) {
						known = true;
						numeric = true;
						break;
					}
				}
				CHECK(known);
				if (!known) {
					continue;
				}
				if (numeric) {
					++numericLines;
				} else if (isString) {
					++stringLines;
				} else {
					++booleanLines;
				}
			}
			// 24 bool settings + 16 debug switches; 18 string settings; 11 int/float settings.
			CHECK(booleanLines == 40); // applied with the bool setter, never the string one
			CHECK(stringLines == 18);
			CHECK(numericLines == 11);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Fork policy decisions (v8-v11): detection lists, severity, stream/combo/mass-guard/repeat-ban
// windows. Pure helpers from decisions.cpp, pinned with the fork's own defaults.
// ---------------------------------------------------------------------------------------------

void TestForkPolicy()
{
	using namespace detection;

	// IsListed: case-insensitive, whitespace-trimmed, no substring matches, empty list.
	CHECK(ListContains("AIMBOT,AIMLOCK,SILENTAIM,BACKTRACK,DLL INJECTION", "backtrack"));
	CHECK(ListContains(" TRIGGER PATTERN , MOUSE MISMATCH ", "mouse mismatch"));
	CHECK(!ListContains("AIMBOT", "AIM"));
	CHECK(!ListContains("", "AIMBOT"));
	CHECK(!ListContains(nullptr, "AIMBOT"));
	CHECK(ListContains("ANTIAIM", "ANTIAIM"));
	CHECK(!ListContains("ANTIAIM", "AIM"));

	// Report-only list: RCS never punishes; TRIGGER PATTERN left it in v9.
	CHECK(IsReportOnlyDetection("RCS"));
	CHECK(!IsReportOnlyDetection("TRIGGER PATTERN"));
	CHECK(!IsReportOnlyDetection("AIMBOT"));

	const char* banList = "AIMBOT,AIMLOCK,SILENTAIM,BACKTRACK,DLL INJECTION";
	const char* kickList = "TRIGGER PATTERN,MOUSE MISMATCH,ANTIAIM";
	CHECK(SeverityForDetection("AIMBOT", false, banList, kickList, false) == DetectionSeverity::Ban);
	CHECK(SeverityForDetection("DLL INJECTION", false, banList, kickList, false) == DetectionSeverity::Ban);
	CHECK(SeverityForDetection("ANTIAIM", false, banList, kickList, false) == DetectionSeverity::Kick);
	CHECK(SeverityForDetection("TRIGGER PATTERN", false, banList, kickList, false) == DetectionSeverity::Kick);
	// RCS is report-only even if an operator lists it everywhere.
	CHECK(SeverityForDetection("RCS", false, "RCS", "RCS", true) == DetectionSeverity::Report);
	// Weak findings follow cs2ac_weak_signal_policy; kickOnly only blocks the ban.
	CHECK(SeverityForDetection("DESUBTICKING", false, banList, kickList, false) == DetectionSeverity::Report);
	CHECK(SeverityForDetection("DESUBTICKING", false, banList, kickList, true) == DetectionSeverity::Kick);
	CHECK(SeverityForDetection("AIMBOT", true, banList, kickList, false) == DetectionSeverity::Report);
	CHECK(SeverityForDetection("AIMBOT", true, banList, kickList, true) == DetectionSeverity::Kick);

	// Stream/burst window: three reports within 60 s.
	{
		std::deque<i64> times;
		CHECK(!WindowedCountReached(times, 1000, 3, 60));
		CHECK(!WindowedCountReached(times, 1030, 3, 60));
		CHECK(WindowedCountReached(times, 1059, 3, 60));
		std::deque<i64> slow;
		CHECK(!WindowedCountReached(slow, 0, 3, 60));
		CHECK(!WindowedCountReached(slow, 61, 3, 60));
		CHECK(!WindowedCountReached(slow, 122, 3, 60)); // each previous entry expired
		CHECK(!WindowedCountReached(slow, 200, 0, 60)); // disabled
		CHECK(!WindowedCountReached(slow, 200, 3, 0));
	}

	// MULTIPLE SIGNALS: 3 different kinds, BHOP/NULLS ignored.
	{
		std::set<std::string> kinds;
		const char* ignore = "BHOP,NULLS";
		CHECK(!ComboReached(kinds, "BHOP", ignore, 3));
		CHECK(!ComboReached(kinds, "NULLS", ignore, 3));
		CHECK(!ComboReached(kinds, "AIMBOT", ignore, 3));
		CHECK(!ComboReached(kinds, "AIMBOT", ignore, 3)); // same kind twice
		CHECK(!ComboReached(kinds, "DESUBTICKING", ignore, 3));
		CHECK(ComboReached(kinds, "INVALID INPUT", ignore, 3));
		std::set<std::string> disabled;
		CHECK(!ComboReached(disabled, "AIMBOT", ignore, 0));
	}

	// Mass guard: 5 distinct players in 60 min, duplicates do not count twice.
	{
		std::deque<MassGuardHit> hits;
		CHECK(!MassGuardReached(hits, 0, 3600, 11, 5));
		CHECK(!MassGuardReached(hits, 10, 3600, 11, 5)); // same player
		CHECK(!MassGuardReached(hits, 20, 3600, 12, 5));
		CHECK(!MassGuardReached(hits, 30, 3600, 13, 5));
		CHECK(!MassGuardReached(hits, 40, 3600, 14, 5));
		CHECK(MassGuardReached(hits, 50, 3600, 15, 5));
		std::deque<MassGuardHit> expired;
		CHECK(!MassGuardReached(expired, 0, 60, 11, 2));
		CHECK(!MassGuardReached(expired, 61, 60, 12, 2)); // the first is outside the window
		CHECK(MassGuardReached(expired, 62, 60, 13, 2));
		CHECK(!MassGuardReached(hits, 50, 3600, 0, 5));   // no steamid
		CHECK(!MassGuardReached(hits, 50, 3600, 99, 0));  // guard off
	}

	// Repeat ban: a second kick within 24 h; INVALID INPUT also needs another finding.
	{
		const char* repeatList = "TRIGGER PATTERN,INVALID INPUT";
		const char* needsOther = "INVALID INPUT";
		const i64 day = 24 * 3600;
		std::deque<i64> kicks;
		std::unordered_map<std::string, i64> findings;
		CHECK(!RepeatBanReached(kicks, findings, "TRIGGER PATTERN", repeatList, needsOther, 100, day));
		kicks.push_back(100);
		CHECK(RepeatBanReached(kicks, findings, "TRIGGER PATTERN", repeatList, needsOther, 200, day));
		// The kick that is being evaluated now has not been recorded yet, so one prior kick is enough.
		std::deque<i64> invalidKicks {100};
		CHECK(!RepeatBanReached(invalidKicks, findings, "INVALID INPUT", repeatList, needsOther, 200, day));
		findings["BHOP"] = 150;
		findings["NULLS"] = 150;
		findings["RCS"] = 150;
		CHECK(!RepeatBanReached(invalidKicks, findings, "INVALID INPUT", repeatList, needsOther, 200, day));
		findings["AIMBOT"] = 150;
		CHECK(RepeatBanReached(invalidKicks, findings, "INVALID INPUT", repeatList, needsOther, 200, day));
		CHECK(!RepeatBanReached(invalidKicks, findings, "NAMECHANGER", repeatList, needsOther, 200, day)); // not listed
		std::deque<i64> oldKicks {100};
		CHECK(!RepeatBanReached(oldKicks, findings, "TRIGGER PATTERN", repeatList, needsOther, 100 + day + 1, day));
	}

	// JSON escaping of attacker-controlled text.
	CHECK(JsonString("plain") == "\"plain\"");
	CHECK(JsonString("a\"b\\c") == "\"a\\\"b\\\\c\"");
	CHECK(JsonString("line\nnext\tend\r") == "\"line\\nnext\\tend\\r\"");
	CHECK(JsonString(std::string("\x01", 1).c_str()) == "\"\\u0001\"");
	CHECK(JsonString(nullptr) == "\"\"");
}

// ---------------------------------------------------------------------------------------------
// Fork tick-integrity / SILENTAIM / trigger-pattern / RCS decisions.
// ---------------------------------------------------------------------------------------------

void TestForkIntegrity()
{
	using namespace detection;

	CHECK_NEAR(TickRenderTime(true, 100, true, 0.5f), 100.5, 1e-9);
	CHECK_NEAR(TickRenderTime(true, 100, false, 0.0f), 100.0, 1e-9);
	CHECK(TickRenderTime(false, 100, true, 0.5f) == kTickNone);
	CHECK(TickRenderTime(true, 0, false, 0.0f) == kTickNone);
	{
		InterpInfo interp;
		interp.hasSrcTick = true;
		interp.srcTick = 100;
		interp.hasDstTick = true;
		interp.dstTick = 102;
		interp.hasFrac = true;
		interp.frac = 0.5f;
		CHECK_NEAR(TickInterpTime(interp), 101.0, 1e-9);
		InterpInfo missing;
		CHECK(TickInterpTime(missing) == kTickNone);
	}
	CHECK_NEAR(TickDip(103.0, 100.0, 104.0), 3.0, 1e-9);
	CHECK_NEAR(TickDip(100.0, 101.0, 104.0), -1.0, 1e-9);
	CHECK(std::isnan(TickDip(0.0, 100.0, 104.0)));
	CHECK(TickHasFraction(100.5));
	CHECK(!TickHasFraction(100.0));
	CHECK(!TickHasFraction(-1.0));
	CHECK(TickRewriteFingerprint(true, kTickNone));
	CHECK(!TickRewriteFingerprint(true, 100.0));
	CHECK(!TickRewriteFingerprint(false, kTickNone));

	// BACKTRACK counting: >= 2 ticks, continuous clock, fingerprint (shot and neighbours).
	CHECK(BacktrackDipCounts(2.0f, 2.0f, 1.0, 4.0f, true, true, true, true));
	CHECK(!BacktrackDipCounts(1.9f, 2.0f, 1.0, 4.0f, true, true, true, true));
	CHECK(!BacktrackDipCounts(2.0f, 2.0f, -1.0, 4.0f, true, true, true, true));
	CHECK(!BacktrackDipCounts(2.0f, 2.0f, 4.5, 4.0f, true, true, true, true));
	CHECK(!BacktrackDipCounts(2.0f, 2.0f, 1.0, 4.0f, false, true, true, true));
	CHECK(BacktrackDipCounts(2.0f, 2.0f, 1.0, 4.0f, false, false, false, false)); // v3 rule
	CHECK(!BacktrackDipCounts(2.0f, 2.0f, 1.0, 4.0f, true, true, false, true));
	CHECK(!BacktrackDipCounts(2.0f, 2.0f, 1.0, 4.0f, true, true, true, false));

	CHECK(TickDipBucket(0.4f) == 0);
	CHECK(TickDipBucket(0.9f) == 1);
	CHECK(TickDipBucket(1.5f) == 2);
	CHECK(TickDipBucket(3.9f) == 3);
	CHECK(TickDipBucket(4.0f) == 4);
	CHECK(TickInterpOffsetBucket(0.5f) == 0);
	CHECK(TickInterpOffsetBucket(2.5f) == 2);
	CHECK(TickInterpOffsetBucket(5.0f) == 4);
	CHECK(TickInterpOffsetBucket(6.0f) == 5);

	// SILENTAIM v7/v7.1: 0.25-degree gate, still camera +1, turning flick < 2 degrees half.
	CHECK(SilentAimPoints(0.24f, -1.0f, false, false, false, false, false) == 0);
	CHECK(SilentAimPoints(0.25f, -1.0f, false, false, false, false, false) == 2);
	CHECK(SilentAimPoints(1.0f, -1.0f, false, false, false, false, false) == 2);
	CHECK(SilentAimPoints(1.0f, 0.2f, false, false, false, false, false) == 3);   // still camera
	CHECK(SilentAimPoints(1.0f, 1.0f, false, false, false, false, false) == 1);   // flick half, rounded up (3/2)
	CHECK(SilentAimPoints(2.0f, 1.0f, false, false, false, false, false) == 2);   // not under 2 degrees
	CHECK(SilentAimPoints(23.0f, 1.0f, false, false, false, false, false) == 4);  // blatant, turning
	CHECK(SilentAimPoints(23.0f, 0.1f, false, false, false, false, false) == 5);  // blatant, still camera
	CHECK(SilentAimPoints(1.0f, 0.1f, true, true, true, true, true) == 10);       // 3+1+2+2+1 base, +1 still
	CHECK(SilentAimPoints(1.0f, 1.0f, true, true, true, true, true) == 5);        // (9+1)/2
	CHECK(SilentAimExcessBucket(0.2f) == 0);
	CHECK(SilentAimExcessBucket(0.3f) == 1);
	CHECK(SilentAimExcessBucket(1.5f) == 3);
	CHECK(SilentAimExcessBucket(5.0f) == 5);
	CHECK(SilentAimExcessBucket(22.5f) == 6);
	CHECK(SilentAimCameraBucket(0.1f) == 0);
	CHECK(SilentAimCameraBucket(3.0f) == 2);
	CHECK(SilentAimCameraBucket(20.0f) == 4);
	CHECK(SilentAimCameraBucket(-1.0f) == 5);
	CHECK(SilentAimCameraBucket(std::numeric_limits<f32>::quiet_NaN()) == 5);

	// Trigger pattern buckets and the upper median of the fork.
	CHECK(TriggerPatternHoldBucket(1.9f) == 0);
	CHECK(TriggerPatternHoldBucket(2.0f) == 1);
	CHECK(TriggerPatternHoldBucket(20.0f) == 3);
	CHECK(TriggerPatternHoldBucket(299.0f) == 6);
	CHECK(TriggerPatternHoldBucket(300.0f) == 7);
	CHECK(TriggerPatternMedian({}) == 0);
	CHECK(TriggerPatternMedian({5}) == 5);
	CHECK(TriggerPatternMedian({1, 2, 3}) == 2);
	CHECK(TriggerPatternMedian({1, 2, 3, 4}) == 3);

	// RCS math.
	CHECK(IsAutomaticWeapon("weapon_ak47"));
	CHECK(IsAutomaticWeapon("m4a1_silencer"));
	CHECK(!IsAutomaticWeapon("awp"));
	CHECK(!IsAutomaticWeapon("deagle"));
	CHECK_NEAR(RecoilAngleDelta(10.0f, 350.0f), 20.0f, 1e-4);
	CHECK_NEAR(RecoilAngleDelta(350.0f, 10.0f), -20.0f, 1e-4);
	{
		const std::vector<f32> x {1.0f, 2.0f, 3.0f, 4.0f};
		const std::vector<f32> y {2.0f, 4.0f, 6.0f, 8.0f};
		const std::vector<f32> inverse {-2.0f, -4.0f, -6.0f, -8.0f};
		const std::vector<f32> flat {1.0f, 1.0f, 1.0f, 1.0f};
		CHECK_NEAR(RecoilCorrelation(x, y), 1.0f, 1e-5);
		CHECK_NEAR(RecoilCorrelation(x, inverse), -1.0f, 1e-5);
		CHECK_NEAR(RecoilCorrelation(x, flat), 0.0f, 1e-5);
		CHECK_NEAR(RecoilCorrelation({1.0f}, {2.0f}), 0.0f, 1e-5);
	}
	CHECK(!RecoilSprayQualifies(true, 10, 20, "ak47"));   // invalid (air/weapon switch)
	CHECK(!RecoilSprayQualifies(false, 7, 20, "ak47"));   // too few shots
	CHECK(!RecoilSprayQualifies(false, 10, 15, "ak47"));  // too few samples
	CHECK(!RecoilSprayQualifies(false, 10, 20, "awp"));   // not automatic
	CHECK(RecoilSprayQualifies(false, 8, 16, "ak47"));
	{
		std::deque<RecoilSprayResult> sprays;
		for (int index = 0; index < 10; ++index) {
			sprays.push_back({0.95f, 1.0f, 0});
		}
		CHECK(RecoilSuspiciousShare(sprays, 0.9f, 0.8f));
		sprays[0].bestLag = 3; // outside the 2-tick limit
		sprays[1].gain = 1.5f; // gain outside [0.5, 1.3]
		sprays[2].bestCorrelation = 0.5f;
		CHECK(!RecoilSuspiciousShare(sprays, 0.9f, 0.8f)); // 7/10 = 70% < 80%
		CHECK(RecoilSuspiciousShare(sprays, 0.9f, 0.7f));  // 70% >= 70%
	}
}

} // namespace

int main()
{
	// Unbuffered: a crash must not swallow the progress line that says where it happened.
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("cs2ac tests\n");

	const std::pair<const char*, void (*)()> tests[] = {
		{"angle math", TestAngleMath},
		{"weapon tables", TestWeaponTables},
		{"scoring rules", TestScoringRules},
		{"guard helpers", TestGuardHelpers},
		{"frame memo", TestFrameMemo},
		{"config parser", TestConfigParser},
		{"convar tables", TestConVarTables},
		{"command template", TestCommandTemplate},
		{"localization", TestLocalization},
		{"usercmd reader", TestUserCmdReader},
		{"fork policy", TestForkPolicy},
		{"fork integrity", TestForkIntegrity},
	};
	for (const auto& test : tests) {
		std::printf("-- %s\n", test.first);
		try {
			test.second();
		} catch (const std::exception& error) {
			++g_failures;
			std::printf("FAIL: %s threw: %s\n", test.first, error.what());
		} catch (...) {
			++g_failures;
			std::printf("FAIL: %s threw an unknown exception\n", test.first);
		}
	}
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
