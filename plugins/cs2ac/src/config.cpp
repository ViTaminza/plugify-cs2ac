// cs2ac — settings: the original's ConVar table (settings.cpp) plus the cs2ac.cfg parser.
//
// The Metamod original registered 31 ConVars and let the engine `exec cs2ac.cfg`; under Plugify the
// plugin registers the same 31 ConVars through s2sdk (so console toggles like `cs2ac_aimbot_debug 1`
// behave identically) and reads configs/cs2ac/cs2ac.cfg itself, applying every `name value` line
// through SetConVar*. The trailing `cs2ac_config_loaded` marker keeps its original meaning: the
// config is only considered complete when the whole file was applied.

#include "core.hpp"

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace cs2ac {

const char* const kDetectionNames[static_cast<size_t>(DetectionType::Count)] = {
	"AIMBOT",    "AIMLOCK",     "ANTIAIM",          "AUTOSTRAFE",   "BHOP",          "DLL INJECTION",      "DESUBTICKING",
	"DOUBLETAP", "HYPERSCROLL", "INHUMAN ACCURACY", "INVALID CVAR", "INVALID INPUT", "IRREGULAR BEHAVIOR", "NAMECHANGER",
	"NULLS",     "SILENTAIM",   "SUBTICK SPAM",     "TRIGGERBOT",
};

namespace settings {

const ConVarSpec kConVars[kConVarCount] = {
	{"cs2ac_enabled", "1", "Enable or disable CS2AC", false},
	{"cs2ac_aimbot_enabled", "1", "Detect damaging visible aim snaps", false},
	{"cs2ac_aimlock_enabled", "1", "Detect unnaturally precise target tracking", false},
	{"cs2ac_antiaim_enabled", "1", "Detect impossible or manipulated view angles", false},
	{"cs2ac_autostrafe_enabled", "1", "Detect automated air strafing", false},
	{"cs2ac_bhop_enabled", "1", "Detect automated bunny hopping", false},
	{"cs2ac_dll_injection_enabled", "1", "Detect suspicious client event subscriptions", false},
	{"cs2ac_desubticking_enabled", "1", "Detect commands that remove normal subtick timing", false},
	{"cs2ac_doubletap_enabled", "1", "Detect impossible rapid fire", false},
	{"cs2ac_hyperscroll_enabled", "1", "Detect automated jump-input frequency", false},
	{"cs2ac_inhuman_accuracy_enabled", "1", "Detect sustained near-perfect accuracy", false},
	{"cs2ac_invalid_cvar_enabled", "1", "Detect unsafe client settings", false},
	{"cs2ac_invalid_input_enabled", "1", "Detect movement button changes without matching subtick records", false},
	{"cs2ac_irregular_behavior_enabled", "1", "Detect repeated success with unusually difficult shots", false},
	{"cs2ac_namechanger_enabled", "1", "Detect repeated player name changes", false},
	{"cs2ac_nulls_enabled", "1", "Detect mechanically perfect airborne opposite-direction switches", false},
	{"cs2ac_silentaim_enabled", "1", "Detect damaging shots that disagree with the visible aim", false},
	{"cs2ac_subtick_spam_enabled", "1", "Detect repeated same-time button aliases carrying pitch or yaw changes", false},
	{"cs2ac_triggerbot_enabled", "1", "Detect repeated inhuman reactions to fresh crosshair contact", false},
	{"cs2ac_chat_announcements", "1", "Show CS2AC detections in public chat", false},
	{"cs2ac_center_announcements", "1", "Show CS2AC detections in the center of the screen", false},
	{"cs2ac_join_watermark", "0",
	 "Show each player the \"protected by CS2AC\" notice in chat and the center of the screen 10 seconds after joining", false},
	{"cs2ac_auto_update", "0", "Automatically download verified stable updates from the configured fork", false},
	{"cs2ac_update_repository", "",
	 "GitHub owner/repo used for automatic updates; empty disables updates and keeps fork builds self-hosted", true},
	{"cs2ac_punishment_command", "css_addban {steamid64} 0 CS2AC: {detection}", "Command run for permanent-ban detections", true},
	{"cs2ac_kick_command", "css_kick #{userid} CS2AC: {detection}", "Command run for kick-only detections", true},
	{"cs2ac_weak_signal_policy", "report",
	 "Punishment for movement, integrity, behavioural and settings findings: report (default, no kick) or kick", true},
	{"cs2ac_ban_detections", "AIMBOT,AIMLOCK,SILENTAIM,BACKTRACK,DLL INJECTION",
	 "Comma-separated detections that may issue a ban (others report or kick)", true},
	{"cs2ac_kick_detections", "TRIGGER PATTERN,MOUSE MISMATCH,ANTIAIM",
	 "Comma-separated detections that kick even when weak findings are report-only", true},
	{"cs2ac_unstable_kick_detections", "ANTIAIM",
	 "Comma-separated detections that still kick when the network check vetoes punishment", true},
	{"cs2ac_combo_ignore_detections", "BHOP,NULLS", "Comma-separated detections that do not count toward cs2ac_combo_kick_types", true},
	{"cs2ac_burst_kick_detections", "DESUBTICKING",
	 "Comma-separated detections that kick a player who trips them in a short burst (empty = off)", true},
	{"cs2ac_repeat_ban_detections", "TRIGGER PATTERN,INVALID INPUT",
	 "Comma-separated kick detections whose second kick within cs2ac_repeat_ban_hours bans (empty = off)", true},
	{"cs2ac_repeat_ban_needs_other", "INVALID INPUT", "Repeat-ban detections that also need another finding in the window", true},
	{"cs2ac_stream_kick_detections", "INVALID INPUT,NAMECHANGER",
	 "Comma-separated detections that kick a player who trips them in a stream (empty = off)", true},
	{"cs2ac_mass_guard_detections", "DLL INJECTION",
	 "Comma-separated ban detections that fall back to reports when many players trip them at once", true},
	{"cs2ac_webhook_url", "", "Discord webhook URL for detection reports", true},
	{"cs2ac_webhook_role_id", "", "Discord role ID mentioned in detection reports", true},
	{"cs2ac_webhook_server_address", "", "Public server address shown in Discord reports", true},
	{"cs2ac_webhook_logo_url", "", "Public HTTPS URL for the logo shown in Discord reports", true},
	{"cs2ac_language", "en", "Language used for public messages and Discord reports", true},
	{"cs2ac_whitelist", "", "SteamID64s that CS2AC may detect but never punish", true},
	{"cs2ac_allow_sv_cheats_testing", "0", "Keep CS2AC detections enabled while sv_cheats is enabled", false},
	{"cs2ac_tick_integrity_enabled", "1", "Check input-history time for backtrack and forged tick counts", false},
	{"cs2ac_backtrack_require_forged", "1",
	 "Count a dip toward BACKTRACK only when the shot entry has a whole-tick render time and no interpolation info (the rewrite "
	 "fingerprint)",
	 false},
	{"cs2ac_backtrack_interp_report", "0", "Allow INTERP BACKTRACK reports (telemetry is always written)", false},
	{"cs2ac_trigger_pattern_enabled", "1", "Collect click-timing, reaction-shape and on-target statistics (report-only)", false},
	{"cs2ac_rcs_enabled", "1", "Collect recoil-control statistics during sprays (report-only)", false},
	{"cs2ac_rcs_report", "0", "Allow RCS reports (telemetry is always written)", false},
};

const NumericConVarSpec kNumericConVars[kNumericConVarCount] = {
	{"cs2ac_announce_cooldown", false, "30", "Seconds between identical detection announcements per player (0 = unlimited)"},
	{"cs2ac_mass_guard_players", false, "5",
	 "Different players within cs2ac_mass_guard_minutes that switch a guarded detection to reports (0 = off)"},
	{"cs2ac_mass_guard_minutes", false, "60", "Window in minutes for cs2ac_mass_guard_players"},
	{"cs2ac_combo_kick_types", false, "3",
	 "Kick a player once this many different report-only detections fired in one connection (0 = off)"},
	{"cs2ac_burst_kick_reports", false, "3",
	 "Reports of a burst-kick detection within cs2ac_burst_kick_seconds that kick the player"},
	{"cs2ac_burst_kick_seconds", false, "60", "Window in seconds for cs2ac_burst_kick_reports"},
	{"cs2ac_repeat_ban_hours", false, "24", "Window in hours for the repeat ban"},
	{"cs2ac_repeat_ban_minutes", false, "1440", "Length of the repeat ban in minutes"},
	{"cs2ac_stream_kick_reports", false, "20",
	 "Reports of a stream-kick detection within cs2ac_stream_kick_seconds that kick the player"},
	{"cs2ac_stream_kick_seconds", false, "180", "Window in seconds for cs2ac_stream_kick_reports"},
	{"cs2ac_backtrack_shots", false, "3", "Backtracked shots within 120 s needed for a BACKTRACK report"},
	{"cs2ac_mouse_mismatch_samples", false, "100", "View turns with horizontal mouse movement needed before MOUSE MISMATCH is judged (0 = off)"},
	{"cs2ac_backtrack_min_ticks", true, "2.0",
	 "A shot counts as backtracked when its time dips this many ticks below the entries around it"},
	{"cs2ac_backtrack_max_gap", true, "4.0",
	 "Ignore a dip when the entries before and after the shot are more than this many ticks apart (client stall)"},
	{"cs2ac_mouse_mismatch_agreement", true, "0.9",
	 "Report MOUSE MISMATCH when fewer than this share of those turns went the way the mouse moved"},
	{"cs2ac_backtrack_interp_min_ticks", true, "4.0",
	 "A shot counts toward INTERP BACKTRACK when its sv_interp0 time is this many ticks behind its own render time"},
	{"cs2ac_trigger_pattern_fast_tap_ms", true, "20.0", "Holds shorter than this many milliseconds are treated as synthetic"},
	{"cs2ac_trigger_pattern_fast_click_share", true, "0.6",
	 "Report when at least this share of recent taps is held under the cs2ac_trigger_pattern_fast_tap_ms threshold"},
	{"cs2ac_trigger_pattern_on_target_share", true, "0.97",
	 "Report when at least this share of recent combat presses starts on the enemy"},
	{"cs2ac_trigger_pattern_delay_concentration", true, "0.8",
	 "Report when at least this share of held-angle reactions lies within one tick of the median"},
	{"cs2ac_rcs_correlation", true, "0.9", "A spray counts as machine-compensated at or above this view/punch correlation"},
	{"cs2ac_rcs_suspicious_share", true, "0.8", "Report when at least this share of recent sprays is machine-compensated"},
};

const DebugConVarSpec kDebugConVars[kDebugConVarCount] = {
	// name, description (both verbatim from the original's CConVar<bool> declarations)
	{"cs2ac_aimbot_debug", "Show why Aimbot accepts or rejects each damaging shot"},
	{"cs2ac_aimlock_debug", "Show Aimlock tracking episodes and evidence"},
	{"cs2ac_antiaim_debug", "Show AntiAim evidence, score decay, and shot matching"},
	{"cs2ac_autostrafe_debug", "Show Autostrafe jump statistics, optimizer matches, and evidence counts"},
	{"cs2ac_bhop_debug", "Show Bhop landing gates, perfect chains, and input patterns"},
	{"cs2ac_dll_injection_debug", "Show DLL Injection scan timing, availability, and matches"},
	{"cs2ac_doubletap_debug", "Show Doubletap tick spacing and network safety checks"},
	{"cs2ac_hyperscroll_debug", "Show Hyperscroll landing samples, jump-input patterns, and perfect ratios"},
	{"cs2ac_inhuman_accuracy_debug", "Show Inhuman Accuracy attempts, evidence, and rejections"},
	{"cs2ac_invalid_cvar_debug", "Show Invalid CVar replies, latch transitions, and unavailable checks"},
	{"cs2ac_irregular_debug", "Show Irregular Behavior attempts, evidence, and rejections"},
	{"cs2ac_namechanger_debug", "Show Namechanger baselines, rolling counts, and evidence expiry"},
	{"cs2ac_nulls_debug", "Show Nulls timing checks and evidence"},
	{"cs2ac_silentaim_debug", "Show why Silentaim accepts or rejects each matched shot"},
	{"cs2ac_subtick_debug", "Show Invalid Input, Subtick Spam, and Desubticking evidence"},
	{"cs2ac_triggerbot_debug", "Show Triggerbot contacts, reactions, and safety rejections"},
	{"cs2ac_tick_integrity_debug", "Show input-history time measurements"},
	{"cs2ac_trigger_pattern_debug", "Show Trigger Pattern measurements"},
	{"cs2ac_rcs_debug", "Show RCS spray measurements"},
};

namespace {

// The original's Configuration struct state (the ConVar values live here; the engine side keeps
// its s2sdk ConVars in sync so console access works the same).
struct Configuration {
	std::string enabled {"1"};
	std::string detectionSettings[static_cast<size_t>(DetectionType::Count)]; // "1"/"0"
	std::string chatAnnouncements {"1"};
	std::string centerAnnouncements {"1"};
	std::string joinWatermark {"0"};
	std::string automaticUpdates {"0"};
	std::string updateRepository;
	std::string punishmentCommand {"css_addban {steamid64} 0 CS2AC: {detection}"};
	std::string kickCommand {"css_kick #{userid} CS2AC: {detection}"};
	std::string weakSignalPolicy {"report"};
	std::string banDetections {"AIMBOT,AIMLOCK,SILENTAIM,BACKTRACK,DLL INJECTION"};
	std::string kickDetections {"TRIGGER PATTERN,MOUSE MISMATCH,ANTIAIM"};
	std::string unstableKickDetections {"ANTIAIM"};
	std::string comboIgnoreDetections {"BHOP,NULLS"};
	std::string burstKickDetections {"DESUBTICKING"};
	std::string repeatBanDetections {"TRIGGER PATTERN,INVALID INPUT"};
	std::string repeatBanNeedsOther {"INVALID INPUT"};
	std::string streamKickDetections {"INVALID INPUT,NAMECHANGER"};
	std::string massGuardDetections {"DLL INJECTION"};
	std::string tickIntegrityEnabled {"1"};
	std::string backtrackRequireForged {"1"};
	std::string backtrackInterpReport {"0"};
	std::string triggerPatternEnabled {"1"};
	std::string rcsEnabled {"1"};
	std::string rcsReport {"0"};
	std::string webhookUrl;
	std::string webhookRoleId;
	std::string webhookServerAddress;
	std::string webhookLogoUrl;
	std::string language {"en"};
	std::string whitelist;
	std::string allowSvCheatsTesting {"0"};
	std::unordered_map<std::string, std::string> debugValues;
	std::unordered_map<std::string, std::string> numericValues;
};

Configuration* configuration {};
std::size_t rejectedWhitelistEntries {};
std::size_t duplicateWhitelistEntries {};
u64 settingsRevision {1};
u64 detectionMask {};
bool detectionMaskDirty {true};
bool pluginEnabled {};

std::vector<u64>& WhitelistedSteamIds()
{
	static std::vector<u64> steamIds;
	return steamIds;
}

void BumpRevisionInternal()
{
	if (++settingsRevision == 0) {
		settingsRevision = 1;
	}
}

bool ToBool(const std::string& value)
{
	return value == "1" || EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "yes");
}

// settings.cpp OnWhitelistChanged
void RebuildWhitelist(const std::string& raw)
{
	auto& steamIds = WhitelistedSteamIds();
	steamIds.clear();
	rejectedWhitelistEntries = 0;
	duplicateWhitelistEntries = 0;
	std::string value = raw;
	std::replace(value.begin(), value.end(), ',', ' ');
	std::replace(value.begin(), value.end(), ';', ' ');
	std::istringstream entries(value);
	for (std::string entry; entries >> entry;) {
		u64 steamId = 0;
		const auto parsed = std::from_chars(entry.data(), entry.data() + entry.size(), steamId);
		if (parsed.ec == std::errc() && parsed.ptr == entry.data() + entry.size() && steamId != 0) {
			steamIds.push_back(steamId);
		} else {
			++rejectedWhitelistEntries;
		}
	}
	std::sort(steamIds.begin(), steamIds.end());
	const std::size_t parsedEntries = steamIds.size();
	steamIds.erase(std::unique(steamIds.begin(), steamIds.end()), steamIds.end());
	duplicateWhitelistEntries = parsedEntries - steamIds.size();
}

const char* DetectionSettingName(DetectionType detection)
{
	switch (detection) {
	case DetectionType::Aimbot: return "cs2ac_aimbot_enabled";
	case DetectionType::Aimlock: return "cs2ac_aimlock_enabled";
	case DetectionType::AntiAim: return "cs2ac_antiaim_enabled";
	case DetectionType::Autostrafe: return "cs2ac_autostrafe_enabled";
	case DetectionType::Bhop: return "cs2ac_bhop_enabled";
	case DetectionType::DllInjection: return "cs2ac_dll_injection_enabled";
	case DetectionType::Desubticking: return "cs2ac_desubticking_enabled";
	case DetectionType::Doubletap: return "cs2ac_doubletap_enabled";
	case DetectionType::Hyperscroll: return "cs2ac_hyperscroll_enabled";
	case DetectionType::InhumanAccuracy: return "cs2ac_inhuman_accuracy_enabled";
	case DetectionType::InvalidCvar: return "cs2ac_invalid_cvar_enabled";
	case DetectionType::InvalidInput: return "cs2ac_invalid_input_enabled";
	case DetectionType::IrregularBehavior: return "cs2ac_irregular_behavior_enabled";
	case DetectionType::NameChanger: return "cs2ac_namechanger_enabled";
	case DetectionType::Nulls: return "cs2ac_nulls_enabled";
	case DetectionType::SilentAim: return "cs2ac_silentaim_enabled";
	case DetectionType::SubtickSpam: return "cs2ac_subtick_spam_enabled";
	case DetectionType::Triggerbot: return "cs2ac_triggerbot_enabled";
	case DetectionType::Count: break;
	}
	return "";
}

bool DetectionSetting(DetectionType detection)
{
	if (!configuration) {
		return false;
	}
	const auto index = static_cast<std::size_t>(detection);
	if (index >= static_cast<std::size_t>(DetectionType::Count)) {
		return false;
	}
	return ToBool(configuration->detectionSettings[index]);
}

} // namespace

void Reset()
{
	// Reset runs on every (re)start; the previous table must not be leaked into the new run.
	delete configuration;
	configuration = new Configuration;
	for (auto& value : configuration->detectionSettings) {
		value = "1";
	}
	for (const auto& spec : kDebugConVars) {
		configuration->debugValues[spec.name] = "0";
	}
	for (const auto& spec : kNumericConVars) {
		configuration->numericValues[spec.name] = spec.defaultValue;
	}
	detectionMaskDirty = true;
	pluginEnabled = false;
	settingsRevision = 1;
	RebuildWhitelist(configuration->whitelist);
}

bool IsPluginEnabled()
{
	GetDetectionMask();
	return pluginEnabled;
}

bool IsDetectionEnabled(DetectionType detection)
{
	const auto index = static_cast<std::uint8_t>(detection);
	return index < static_cast<std::uint8_t>(DetectionType::Count) && (GetDetectionMask() & (u64 {1} << index)) != 0;
}

bool IsPlayerWhitelisted(u64 steamId)
{
	const auto& steamIds = WhitelistedSteamIds();
	return steamId != 0 && std::binary_search(steamIds.begin(), steamIds.end(), steamId);
}

std::size_t GetWhitelistCount() { return WhitelistedSteamIds().size(); }
std::size_t GetRejectedWhitelistCount() { return rejectedWhitelistEntries; }
std::size_t GetDuplicateWhitelistCount() { return duplicateWhitelistEntries; }

std::size_t GetEnabledDetectionCount()
{
	const u64 mask = GetDetectionMask();
	std::size_t count = 0;
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index) {
		count += (mask >> index) & 1;
	}
	return count;
}

u64 GetDetectionMask()
{
	if (!detectionMaskDirty) {
		return detectionMask;
	}
	detectionMask = 0;
	pluginEnabled = configuration && ToBool(configuration->enabled);
	if (pluginEnabled) {
		for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index) {
			if (DetectionSetting(static_cast<DetectionType>(index))) {
				detectionMask |= u64 {1} << index;
			}
		}
	}
	detectionMaskDirty = false;
	return detectionMask;
}

u64 GetRevision() { return settingsRevision; }
bool ShowChatAnnouncements() { return configuration && ToBool(configuration->chatAnnouncements); }
bool ShowCenterAnnouncements() { return configuration && ToBool(configuration->centerAnnouncements); }
bool ShowJoinWatermark() { return configuration && ToBool(configuration->joinWatermark); }
bool AutomaticUpdatesEnabled()
{
	// A fork must opt in to its own release repository before the updater is allowed to run.
	return configuration && ToBool(configuration->automaticUpdates) && !configuration->updateRepository.empty();
}
bool WeakSignalsKick() { return configuration && EqualsIgnoreCase(configuration->weakSignalPolicy, "kick"); }
const char* GetUpdateRepository() { return configuration ? configuration->updateRepository.c_str() : ""; }
const char* GetPunishmentCommand() { return configuration ? configuration->punishmentCommand.c_str() : ""; }
const char* GetKickCommand() { return configuration ? configuration->kickCommand.c_str() : ""; }

bool BoolSetting(const char* name, bool defaultValue)
{
	if (!configuration || !name) {
		return defaultValue;
	}
	if (EqualsIgnoreCase(name, "cs2ac_join_watermark")) {
		return ToBool(configuration->joinWatermark);
	}
	if (EqualsIgnoreCase(name, "cs2ac_tick_integrity_enabled")) {
		return ToBool(configuration->tickIntegrityEnabled);
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_require_forged")) {
		return ToBool(configuration->backtrackRequireForged);
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_interp_report")) {
		return ToBool(configuration->backtrackInterpReport);
	}
	if (EqualsIgnoreCase(name, "cs2ac_trigger_pattern_enabled")) {
		return ToBool(configuration->triggerPatternEnabled);
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_enabled")) {
		return ToBool(configuration->rcsEnabled);
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_report")) {
		return ToBool(configuration->rcsReport);
	}
	// The rest use the live debug/boolean resolver (console changes included).
	return settings::DebugFlag(name);
}

f32 FloatSetting(const char* name, f32 defaultValue)
{
	if (!configuration || !name) {
		return defaultValue;
	}
	const auto found = configuration->numericValues.find(name);
	if (found == configuration->numericValues.end()) {
		return defaultValue;
	}
	char* end = nullptr;
	const f32 value = std::strtof(found->second.c_str(), &end);
	return end != found->second.c_str() && std::isfinite(value) ? value : defaultValue;
}

int IntSetting(const char* name, int defaultValue)
{
	if (!configuration || !name) {
		return defaultValue;
	}
	const auto found = configuration->numericValues.find(name);
	if (found == configuration->numericValues.end()) {
		return defaultValue;
	}
	char* end = nullptr;
	const long value = std::strtol(found->second.c_str(), &end, 10);
	return end != found->second.c_str() ? static_cast<int>(value) : defaultValue;
}
const char* GetWebhookUrl() { return configuration ? configuration->webhookUrl.c_str() : ""; }
const char* GetWebhookRoleId() { return configuration ? configuration->webhookRoleId.c_str() : ""; }
const char* GetWebhookServerAddress() { return configuration ? configuration->webhookServerAddress.c_str() : ""; }
const char* GetWebhookLogoUrl() { return configuration ? configuration->webhookLogoUrl.c_str() : ""; }
const char* GetLanguage() { return configuration ? configuration->language.c_str() : "en"; }
bool AllowSvCheatsTesting() { return configuration && ToBool(configuration->allowSvCheatsTesting); }

bool DebugFlagValue(const char* convarName)
{
	if (!configuration || !convarName) {
		return false;
	}
	const auto found = configuration->debugValues.find(convarName);
	return found != configuration->debugValues.end() && ToBool(found->second);
}

namespace {
std::function<bool(const char*)>& DebugResolver()
{
	static std::function<bool(const char*)> resolver;
	return resolver;
}
} // namespace

void SetDebugResolver(std::function<bool(const char*)> resolver)
{
	DebugResolver() = std::move(resolver);
}

bool DebugFlag(const char* convarName)
{
	// Live ConVar first (console toggles), then the configured value.
	if (DebugResolver() && convarName) {
		return DebugResolver()(convarName);
	}
	return DebugFlagValue(convarName);
}

void MarkConfigReloaded() { BumpRevisionInternal(); }

void BumpRevision()
{
	BumpRevisionInternal();
	detectionMaskDirty = true;
}

void SetDetectionSetting(DetectionType detection, bool value)
{
	if (!configuration) {
		return;
	}
	const auto index = static_cast<std::size_t>(detection);
	if (index < static_cast<std::size_t>(DetectionType::Count)) {
		configuration->detectionSettings[index] = value ? "1" : "0";
		BumpRevision();
	}
}

void SetEnabled(bool value)
{
	if (configuration) {
		configuration->enabled = value ? "1" : "0";
		BumpRevision();
	}
}

void SetAnnouncements(bool chat, bool center)
{
	if (configuration) {
		configuration->chatAnnouncements = chat ? "1" : "0";
		configuration->centerAnnouncements = center ? "1" : "0";
	}
}

void SetWhitelist(const char* value)
{
	if (!configuration) {
		return;
	}
	configuration->whitelist = value ? value : "";
	RebuildWhitelist(configuration->whitelist);
	BumpRevisionInternal();
}

// settings::SetBool / SetString — the config loader's entry points, keyed by ConVar name.
void SetBool(const char* name, bool value)
{
	if (!configuration || !name) {
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_enabled")) {
		SetEnabled(value);
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_chat_announcements")) {
		configuration->chatAnnouncements = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_center_announcements")) {
		configuration->centerAnnouncements = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_auto_update")) {
		configuration->automaticUpdates = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_join_watermark")) {
		configuration->joinWatermark = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_tick_integrity_enabled")) {
		configuration->tickIntegrityEnabled = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_require_forged")) {
		configuration->backtrackRequireForged = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_interp_report")) {
		configuration->backtrackInterpReport = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_trigger_pattern_enabled")) {
		configuration->triggerPatternEnabled = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_enabled")) {
		configuration->rcsEnabled = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_report")) {
		configuration->rcsReport = value ? "1" : "0";
		return;
	}
	if (EqualsIgnoreCase(name, "cs2ac_allow_sv_cheats_testing")) {
		configuration->allowSvCheatsTesting = value ? "1" : "0";
		return;
	}
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index) {
		if (EqualsIgnoreCase(name, DetectionSettingName(static_cast<DetectionType>(index)))) {
			SetDetectionSetting(static_cast<DetectionType>(index), value);
			return;
		}
	}
	for (const auto& spec : kDebugConVars) {
		if (EqualsIgnoreCase(name, spec.name)) {
			configuration->debugValues[spec.name] = value ? "1" : "0";
			return;
		}
	}
}

void SetString(const char* name, const char* value)
{
	if (!configuration || !name) {
		return;
	}
	const std::string text = value ? value : "";
	if (EqualsIgnoreCase(name, "cs2ac_punishment_command")) {
		configuration->punishmentCommand = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_kick_command")) {
		configuration->kickCommand = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_update_repository")) {
		configuration->updateRepository = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_weak_signal_policy")) {
		configuration->weakSignalPolicy = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_ban_detections")) {
		configuration->banDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_kick_detections")) {
		configuration->kickDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_unstable_kick_detections")) {
		configuration->unstableKickDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_combo_ignore_detections")) {
		configuration->comboIgnoreDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_burst_kick_detections")) {
		configuration->burstKickDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_repeat_ban_detections")) {
		configuration->repeatBanDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_repeat_ban_needs_other")) {
		configuration->repeatBanNeedsOther = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_stream_kick_detections")) {
		configuration->streamKickDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_mass_guard_detections")) {
		configuration->massGuardDetections = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_webhook_url")) {
		configuration->webhookUrl = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_webhook_role_id")) {
		configuration->webhookRoleId = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_webhook_server_address")) {
		configuration->webhookServerAddress = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_webhook_logo_url")) {
		configuration->webhookLogoUrl = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_language")) {
		configuration->language = text;
	} else if (EqualsIgnoreCase(name, "cs2ac_whitelist")) {
		SetWhitelist(text.c_str());
	}
}

void SetInt(const char* name, int value)
{
	if (!configuration || !name) {
		return;
	}
	for (const auto& spec : kNumericConVars) {
		if (!spec.isFloat && EqualsIgnoreCase(name, spec.name)) {
			configuration->numericValues[spec.name] = std::to_string(value);
			return;
		}
	}
}

void SetFloat(const char* name, f32 value)
{
	if (!configuration || !name) {
		return;
	}
	for (const auto& spec : kNumericConVars) {
		if (spec.isFloat && EqualsIgnoreCase(name, spec.name)) {
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
			configuration->numericValues[spec.name] = buffer;
			return;
		}
	}
}

const char* RawValue(const char* name)
{
	if (!configuration || !name) {
		return "";
	}
	if (EqualsIgnoreCase(name, "cs2ac_enabled")) {
		return configuration->enabled.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_chat_announcements")) {
		return configuration->chatAnnouncements.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_center_announcements")) {
		return configuration->centerAnnouncements.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_auto_update")) {
		return configuration->automaticUpdates.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_join_watermark")) {
		return configuration->joinWatermark.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_tick_integrity_enabled")) {
		return configuration->tickIntegrityEnabled.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_require_forged")) {
		return configuration->backtrackRequireForged.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_backtrack_interp_report")) {
		return configuration->backtrackInterpReport.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_trigger_pattern_enabled")) {
		return configuration->triggerPatternEnabled.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_enabled")) {
		return configuration->rcsEnabled.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_rcs_report")) {
		return configuration->rcsReport.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_allow_sv_cheats_testing")) {
		return configuration->allowSvCheatsTesting.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_punishment_command")) {
		return configuration->punishmentCommand.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_kick_command")) {
		return configuration->kickCommand.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_update_repository")) {
		return configuration->updateRepository.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_weak_signal_policy")) {
		return configuration->weakSignalPolicy.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_ban_detections")) {
		return configuration->banDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_kick_detections")) {
		return configuration->kickDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_unstable_kick_detections")) {
		return configuration->unstableKickDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_combo_ignore_detections")) {
		return configuration->comboIgnoreDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_burst_kick_detections")) {
		return configuration->burstKickDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_repeat_ban_detections")) {
		return configuration->repeatBanDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_repeat_ban_needs_other")) {
		return configuration->repeatBanNeedsOther.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_stream_kick_detections")) {
		return configuration->streamKickDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_mass_guard_detections")) {
		return configuration->massGuardDetections.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_webhook_url")) {
		return configuration->webhookUrl.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_webhook_role_id")) {
		return configuration->webhookRoleId.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_webhook_server_address")) {
		return configuration->webhookServerAddress.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_webhook_logo_url")) {
		return configuration->webhookLogoUrl.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_language")) {
		return configuration->language.c_str();
	}
	if (EqualsIgnoreCase(name, "cs2ac_whitelist")) {
		return configuration->whitelist.c_str();
	}
	for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(DetectionType::Count); ++index) {
		if (EqualsIgnoreCase(name, DetectionSettingName(static_cast<DetectionType>(index)))) {
			return configuration->detectionSettings[index].c_str();
		}
	}
	for (const auto& spec : kDebugConVars) {
		if (EqualsIgnoreCase(name, spec.name)) {
			const auto found = configuration->debugValues.find(spec.name);
			return found == configuration->debugValues.end() ? "0" : found->second.c_str();
		}
	}
	for (const auto& spec : kNumericConVars) {
		if (EqualsIgnoreCase(name, spec.name)) {
			const auto found = configuration->numericValues.find(spec.name);
			return found == configuration->numericValues.end() ? spec.defaultValue : found->second.c_str();
		}
	}
	return "";
}

} // namespace settings

// ---------------------------------------------------------------------------------------------
// cs2ac.cfg parser
//
// The engine's exec format: one `name value` per line, `//` comments, optional double quotes around
// strings, values may contain spaces when quoted. The trailing `cs2ac_config_loaded` line has no
// value and is reported as such (name set, value empty) so the caller can treat it as the marker.
// ---------------------------------------------------------------------------------------------

std::vector<ConfigLine> ParseConfigFile(const std::string& contents)
{
	std::vector<ConfigLine> lines;
	std::istringstream stream(contents);
	std::string raw;
	u32 lineNumber = 0;
	while (std::getline(stream, raw)) {
		++lineNumber;
		// Strip trailing CR (the shipped files are CRLF/LF mixed).
		if (!raw.empty() && raw.back() == '\r') {
			raw.pop_back();
		}
		// Trim leading whitespace.
		std::size_t start = raw.find_first_not_of(" \t");
		if (start == std::string::npos) {
			continue;
		}
		// Comments (full-line or trailing, outside quotes).
		bool inQuotes = false;
		std::size_t commentAt = std::string::npos;
		for (std::size_t index = start; index + 1 < raw.size(); ++index) {
			if (raw[index] == '"') {
				inQuotes = !inQuotes;
			} else if (!inQuotes && raw[index] == '/' && raw[index + 1] == '/') {
				commentAt = index;
				break;
			}
		}
		std::string line = raw.substr(start, commentAt == std::string::npos ? std::string::npos : commentAt - start);
		while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		// name [value]
		const std::size_t split = line.find_first_of(" \t");
		ConfigLine entry;
		entry.line = lineNumber;
		entry.name = split == std::string::npos ? line : line.substr(0, split);
		if (split != std::string::npos) {
			std::string value = line.substr(split);
			const std::size_t valueStart = value.find_first_not_of(" \t");
			value = valueStart == std::string::npos ? std::string {} : value.substr(valueStart);
			// Strip one layer of surrounding quotes (the cfg quoting).
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
				value = value.substr(1, value.size() - 2);
			}
			entry.value = value;
		}
		lines.push_back(std::move(entry));
	}
	return lines;
}

// cs2ac.cpp CheckCommandTemplate (identical output).
int CheckCommandTemplate(const char* settingName, const char* commandTemplate, bool requireSteamId)
{
	auto log = [](const std::string& message) { std::printf("%s\n", message.c_str()); };
	if (!commandTemplate || !*commandTemplate) {
		log(FormatString("[CS2AC] Review %s: it is empty, so this punishment is disabled.", settingName));
		return 1;
	}

	int findings = 0;
	bool hasSteamId = false;
	bool hasUserId = false;
	const std::string command = commandTemplate;
	if ((command.rfind("mm_ban", 0) == 0 || command.rfind("!ban", 0) == 0) && command.find("{userid}") != std::string::npos) {
		log(FormatString("[CS2AC] Review %s: mm_ban with {userid} is unsafe - admin_system reads short numbers as a player slot. Use "
						 "{steamid64}.",
						 settingName));
		++findings;
	}
	if (command.front() == '"' || command.back() == '"') {
		log(FormatString("[CS2AC] Review %s: the command itself starts or ends with a quote. Keep only the cfg value's outer quotes.", settingName));
		++findings;
	}
	for (size_t position = 0; position < command.size();) {
		const size_t open = command.find('{', position);
		const size_t close = command.find('}', position);
		if (close != std::string::npos && (open == std::string::npos || close < open)) {
			log(FormatString("[CS2AC] Review %s: it contains an unmatched closing brace.", settingName));
			++findings;
			position = close + 1;
			continue;
		}
		if (open == std::string::npos) {
			break;
		}
		const size_t placeholderEnd = command.find('}', open + 1);
		if (placeholderEnd == std::string::npos) {
			log(FormatString("[CS2AC] Review %s: it contains an unmatched opening brace.", settingName));
			++findings;
			break;
		}

		const std::string placeholder = command.substr(open, placeholderEnd - open + 1);
		hasSteamId |= placeholder == "{steamid64}";
		hasUserId |= placeholder == "{userid}";
		if (placeholder != "{steamid64}" && placeholder != "{userid}" && placeholder != "{detection}") {
			log(FormatString("[CS2AC] Review %s: %s is not a supported placeholder.", settingName, SanitizeConsoleText(placeholder.c_str()).c_str()));
			++findings;
		}
		position = placeholderEnd + 1;
	}

	if (requireSteamId && !hasSteamId) {
		log(FormatString("[CS2AC] Review %s: permanent bans need the {steamid64} placeholder.", settingName));
		++findings;
	}
	if (!requireSteamId && !hasSteamId && !hasUserId) {
		log(FormatString("[CS2AC] Review %s: kicks need either {userid} or {steamid64}.", settingName));
		++findings;
	}

	// The engine's command length limit (CCommand::MaxCommandLength()).
	constexpr size_t kMaxCommandLength = 512;
	std::string expanded = command;
	ReplaceAll(expanded, "{steamid64}", "18446744073709551615");
	ReplaceAll(expanded, "{userid}", "2147483647");
	ReplaceAll(expanded, "{detection}", "IRREGULAR BEHAVIOR");
	if (expanded.size() >= kMaxCommandLength) {
		log(FormatString("[CS2AC] Review %s: its expanded command can exceed the engine limit.", settingName));
		++findings;
	}
	return findings;
}

} // namespace cs2ac
