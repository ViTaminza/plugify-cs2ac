// cs2ac — the Discord report queue (webhook.cpp).
//
// The payload building, the URL/role/logo validation, the queue with its overflow rule, the
// one-retry policy, the 429 Retry-After back-off, the avatar lookup through Steam's public profile
// XML and the "disabled after an error" behaviour are the original's, line for line.
//
// Transport: the original used CSteamGameServerAPIContext + ISteamHTTP and a CCallResult. s2sdk
// does not expose HTTP, so this file re-creates that interface on the server's own
// libsteam_api.so (dlopen + the documented SteamAPI_ISteamHTTP_* C accessors + a CCallbackBase-
// layout call result), the same technique the other ported plugins use for Steam HTTP. Requests
// are asynchronous: Report() only enqueues, and the completed response is handled from the
// plugin's per-frame pump - nothing here ever blocks the game thread.
//
// Deviation (documented in PORT_PLAN.md §6): the original's automatic server address came from
// the engine's GetPublicIP; without a verified accessor for it the port uses
// cs2ac_webhook_server_address and omits the field (with a one-time log) when it is empty.

#include "cs2ac.hpp"

#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <dlfcn.h>

#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace cs2ac {

namespace {

constexpr std::size_t maximumQueueSize = 64;
constexpr std::size_t maximumAvatarCacheSize = 256;
constexpr uint32_t maximumProfileSize = 64 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(45);

std::string JsonEscape(std::string_view value)
{
	std::string result;
	result.reserve(value.size() + 8);
	for (const unsigned char character : value) {
		switch (character) {
		case '"': result += "\\\""; break;
		case '\\': result += "\\\\"; break;
		case '\b': result += "\\b"; break;
		case '\f': result += "\\f"; break;
		case '\n': result += "\\n"; break;
		case '\r': result += "\\r"; break;
		case '\t': result += "\\t"; break;
		default:
			if (character < 32 || character == 127) {
				result += '?';
			} else {
				result += static_cast<char>(character);
			}
		}
	}
	return result;
}

std::string MarkdownEscape(std::string_view value)
{
	std::string result;
	result.reserve(value.size() + 8);
	for (const char character : value) {
		if (std::strchr("\\`*_{}[]()<>#+-.!|~", character)) {
			result += '\\';
		}
		result += character;
	}
	return result;
}

std::string Limit(std::string value, std::size_t maximum)
{
	if (value.size() <= maximum) {
		return value;
	}
	value.resize(maximum - 3);
	while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xc0) == 0x80) {
		value.pop_back();
	}
	value += "...";
	return value;
}

std::string CompleteSentence(std::string value)
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	if (!value.empty() && !std::strchr(".!?", value.back())) {
		value += '.';
	}
	return value;
}

std::string AvatarFromProfileXml(std::string_view xml)
{
	constexpr std::string_view opening = "<avatarFull><![CDATA[";
	constexpr std::string_view closing = "]]></avatarFull>";
	const std::size_t start = xml.find(opening);
	if (start == std::string_view::npos) {
		return "";
	}
	const std::size_t valueStart = start + opening.size();
	const std::size_t end = xml.find(closing, valueStart);
	if (end == std::string_view::npos) {
		return "";
	}
	const std::string url(xml.substr(valueStart, end - valueStart));
	const bool steamStatic = url.rfind("https://avatars.", 0) == 0 && url.find(".steamstatic.com/") != std::string::npos;
	const bool steamCdn = url.rfind("https://steamcdn-a.akamaihd.net/steamcommunity/public/images/avatars/", 0) == 0;
	return steamStatic || steamCdn ? url : "";
}

std::string UtcTimestamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm utc {};
	gmtime_r(&now, &utc);
	std::ostringstream output;
	output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
	return output.str();
}

// --- Steam HTTP transport ---------------------------------------------------------------

// HTTPRequestCompleted_t (isteamhttp.h) — 4 bytes handle, padding, 8 bytes context value, bool,
// padding, status code, body size.
struct HTTPRequestCompleted_t {
	enum { k_iCallback = 2102 }; // k_iSteamHTTPCallbacks (2101) + 1
	uint32_t m_hRequest;
	uint64_t m_ulContextValue;
	bool m_bRequestSuccessful;
	int32_t m_eStatusCode;
	uint32_t m_unBodySize;
};
static_assert(sizeof(HTTPRequestCompleted_t) == 32, "HTTPRequestCompleted_t layout");
static_assert(offsetof(HTTPRequestCompleted_t, m_eStatusCode) == 20, "HTTPRequestCompleted_t layout");

// Valve's CCallbackBase: the vtable has to start with Run(void*), Run(void*, bool, SteamAPICall_t)
// and GetCallbackSizeBytes(), and the two data members sit right after the vptr.
class CallbackBase {
public:
	virtual void Run(void* pvParam) = 0;
	virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
	virtual int GetCallbackSizeBytes() = 0;

	enum {
		k_ECallbackFlagsRegistered = 0x01,
		k_ECallbackFlagsGameServer = 0x02,
	};
	uint8_t m_nCallbackFlags;
	int32_t m_iCallback;

protected:
	CallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
	~CallbackBase() = default;
};

struct SteamHttpApi {
	void* module {};
	void* (*getHttp)() {};
	uint32_t (*CreateHTTPRequest)(void*, int32_t, const char*) {};
	bool (*SetHTTPRequestHeaderValue)(void*, uint32_t, const char*, const char*) {};
	bool (*SetHTTPRequestRawPostBody)(void*, uint32_t, const char*, uint8_t*, uint32_t) {};
	bool (*SendHTTPRequest)(void*, uint32_t, uint64_t*) {};
	bool (*GetHTTPResponseBodySize)(void*, uint32_t, uint32_t*) {};
	bool (*GetHTTPResponseBodyData)(void*, uint32_t, uint8_t*, uint32_t) {};
	bool (*GetHTTPResponseHeaderSize)(void*, uint32_t, const char*, uint32_t*) {};
	bool (*GetHTTPResponseHeaderValue)(void*, uint32_t, const char*, uint8_t*, uint32_t) {};
	bool (*ReleaseHTTPRequest)(void*, uint32_t) {};
	void (*RegisterCallResult)(CallbackBase*, uint64_t) {};
	void (*UnregisterCallResult)(CallbackBase*, uint64_t) {};
	void* http {};
};

SteamHttpApi g_steam;
bool g_symbolFailure {};
bool g_loadLogged {};

template<typename Fn>
Fn ResolveSymbol(void* module, const char* name)
{
	return reinterpret_cast<Fn>(dlsym(module, name));
}

bool ResolveSteamHttp()
{
	if (g_symbolFailure) {
		return false;
	}
	if (g_steam.getHttp == nullptr) {
		std::vector<std::string> candidates = {"libsteam_api.so"};
		if (__s2sdk_GetGameDirectory) {
			const std::string gameDir(s2sdk::GetGameDirectory());
			if (!gameDir.empty()) {
				candidates.push_back(gameDir + "/bin/linuxsteamrt64/libsteam_api.so");
				candidates.push_back(gameDir + "/../bin/linuxsteamrt64/libsteam_api.so");
			}
		}
		candidates.push_back("bin/linuxsteamrt64/libsteam_api.so");
		for (const std::string& candidate : candidates) {
			if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_NOLOAD)) {
				g_steam.module = handle;
				break;
			}
		}
		if (g_steam.module == nullptr) {
			for (const std::string& candidate : candidates) {
				if (void* handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL)) {
					g_steam.module = handle;
					break;
				}
			}
		}
		if (g_steam.module == nullptr) {
			if (!g_loadLogged) {
				g_loadLogged = true;
				LogWarning("libsteam_api.so could not be loaded; Discord webhook reports stay disabled.");
			}
			return false;
		}
		for (const char* name : {"SteamAPI_SteamGameServerHTTP_v004", "SteamAPI_SteamGameServerHTTP_v003",
								 "SteamAPI_SteamGameServerHTTP_v002"}) {
			g_steam.getHttp = ResolveSymbol<decltype(g_steam.getHttp)>(g_steam.module, name);
			if (g_steam.getHttp) {
				break;
			}
		}
		g_steam.CreateHTTPRequest = ResolveSymbol<decltype(g_steam.CreateHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_CreateHTTPRequest");
		g_steam.SetHTTPRequestHeaderValue =
			ResolveSymbol<decltype(g_steam.SetHTTPRequestHeaderValue)>(g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestHeaderValue");
		g_steam.SetHTTPRequestRawPostBody =
			ResolveSymbol<decltype(g_steam.SetHTTPRequestRawPostBody)>(g_steam.module, "SteamAPI_ISteamHTTP_SetHTTPRequestRawPostBody");
		g_steam.SendHTTPRequest = ResolveSymbol<decltype(g_steam.SendHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_SendHTTPRequest");
		g_steam.GetHTTPResponseBodySize =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseBodySize)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodySize");
		g_steam.GetHTTPResponseBodyData =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseBodyData)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseBodyData");
		g_steam.GetHTTPResponseHeaderSize =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseHeaderSize)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderSize");
		g_steam.GetHTTPResponseHeaderValue =
			ResolveSymbol<decltype(g_steam.GetHTTPResponseHeaderValue)>(g_steam.module, "SteamAPI_ISteamHTTP_GetHTTPResponseHeaderValue");
		g_steam.ReleaseHTTPRequest = ResolveSymbol<decltype(g_steam.ReleaseHTTPRequest)>(g_steam.module, "SteamAPI_ISteamHTTP_ReleaseHTTPRequest");
		g_steam.RegisterCallResult = ResolveSymbol<decltype(g_steam.RegisterCallResult)>(g_steam.module, "SteamAPI_RegisterCallResult");
		g_steam.UnregisterCallResult = ResolveSymbol<decltype(g_steam.UnregisterCallResult)>(g_steam.module, "SteamAPI_UnregisterCallResult");

		const bool complete = g_steam.getHttp && g_steam.CreateHTTPRequest && g_steam.SetHTTPRequestRawPostBody && g_steam.SendHTTPRequest
							  && g_steam.ReleaseHTTPRequest && g_steam.RegisterCallResult && g_steam.UnregisterCallResult;
		if (!complete) {
			g_symbolFailure = true;
			LogWarning("libsteam_api.so does not export the expected ISteamHTTP entry points; Discord webhook reports stay disabled.");
			g_steam.getHttp = nullptr;
			return false;
		}
	}
	if (g_steam.http == nullptr) {
		g_steam.http = g_steam.getHttp();
	}
	return g_steam.http != nullptr;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// WebhookService
// ---------------------------------------------------------------------------------------------

struct WebhookService::Impl {
	struct ReportData {
		std::string detection;
		std::string playerName;
		std::string evidence;
		std::string avatarUrl;
		std::string payload;
		u64 steamId {};
		announce::DetectionOutcome outcome {};
		unsigned retries {};
		bool avatarResolved {};
	};

	enum class RequestKind {
		None,
		Avatar,
		Discord,
	};

	std::deque<ReportData> queue;
	std::unordered_map<u64, std::string> avatarCache;
	bool disabled {};
	bool overflowWarned {};
	bool httpUnavailableWarned {};
	bool addressWarned {};
	std::chrono::steady_clock::time_point nextAttempt {};
	uint32_t request {};
	uint64_t call {};
	RequestKind requestKind {RequestKind::None};
	std::chrono::steady_clock::time_point requestStarted {};
	bool responseReady {};
	bool responseFailed {};
	int responseStatus {};
	std::string responseBody;

	void CancelRequest();
	void SendNext();
	bool SendAvatarLookup();
	void FinishAvatarLookup(std::string avatarUrl);
	void RetryOrDrop(int status);
	void Disable(const char* reason);
	void CompleteResponse(bool failed, int status, std::string body);
	std::string ServerAddress();
	std::string BuildPayload(const ReportData& report);
};

namespace {

// The call result object handed to SteamAPI_RegisterCallResult.
class WebhookCallback final : public CallbackBase {
public:
	explicit WebhookCallback(WebhookService::Impl* impl) : impl(impl)
	{
		m_nCallbackFlags = static_cast<uint8_t>(k_ECallbackFlagsGameServer);
		m_iCallback = HTTPRequestCompleted_t::k_iCallback;
	}

	void Run(void* pvParam) override { Run(pvParam, false, 0); }
	void Run(void* pvParam, bool bIOFailure, uint64_t /*hSteamAPICall*/) override
	{
		try {
			if (!impl) {
				return;
			}
			if (bIOFailure || pvParam == nullptr) {
				impl->CompleteResponse(true, 0, {});
				return;
			}
			const auto* completed = static_cast<const HTTPRequestCompleted_t*>(pvParam);
			std::string body;
			constexpr uint32_t kMaxBodySize = 4u * 1024u * 1024u;
			if (completed->m_bRequestSuccessful && completed->m_unBodySize > 0 && completed->m_unBodySize <= kMaxBodySize && g_steam.http
				&& g_steam.GetHTTPResponseBodyData) {
				body.resize(completed->m_unBodySize);
				uint32_t size = completed->m_unBodySize;
				if (!g_steam.GetHTTPResponseBodyData(g_steam.http, completed->m_hRequest, reinterpret_cast<uint8_t*>(body.data()), size)) {
					body.clear();
				}
			}
			impl->CompleteResponse(false, static_cast<int>(completed->m_eStatusCode), std::move(body));
		} catch (...) {
			// Never unwind into Steam's callback pump.
		}
	}
	int GetCallbackSizeBytes() override { return static_cast<int>(sizeof(HTTPRequestCompleted_t)); }

	WebhookService::Impl* impl {};
};

WebhookCallback g_callback {nullptr};

} // namespace

void WebhookService::Impl::CancelRequest()
{
	// Unregister first: a pending Steam call result would otherwise fire into this object after the
	// plugin (and this Impl) is gone.
	if (call != 0 && g_steam.UnregisterCallResult) {
		g_steam.UnregisterCallResult(&g_callback, call);
		call = 0;
	}
	if (request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
	}
	request = 0;
	requestKind = RequestKind::None;
	responseReady = false;
	g_callback.impl = nullptr;
}

void WebhookService::Impl::CompleteResponse(bool failed, int status, std::string body)
{
	responseReady = true;
	responseFailed = failed;
	responseStatus = status;
	responseBody = std::move(body);
}

void WebhookService::Impl::Disable(const char* reason)
{
	disabled = true;
	queue.clear();
	ConsoleMessage("Discord webhook reports are disabled until the next cs2ac_reload. %s", reason);
}

std::string WebhookService::Impl::ServerAddress()
{
	const char* overrideAddress = settings::GetWebhookServerAddress();
	if (overrideAddress && *overrideAddress) {
		return overrideAddress;
	}
	// The original asked Steam for the public IP; the port has no verified accessor for it, so the
	// field is omitted unless the operator configured it (one-time notice).
	if (!addressWarned) {
		addressWarned = true;
		ConsoleMessage("Discord reports omit the server address because cs2ac_webhook_server_address is empty.");
	}
	return "";
}

void WebhookService::Impl::RetryOrDrop(int status)
{
	if (!queue.empty() && queue.front().retries++ == 0) {
		nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		return;
	}
	if (status) {
		ConsoleMessage("A Discord webhook report failed after one retry and was dropped (HTTP %d).", status);
	} else {
		ConsoleMessage("A Discord webhook report failed after one retry and was dropped.");
	}
	if (!queue.empty()) {
		queue.pop_front();
	}
	nextAttempt = {};
}

void WebhookService::Impl::FinishAvatarLookup(std::string avatarUrl)
{
	if (queue.empty()) {
		return;
	}
	auto& report = queue.front();
	report.avatarResolved = true;
	report.avatarUrl = std::move(avatarUrl);
	if (avatarCache.size() >= maximumAvatarCacheSize) {
		avatarCache.clear();
	}
	avatarCache[report.steamId] = report.avatarUrl;
}

bool WebhookService::Impl::SendAvatarLookup()
{
	// Steam's public XML profile supplies avatarFull without requiring a Web API key.
	const std::string url = FormatString("https://steamcommunity.com/profiles/%llu?xml=1", static_cast<unsigned long long>(queue.front().steamId));
	if (!ResolveSteamHttp()) {
		return false;
	}
	request = g_steam.CreateHTTPRequest(g_steam.http, 1 /*k_EHTTPMethodGET*/, url.c_str());
	if (request == 0) {
		FinishAvatarLookup("");
		return false;
	}
	uint64_t call = 0;
	if (!g_steam.SendHTTPRequest(g_steam.http, request, &call)) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
		request = 0;
		FinishAvatarLookup("");
		return false;
	}
	requestKind = RequestKind::Avatar;
	requestStarted = std::chrono::steady_clock::now();
	responseReady = false;
	g_callback.impl = this;
	this->call = call;
	g_steam.RegisterCallResult(&g_callback, call);
	return true;
}

void WebhookService::Impl::SendNext()
{
	if (!ResolveSteamHttp()) {
		if (!httpUnavailableWarned) {
			httpUnavailableWarned = true;
			ConsoleMessage("Discord reports are waiting because Steam's HTTP service is not ready yet.");
		}
		nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		return;
	}

	if (!queue.front().avatarResolved) {
		const auto cachedAvatar = avatarCache.find(queue.front().steamId);
		if (cachedAvatar != avatarCache.end()) {
			queue.front().avatarResolved = true;
			queue.front().avatarUrl = cachedAvatar->second;
		} else if (SendAvatarLookup()) {
			return;
		}
	}
	if (queue.front().payload.empty()) {
		queue.front().payload = BuildPayload(queue.front());
	}

	std::string url = settings::GetWebhookUrl();
	url += url.find('?') == std::string::npos ? "?wait=true" : "&wait=true";
	request = g_steam.CreateHTTPRequest(g_steam.http, 3 /*k_EHTTPMethodPOST*/, url.c_str());
	if (request == 0
		|| !g_steam.SetHTTPRequestRawPostBody(g_steam.http, request, "application/json",
											 reinterpret_cast<uint8_t*>(queue.front().payload.data()),
											 static_cast<uint32_t>(queue.front().payload.size()))) {
		if (request != 0) {
			g_steam.ReleaseHTTPRequest(g_steam.http, request);
			request = 0;
		}
		RetryOrDrop(0);
		return;
	}

	uint64_t call = 0;
	if (!g_steam.SendHTTPRequest(g_steam.http, request, &call)) {
		g_steam.ReleaseHTTPRequest(g_steam.http, request);
		request = 0;
		RetryOrDrop(0);
		return;
	}
	requestKind = RequestKind::Discord;
	requestStarted = std::chrono::steady_clock::now();
	responseReady = false;
	g_callback.impl = this;
	this->call = call;
	g_steam.RegisterCallResult(&g_callback, call);
}

bool WebhookService::IsValidUrl(const char* url)
{
	if (!url || !*url) {
		return true;
	}
	const std::string_view value(url);
	const std::string_view currentPrefix = "https://discord.com/api/webhooks/";
	const std::string_view legacyPrefix = "https://discordapp.com/api/webhooks/";
	return ((value.size() >= currentPrefix.size() && value.substr(0, currentPrefix.size()) == currentPrefix)
			|| (value.size() >= legacyPrefix.size() && value.substr(0, legacyPrefix.size()) == legacyPrefix))
		   && value.find_first_of(" \t\r\n") == std::string_view::npos;
}

bool WebhookService::IsValidRoleId(const char* roleId)
{
	if (!roleId || !*roleId) {
		return true;
	}
	const std::string_view value(roleId);
	return std::all_of(value.begin(), value.end(), [](unsigned char character) { return std::isdigit(character) != 0; });
}

bool WebhookService::IsValidLogoUrl(const char* url)
{
	if (!url || !*url) {
		return true;
	}
	const std::string_view value(url);
	constexpr std::string_view prefix = "https://";
	return value.size() > prefix.size() && value.substr(0, prefix.size()) == prefix && value.find_first_of(" \t\r\n") == std::string_view::npos;
}

WebhookService::WebhookService() : impl(std::make_unique<Impl>()) {}
WebhookService::~WebhookService() = default;

bool WebhookService::IsConfigured() const
{
	const char* url = settings::GetWebhookUrl();
	return url && *url;
}

bool WebhookService::IsDisabled() const
{
	return impl->disabled;
}

std::size_t WebhookService::QueueSize() const
{
	return impl->queue.size();
}

void WebhookService::Reload()
{
	impl->CancelRequest();
	impl->queue.clear();
	impl->avatarCache.clear();
	impl->disabled = false;
	impl->overflowWarned = false;
	impl->httpUnavailableWarned = false;
	impl->nextAttempt = {};
	if (!IsConfigured()) {
		return;
	}
	if (!IsValidUrl(settings::GetWebhookUrl())) {
		impl->Disable("cs2ac_webhook_url is not a Discord webhook URL.");
		return;
	}
	if (!IsValidRoleId(settings::GetWebhookRoleId())) {
		impl->Disable("cs2ac_webhook_role_id must contain only numbers.");
		return;
	}
	if (!IsValidLogoUrl(settings::GetWebhookLogoUrl())) {
		impl->Disable("cs2ac_webhook_logo_url must be an HTTPS URL.");
		return;
	}
}

void WebhookService::Unload()
{
	impl->CancelRequest();
	impl->queue.clear();
	impl->avatarCache.clear();
	impl->disabled = false;
	impl->overflowWarned = false;
	impl->httpUnavailableWarned = false;
	impl->nextAttempt = {};
}

void WebhookService::Report(const char* detection, int playerIndex, std::string_view evidence, announce::DetectionOutcome outcome)
{
	if (!IsConfigured() || impl->disabled || !detection || playerIndex < 1 || playerIndex > MAXPLAYERS) {
		return;
	}
	if (impl->queue.size() >= maximumQueueSize) {
		impl->queue.erase(impl->queue.begin() + (impl->request == 0 ? 0 : 1));
		if (!impl->overflowWarned) {
			impl->overflowWarned = true;
			ConsoleMessage("The Discord webhook queue filled up. The oldest report was dropped so gameplay stays unaffected.");
		}
	}
	Impl::ReportData report;
	report.detection = detection;
	const char* playerName = engine::PlayerName(playerIndex);
	report.playerName = playerName ? playerName : localization::Get("webhook.unknown_player", "Unknown player");
	report.evidence = evidence;
	report.steamId = engine::PlayerSteamId(playerIndex);
	report.outcome = outcome;
	const auto cachedAvatar = impl->avatarCache.find(report.steamId);
	if (!report.steamId || cachedAvatar != impl->avatarCache.end()) {
		report.avatarResolved = true;
		if (cachedAvatar != impl->avatarCache.end()) {
			report.avatarUrl = cachedAvatar->second;
		}
	}
	impl->queue.push_back(std::move(report));
}

void WebhookService::Test()
{
	if (!IsConfigured()) {
		ConsoleMessage("The Discord webhook test was not sent because cs2ac_webhook_url is empty.");
		return;
	}
	if (impl->disabled) {
		ConsoleMessage("The Discord webhook is disabled after an error. Fix it and run cs2ac_reload before testing again.");
		return;
	}
	if (impl->queue.size() >= maximumQueueSize) {
		impl->queue.erase(impl->queue.begin() + (impl->request == 0 ? 0 : 1));
	}
	Impl::ReportData report;
	report.detection = localization::Get("webhook.test_detection", "WEBHOOK TEST");
	report.playerName = localization::Get("webhook.test_player", "Webhook test");
	report.evidence = localization::Get("webhook.test_evidence", "This is a harmless test report. No detection or punishment was created.");
	report.outcome = announce::DetectionOutcome::PunishmentDisabled;
	report.avatarResolved = true;
	impl->queue.push_back(std::move(report));
	ConsoleMessage("The Discord webhook test was queued.");
}

void WebhookService::OnGameFrame()
{
	// Reap a request that never completed (no Steam call-result pump, offline server).
	if (impl->request != 0 && !impl->responseReady && std::chrono::steady_clock::now() - impl->requestStarted > kRequestTimeout) {
		impl->CancelRequest();
		impl->RetryOrDrop(0);
	}

	if (impl->responseReady) {
		const bool failed = impl->responseFailed;
		const int status = impl->responseStatus;
		std::string body = std::move(impl->responseBody);
		impl->responseReady = false;
		const Impl::RequestKind kind = impl->requestKind;
		impl->requestKind = Impl::RequestKind::None;
		if (impl->request != 0 && g_steam.http && g_steam.ReleaseHTTPRequest) {
			g_steam.ReleaseHTTPRequest(g_steam.http, impl->request);
			impl->request = 0;
		}
		// Steam unregisters a call result once it has fired; the id is only kept so an outstanding
		// request can be cancelled on unload.
		impl->call = 0;
		g_callback.impl = nullptr;

		if (kind == Impl::RequestKind::Avatar) {
			std::string avatarUrl;
			if (!failed && status >= 200 && status <= 299 && !body.empty() && body.size() <= maximumProfileSize) {
				avatarUrl = AvatarFromProfileXml(body);
			}
			impl->FinishAvatarLookup(std::move(avatarUrl));
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (status == 429) {
			double retrySeconds = 1.0;
			// The original read the Retry-After header; the body of a Discord 429 carries
			// retry_after in seconds, so the port uses that when the header is unavailable.
			const std::size_t retryAt = body.find("\"retry_after\"");
			if (retryAt != std::string::npos) {
				const std::size_t colon = body.find(':', retryAt);
				if (colon != std::string::npos) {
					char* end = nullptr;
					const double parsed = std::strtod(body.c_str() + colon + 1, &end);
					if (end && std::isfinite(parsed) && parsed > 0.0) {
						retrySeconds = std::clamp(parsed, 0.1, 3600.0);
					}
				}
			}
			impl->nextAttempt = now + std::chrono::milliseconds(static_cast<int>(retrySeconds * 1000.0));
		} else if (!failed && status >= 200 && status <= 299) {
			impl->queue.pop_front();
			impl->overflowWarned = false;
			impl->nextAttempt = {};
		} else if (status == 401 || status == 403 || status == 404) {
			impl->Disable("Discord rejected the webhook. It may be invalid or deleted.");
		} else if (!impl->queue.empty() && impl->queue.front().retries++ == 0) {
			impl->nextAttempt = now + std::chrono::seconds(2);
		} else {
			ConsoleMessage("A Discord webhook report failed after one retry and was dropped (HTTP %d).", status);
			if (!impl->queue.empty()) {
				impl->queue.pop_front();
			}
			impl->nextAttempt = {};
		}
		return;
	}

	if (impl->request == 0 && !impl->queue.empty() && std::chrono::steady_clock::now() >= impl->nextAttempt) {
		impl->SendNext();
	}
}

// webhook.cpp BuildPayload — field for field the original's Discord embed.
std::string WebhookService::Impl::BuildPayload(const ReportData& report)
{
	const std::string name = Limit(MarkdownEscape(report.playerName), 240);
	const std::string playerValue =
		report.steamId ? FormatString("[%s](https://steamcommunity.com/profiles/%llu)", name.c_str(),
									  static_cast<unsigned long long>(report.steamId))
					   : name;
	const std::string steamIdValue = report.steamId ? FormatString("||%llu||", static_cast<unsigned long long>(report.steamId))
													: localization::Get("webhook.steamid_unavailable", "SteamID64 is unavailable.");
	const std::string map = engine::GameReady() ? std::string(s2sdk::GetCurrentMap().c_str()) : localization::Get("webhook.unavailable", "Unavailable");
	const std::string role = settings::GetWebhookRoleId();
	const std::string content = role.empty() ? "" : "<@&" + role + ">";
	const std::string allowed = role.empty() ? "{\"parse\":[]}" : "{\"parse\":[],\"roles\":[\"" + role + "\"]}";
	const std::string logo = settings::GetWebhookLogoUrl();
	const std::string thumbnailUrl = report.avatarUrl.empty() ? logo : report.avatarUrl;
	const std::string thumbnail = thumbnailUrl.empty() ? "" : FormatString(",\"thumbnail\":{\"url\":\"%s\"}", JsonEscape(Limit(thumbnailUrl, 2048)).c_str());
	const std::string authorIcon = logo.empty() ? "" : FormatString(",\"icon_url\":\"%s\"", JsonEscape(Limit(logo, 2048)).c_str());
	const std::string footerIcon = authorIcon;
	const std::string address = ServerAddress();
	const std::string addressField =
		address.empty() ? ""
						: FormatString(",{\"name\":\"%s\",\"value\":\"`%s`\",\"inline\":true}",
									   JsonEscape(localization::Get("webhook.field.address", "Address")).c_str(), JsonEscape(Limit(address, 512)).c_str());
	const std::string evidenceText =
		Limit(CompleteSentence(report.evidence.empty() ? localization::Get("webhook.default_evidence", "The detector reached its configured threshold")
													   : report.evidence),
			  1000);
	// The hostname goes through the engine's validated cvar cache (a raw FindConVar/GetConVarString
	// pair here would pass s2sdk's invalid handle straight into GetConVarString when the cvar is
	// missing, which is what makes s2sdk print "Invalid ConVar handle").
	const std::string hostname = engine::ServerCvarString("hostname");
	const std::string serverName = Limit(hostname.empty() ? localization::Get("webhook.default_server_name", "CS2 Server") : hostname, 256);
	const std::string footer = FormatString("CS2AC %s - %s", CS2AC_VERSION, localization::Get("webhook.footer", "Detection report"));

	return FormatString(
		"{\"username\":\"CS2AC\",\"content\":\"%s\",\"allowed_mentions\":%s,"
		"\"embeds\":[{\"author\":{\"name\":\"%s\"%s},\"color\":15548997%s,\"fields\":["
		"{\"name\":\"%s\",\"value\":\"%s\",\"inline\":true},"
		"{\"name\":\"%s\",\"value\":\"%s\",\"inline\":true},"
		"{\"name\":\"%s\",\"value\":\"`%s`\"},"
		"{\"name\":\"%s\",\"value\":\"```text\\n%s\\n```\"},"
		"{\"name\":\"%s\",\"value\":\"%s\"},"
		"{\"name\":\"%s\",\"value\":\"`%s`\",\"inline\":true}%s],"
		"\"timestamp\":\"%s\",\"footer\":{\"text\":\"%s\"%s},"
		"\"image\":{\"url\":\"https://raw.githubusercontent.com/karola3vax/CS2AC/main/docs/cs2ac-logo.png\"}}]}",
		JsonEscape(content).c_str(), allowed.c_str(), JsonEscape(serverName).c_str(), authorIcon.c_str(), thumbnail.c_str(),
		JsonEscape(localization::Get("webhook.field.player", "Player")).c_str(), JsonEscape(playerValue).c_str(),
		JsonEscape(localization::Get("webhook.field.steamid64", "STEAMID64")).c_str(), JsonEscape(steamIdValue).c_str(),
		JsonEscape(localization::Get("webhook.field.detection", "Detection")).c_str(),
		JsonEscape(Limit(report.detection.empty() ? localization::Get("webhook.unknown_detection", "UNKNOWN") : report.detection, 256)).c_str(),
		JsonEscape(localization::Get("webhook.field.evidence", "Evidence")).c_str(), JsonEscape(evidenceText).c_str(),
		JsonEscape(localization::Get("webhook.field.punishment", "Punishment")).c_str(), JsonEscape(announce::OutcomeText(report.outcome)).c_str(),
		JsonEscape(localization::Get("webhook.field.map", "Map")).c_str(), JsonEscape(Limit(map, 256)).c_str(), addressField.c_str(),
		UtcTimestamp().c_str(), JsonEscape(footer).c_str(), footerIcon.c_str());
}

} // namespace cs2ac
