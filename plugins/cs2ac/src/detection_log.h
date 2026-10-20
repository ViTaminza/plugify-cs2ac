// Append-only JSONL log of every CS2AC detection and the raw telemetry rows the fork's analysis
// tooling consumes (fork detection_log.cpp). The fork wrote to csgo/addons/cs2ac/logs; the Plugify
// port writes to <plugify logs>/cs2ac with the same file names and JSON schema.

#pragma once

#include "cs2ac.hpp"

#include <string>

namespace cs2ac::detection_log {

// Call once after the plugin is loaded (creates the directory).
void Init();
void Shutdown();
bool Ready();

// severity: "ban" | "tempban" | "kick" | "report"; empty when not decided yet.
void Write(const char* detection, PlayerRef* player, u64 steamId, const std::string& evidenceEnglish, const char* severity,
		   announce::DetectionOutcome outcome);

// Raw measurements for offline analysis, one JSON object per line. `fields` must be a comma-prefixed
// list of already-encoded JSON members, e.g. ",\"shots\":12,\"r\":0.97".
void WriteTelemetry(const char* type, PlayerRef* player, const std::string& fields);

} // namespace cs2ac::detection_log
