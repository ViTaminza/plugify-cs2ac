// cs2ac — announcements and the punishment command (utils/utils_print.cpp + cs2ac.cpp
// HandleDetection).
//
// The original built TextMsg payloads by hand for chat (with its {red}/{default} colour codes) and
// fired show_survival_respawn_status for the center-screen HTML, re-sending it every 100 ms for the
// duration. s2sdk exposes exactly those two mechanisms (PrintToChatColored* runs the same colour
// conversion, PrintCentreHtml* fires show_survival_respawn_status), so the port keeps the same
// output and the same re-send timer (driven from Plugin::OnUpdate instead of the engine timer).

#include "cs2ac.hpp"

#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

namespace cs2ac {
namespace announce {

namespace {

std::string EscapeChatMarkup(const char* text)
{
	// The original prefixed the player name with a raw HUD control byte (0x08) and stripped control
	// characters; the byte is what colours the name inside the TextMsg parameter.
	std::string escaped;
	for (const unsigned char* current = reinterpret_cast<const unsigned char*>(text ? text : ""); *current; ++current) {
		if (*current < 0x20 || *current == 0x7f) {
			escaped += ' ';
		} else {
			escaped += static_cast<char>(*current);
		}
	}
	return escaped;
}

std::string EscapeHtml(const char* text)
{
	std::string escaped;
	for (const unsigned char* current = reinterpret_cast<const unsigned char*>(text ? text : ""); *current; ++current) {
		switch (*current) {
		case '&': escaped += "&amp;"; break;
		case '<': escaped += "&lt;"; break;
		case '>': escaped += "&gt;"; break;
		case '"': escaped += "&quot;"; break;
		case '\'': escaped += "&#39;"; break;
		default:
			if (*current < 0x20 || *current == 0x7f) {
				escaped += ' ';
			} else {
				escaped += static_cast<char>(*current);
			}
			break;
		}
	}
	return escaped;
}

// utils_print.cpp center-message state.
std::string detectionHtml;
std::chrono::steady_clock::time_point detectionHtmlExpires;
bool detectionHtmlTimerRunning;
bool detectionHtmlIsDetection;
bool detectionHtmlAlwaysVisible;
int detectionHtmlBroadcasts;
int detectionHtmlDuration = 5;
constexpr int maximumDetectionHtmlBroadcasts = 51;
// The original re-sent the center HTML from a 100 ms timer; the port pumps from the frame callback
// and keeps the same period (a per-frame send would be up to 64 broadcasts per second and would
// exhaust the broadcast budget in under a second instead of the intended five).
std::chrono::steady_clock::time_point nextHtmlResend {};
constexpr auto kHtmlResendInterval = std::chrono::milliseconds(100);

// Fork cs2ac_announce_cooldown state.
struct AnnounceThrottleEntry {
	std::string detection;
	DetectionOutcome outcome {DetectionOutcome::PunishmentSent};
	f64 last {};
};
std::unordered_map<u64, AnnounceThrottleEntry> announceThrottle;

f64 AnnounceNowSeconds()
{
	return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Both sends are gated on a live map: the reset paths run on map start/shutdown, where a network
// message must not be posted (there is no client to receive it and the engine is tearing the world
// down).
void SendDetectionHtml(const char* message, int duration)
{
	if (!__s2sdk_PrintCentreHtmlAll || !engine::GameReady()) {
		return;
	}
	s2sdk::PrintCentreHtmlAll(message ? message : "<font></font>", duration);
}

void SendDetectionHtmlTo(int slot, const char* message, int duration)
{
	if (!__s2sdk_PrintCentreHtml || !engine::GameReady()) {
		return;
	}
	s2sdk::PrintCentreHtml(slot, message ? message : "<font></font>", duration);
}

// utils_print.cpp ShowCenterMessage.
void ShowCenterMessage(std::string message, bool isDetection = false, bool alwaysVisible = false, int duration = 5)
{
	const auto now = std::chrono::steady_clock::now();
	if (!isDetection && detectionHtmlIsDetection && now < detectionHtmlExpires) {
		return;
	}
	detectionHtml = std::move(message);
	detectionHtmlExpires = now + std::chrono::seconds(duration);
	detectionHtmlIsDetection = isDetection;
	detectionHtmlAlwaysVisible = alwaysVisible;
	detectionHtmlDuration = duration;
	SendDetectionHtml(detectionHtml.c_str(), detectionHtmlDuration);
	detectionHtmlBroadcasts = 1;
	detectionHtmlTimerRunning = true;
}

} // namespace

std::string OutcomeText(DetectionOutcome outcome)
{
	switch (outcome) {
	case DetectionOutcome::PunishmentSent:
		return localization::Get("announcement.outcome.punishment_sent", " and sent the punishment.");
	case DetectionOutcome::ReportOnly:
		return localization::Get("announcement.outcome.report_only", ", but this finding is report-only.");
	case DetectionOutcome::Whitelisted:
		return localization::Get("announcement.outcome.whitelisted", ", but they are whitelisted.");
	case DetectionOutcome::PunishmentDisabled:
		return localization::Get("announcement.outcome.punishment_disabled", ", but punishment is disabled.");
	case DetectionOutcome::IdentityUnavailable:
		return localization::Get("announcement.outcome.identity_unavailable", ", but their identity is not ready yet.");
	case DetectionOutcome::AlreadyPunished:
		return localization::Get("announcement.outcome.already_punished", " and the punishment was already sent.");
	case DetectionOutcome::CommandTooLong:
		return localization::Get("announcement.outcome.command_too_long", ", but the punishment command is too long.");
	case DetectionOutcome::CommandServiceUnavailable:
		return localization::Get("announcement.outcome.command_service_unavailable", ", but the server command service is unavailable.");
	case DetectionOutcome::NetworkUnstable:
		return localization::Get("announcement.outcome.network_unstable", ", but no punishment was sent because their connection was unstable.");
	}
	return ".";
}

void AnnounceDetection(const char* detection, const char* playerName, DetectionOutcome outcome)
{
	if (!detection || !playerName) {
		return;
	}

	const std::string outcomeText = OutcomeText(outcome);
	std::string displayDetection = detection;
	for (char& character : displayDetection) {
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	if (settings::ShowChatAnnouncements() && __s2sdk_PrintToChatColoredAll && engine::GameReady()) {
		const std::string safeChatDetection = EscapeChatMarkup(displayDetection.c_str());
		const std::string safeChatPlayerName = EscapeChatMarkup(playerName);
		const std::string chatBody = localization::Format("announcement.detected", "detected {detection} on {player}{outcome}",
														  {{"detection", std::string("{lime}") + safeChatDetection + "{default}"},
														   {"player", std::string("{grey}") + safeChatPlayerName + "{default}"},
														   {"outcome", outcomeText}})
										  .localized;
		s2sdk::PrintToChatColoredAll("{red}[CS2AC]{default} " + plg::string(chatBody.c_str()));
	}

	if (!settings::ShowCenterAnnouncements()) {
		return;
	}

	const std::string safeDetection = EscapeHtml(displayDetection.c_str());
	const std::string safePlayerName = EscapeHtml(playerName);
	std::string centerBody = EscapeHtml(localization::Get("announcement.detected", "detected {detection} on {player}{outcome}").c_str());
	ReplaceAll(centerBody, "{detection}", "<span color='#00FF00'>" + safeDetection + "</span>");
	ReplaceAll(centerBody, "{player}", "<span color='#B0B0B0'>" + safePlayerName + "</span>");
	ReplaceAll(centerBody, "{outcome}", EscapeHtml(outcomeText.c_str()));
	ShowCenterMessage("<span class='fontSize-l'><span color='#FF0000'>[CS2AC]</span> <span color='#FFFFFF'>" + centerBody + "</span></span>", true);
}

void AnnounceDetectionThrottled(u64 steamId, const char* detection, const char* playerName, DetectionOutcome outcome, int cooldownSeconds)
{
	if (!detection || !playerName) {
		return;
	}
	if (steamId != 0 && cooldownSeconds > 0) {
		const f64 now = AnnounceNowSeconds();
		auto& entry = announceThrottle[steamId];
		if (entry.detection == detection && entry.outcome == outcome && now - entry.last < static_cast<f64>(cooldownSeconds)) {
			return;
		}
		entry.detection = detection;
		entry.outcome = outcome;
		entry.last = now;
	}
	AnnounceDetection(detection, playerName, outcome);
}

void AnnounceTest()
{
	static constexpr const char* text = "This is a test. No player was detected or punished.";
	const std::string localized = localization::Get("announcement.test", text);
	// The same live-map gate as AnnounceDetection: a `cs2ac_test_announcement` from rcon can land
	// between the map end and the next map start, where s2sdk's client queries (the broadcast
	// walks the client list) crash.
	if (settings::ShowChatAnnouncements() && __s2sdk_PrintToChatColoredAll && engine::GameReady()) {
		s2sdk::PrintToChatColoredAll(plg::string(("{red}[CS2AC]{default} " + localized).c_str()));
	} else {
		ConsoleMessage("The chat test was skipped because chat announcements are disabled.");
	}

	if (settings::ShowCenterAnnouncements()) {
		ShowCenterMessage("<span class='fontSize-l'><span color='#FF0000'>[CS2AC]</span> <span color='#FFFFFF'>" + EscapeHtml(localized.c_str())
						  + "</span></span>");
	} else {
		ConsoleMessage("The center-screen test was skipped because center announcements are disabled.");
	}
	ConsoleMessage("Test announcement finished: %s", text);
}

void AnnounceWatermarkTo(int slot, bool centerOnly)
{
	if (slot < 0 || slot >= MAXPLAYERS) {
		return;
	}
	if (!centerOnly) {
		if (settings::ShowChatAnnouncements() && __s2sdk_PrintToChatColored && engine::GameReady()) {
			const std::string chatBody = localization::Watermark({{"author", "{grey}karola3vax{default}"}}).localized;
			s2sdk::PrintToChatColored(slot, plg::string(("{red}[CS2AC]{default} " + chatBody).c_str()));
		}
		return;
	}

	std::string centerBody = EscapeHtml(localization::Watermark().localized.c_str());
	ReplaceAll(centerBody, "{author}", "<span color='#B0B0B0'>karola3vax</span>");
	const std::string message =
		"<span class='fontSize-l'><span color='#FF0000'>[CS2AC]</span> <span color='#FFFFFF'>" + centerBody + "</span></span>";
	SendDetectionHtmlTo(slot, message.c_str(), 3);
}

void ClearWatermarkFor(int slot)
{
	if (slot >= 0 && slot < MAXPLAYERS) {
		SendDetectionHtmlTo(slot, "<font></font>", 1);
	}
}

bool IsDetectionAnnouncementActive()
{
	return detectionHtmlIsDetection && std::chrono::steady_clock::now() < detectionHtmlExpires;
}

void ResetDetectionAnnouncement()
{
	if (!detectionHtml.empty()) {
		SendDetectionHtml("<font></font>", 1);
	}
	detectionHtml.clear();
	detectionHtmlExpires = {};
	detectionHtmlTimerRunning = false;
	detectionHtmlIsDetection = false;
	detectionHtmlAlwaysVisible = false;
	detectionHtmlBroadcasts = 0;
	nextHtmlResend = {};
	announceThrottle.clear();
}

// The original's StartTimer(ResendDetectionHtml, 0.1, ...) — pumped from OnGameFrame but throttled
// to the same 100 ms period: sending on every frame would be up to 64 broadcasts per second (and
// would burn the 51-broadcast budget in under a second instead of the intended five).
void OnGameFrame()
{
	if (!detectionHtmlTimerRunning) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < nextHtmlResend) {
		return;
	}
	nextHtmlResend = now + kHtmlResendInterval;
	if (!detectionHtmlAlwaysVisible && !settings::ShowCenterAnnouncements()) {
		detectionHtml.clear();
		detectionHtmlTimerRunning = false;
		detectionHtmlIsDetection = false;
		detectionHtmlAlwaysVisible = false;
		SendDetectionHtml("<font></font>", 1);
		return;
	}
	if (detectionHtml.empty()) {
		detectionHtmlTimerRunning = false;
		return;
	}
	if (now >= detectionHtmlExpires) {
		detectionHtml.clear();
		detectionHtmlTimerRunning = false;
		detectionHtmlIsDetection = false;
		detectionHtmlAlwaysVisible = false;
		SendDetectionHtml("<font></font>", 1);
		return;
	}
	if (detectionHtmlBroadcasts < maximumDetectionHtmlBroadcasts) {
		SendDetectionHtml(detectionHtml.c_str(), detectionHtmlDuration);
		++detectionHtmlBroadcasts;
	}
	// The next 100 ms re-send happens on the following frames (the original used a timer); the
	// pump runs every frame and the broadcast cap keeps the traffic identical.
}

} // namespace announce
} // namespace cs2ac
