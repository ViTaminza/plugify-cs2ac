// detection_log.cpp — fork detection_log.cpp on the Plugify side.
//
// One JSON object per line, UTC timestamps and the server tick, appended with
// open/append/close per record so the files survive crashes, rotation and an operator moving them.

#include "detection_log.h"

#include <plg/plugin.hpp>
#include <s2sdk/s2sdk.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace cs2ac::detection_log {

namespace {

std::filesystem::path g_logDirectory;
bool g_ready = false;

const char* OutcomeName(announce::DetectionOutcome outcome)
{
	switch (outcome) {
	case announce::DetectionOutcome::PunishmentSent: return "punishment_sent";
	case announce::DetectionOutcome::ReportOnly: return "report_only";
	case announce::DetectionOutcome::Whitelisted: return "whitelisted";
	case announce::DetectionOutcome::PunishmentDisabled: return "punishment_disabled";
	case announce::DetectionOutcome::IdentityUnavailable: return "identity_unavailable";
	case announce::DetectionOutcome::AlreadyPunished: return "already_punished";
	case announce::DetectionOutcome::CommandTooLong: return "command_too_long";
	case announce::DetectionOutcome::CommandServiceUnavailable: return "command_service_unavailable";
	case announce::DetectionOutcome::NetworkUnstable: return "network_unstable";
	}
	return "unknown";
}

void UtcNow(char* iso, std::size_t isoSize, char* day, std::size_t daySize)
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	std::tm utc {};
	gmtime_r(&seconds, &utc);
	char base[32];
	std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc);
	std::snprintf(iso, isoSize, "%s.%03dZ", base, static_cast<int>(millis));
	std::strftime(day, daySize, "%Y-%m-%d", &utc);
}

void AppendLine(const char* prefix, const char* day, const std::string& line)
{
	std::ofstream file(g_logDirectory / (std::string(prefix) + day + ".jsonl"), std::ios::app | std::ios::binary);
	if (file) {
		file << line;
	}
}

struct LineContext {
	char iso[40] {};
	char day[16] {};
	std::string map;
	int tick {-1};
};

LineContext Context()
{
	LineContext context;
	UtcNow(context.iso, sizeof(context.iso), context.day, sizeof(context.day));
	context.tick = engine::CurrentTick();
	if (engine::GameReady() && __s2sdk_GetCurrentMap) {
		try {
			context.map = s2sdk::GetCurrentMap().c_str();
		} catch (...) {
		}
	}
	return context;
}

} // namespace

void Init()
{
	g_ready = false;
	// Keep the returned string alive: a c_str() of the temporary would dangle.
	const std::string logsDir = plg::GetLogsDir ? std::string(plg::GetLogsDir()) : std::string();
	if (logsDir.empty()) {
		LogWarning("Detection log disabled: the Plugify logs directory is unknown.");
		return;
	}
	std::error_code error;
	g_logDirectory = std::filesystem::path(logsDir) / "cs2ac";
	std::filesystem::create_directories(g_logDirectory, error);
	if (error) {
		LogWarning("Detection log disabled: cannot create %s (%s).", g_logDirectory.string().c_str(), error.message().c_str());
		return;
	}
	g_ready = true;
	Log("Detections and telemetry are logged to %s", g_logDirectory.string().c_str());
}

void Shutdown()
{
	g_ready = false;
	g_logDirectory.clear();
}

bool Ready()
{
	return g_ready;
}

void Write(const char* detection, PlayerRef* player, u64 steamId, const std::string& evidenceEnglish, const char* severity,
		   announce::DetectionOutcome outcome)
{
	// No client queries in the map-loading window (PORT_PLAN §8b / PITFALLS B.2).
	if (!g_ready || !engine::GameReady() || !detection || !player) {
		return;
	}
	const LineContext context = Context();

	f32 pingMs = -1.0f;
	f32 lossIn = -1.0f;
	f32 lossOut = -1.0f;
	f32 chokeIn = -1.0f;
	if (engine::GameReady()) {
		const engine::NetStats network = engine::ClientNetStats(player->index);
		if (network.pingAvailable) {
			pingMs = network.pingSeconds * 1000.0f;
		}
		if (network.lossChokeAvailable) {
			lossIn = network.incomingLoss;
			lossOut = network.outgoingLoss;
			chokeIn = network.incomingChoke;
		}
	}

	char numbers[256];
	std::snprintf(numbers, sizeof(numbers),
				  "\"tick\":%d,\"slot\":%d,\"steamid64\":\"%llu\",\"ping_ms\":%.1f,\"loss_in\":%.3f,\"loss_out\":%.3f,\"choke_in\":%.3f",
				  context.tick, player->GetPlayerSlot(), static_cast<unsigned long long>(steamId), static_cast<double>(pingMs),
				  static_cast<double>(lossIn), static_cast<double>(lossOut), static_cast<double>(chokeIn));

	std::string line;
	line.reserve(512 + evidenceEnglish.size());
	line += "{\"ts\":" + detection::JsonString(context.iso);
	line += ",\"map\":" + detection::JsonString(context.map.c_str());
	line += std::string(",") + numbers;
	line += ",\"name\":" + detection::JsonString(player->GetName());
	line += ",\"detection\":" + detection::JsonString(detection);
	line += ",\"severity\":" + detection::JsonString(severity ? severity : "");
	line += ",\"outcome\":" + detection::JsonString(OutcomeName(outcome));
	line += ",\"evidence\":" + detection::JsonString(evidenceEnglish.c_str());
	line += "}\n";
	AppendLine("detections-", context.day, line);
}

void WriteTelemetry(const char* type, PlayerRef* player, const std::string& fields)
{
	if (!g_ready || !engine::GameReady() || !type || !player) {
		return;
	}
	const LineContext context = Context();
	std::string line;
	line.reserve(256 + fields.size());
	line += "{\"ts\":" + detection::JsonString(context.iso);
	line += ",\"type\":" + detection::JsonString(type);
	line += ",\"map\":" + detection::JsonString(context.map.c_str());
	line += ",\"tick\":" + std::to_string(context.tick);
	line += ",\"steamid64\":\"" + std::to_string(player->GetSteamId64(false)) + "\"";
	line += ",\"name\":" + detection::JsonString(player->GetName());
	line += fields;
	line += "}\n";
	AppendLine("telemetry-", context.day, line);
}

} // namespace cs2ac::detection_log
