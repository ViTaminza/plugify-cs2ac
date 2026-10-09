// cs2ac — localization (localization/localization.cpp).
//
// Same file names, same KeyValues "Phrases" content and the same fallback rules as the original:
// a phrase is only used when it exists *and* has the same placeholder set as the English text;
// otherwise the English text is used and the one-time fallback warning is printed.
//
// The Metamod original read `addons/cs2ac/translations/<lang>.txt` through the engine filesystem.
// Here the caller passes the translations directory (the plugin's <configs>/cs2ac/translations),
// which keeps this file free of engine/plugify symbols so the unit tests can exercise it.

#include "core.hpp"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace cs2ac {
namespace localization {

namespace {

std::unordered_map<std::string, std::string> phrases;
std::string currentLanguage {"en"};
bool fallbackWarningPrinted;
std::function<void(const std::string&)> warningSink;

// localization.cpp watermarkPhrases — the English watermark and its 37 translations.
constexpr std::pair<std::string_view, std::string_view> watermarkPhrases[] = {
	{"ar", "هذا الخادم محمي بنظام مكافحة الغش الخاص بـ {author}."},
	{"bg", "Този сървър е защитен от анти-чита на {author}."},
	{"cs", "Tento server chrání anti-cheat od {author}."},
	{"da", "Denne server beskyttes af {author}s anti-cheat."},
	{"de", "Dieser Server wird durch den Anti-Cheat von {author} geschützt."},
	{"el", "Αυτός ο διακομιστής προστατεύεται από το anti-cheat του {author}."},
	{"en", "This server is protected by {author}'s anti-cheat."},
	{"es-419", "Este servidor está protegido por el anticheat de {author}."},
	{"es-es", "Este servidor está protegido por el antitrampas de {author}."},
	{"et", "Seda serverit kaitseb {author} anti-cheat."},
	{"fi", "Tätä palvelinta suojaa käyttäjän {author} anti-cheat."},
	{"fr", "Ce serveur est protégé par l'anti-triche de {author}."},
	{"he", "שרת זה מוגן על ידי האנטי-צ'יט של {author}."},
	{"hr", "Ovaj poslužitelj štiti anti-cheat autora {author}."},
	{"hu", "Ezt a szervert {author} anti-cheat rendszere védi."},
	{"id", "Server ini dilindungi oleh anti-cheat milik {author}."},
	{"it", "Questo server è protetto dall'anti-cheat di {author}."},
	{"ja", "このサーバーは {author} のアンチチートで保護されています。"},
	{"ko", "이 서버는 {author}의 안티치트로 보호됩니다."},
	{"lt", "Šį serverį saugo {author} anti-cheat."},
	{"lv", "Šo serveri aizsargā {author} anti-cheat."},
	{"nl", "Deze server wordt beschermd door de anti-cheat van {author}."},
	{"no", "Denne serveren beskyttes av anti-cheaten til {author}."},
	{"pl", "Ten serwer jest chroniony przez antycheat {author}."},
	{"pt-br", "Este servidor é protegido pelo anticheat de {author}."},
	{"pt-pt", "Este servidor está protegido pelo anticheat de {author}."},
	{"ro", "Acest server este protejat de sistemul anti-cheat al lui {author}."},
	{"ru", "Этот сервер защищён античитом {author}."},
	{"sk", "Tento server chráni anti-cheat od {author}."},
	{"sr", "Ovaj server štiti anti-cheat autora {author}."},
	{"sv", "Den här servern skyddas av {author}s anti-cheat."},
	{"th", "เซิร์ฟเวอร์นี้ได้รับการปกป้องโดยระบบป้องกันโกงของ {author}"},
	{"tr", "Bu sunucu {author} anti-hilesi tarafından korunuyor."},
	{"uk", "Цей сервер захищено античитом {author}."},
	{"vi", "Máy chủ này được bảo vệ bởi hệ thống chống gian lận của {author}."},
	{"zh-cn", "本服务器受 {author} 的反作弊系统保护。"},
	{"zh-tw", "本伺服器受 {author} 的反作弊系統保護。"},
};

void WarnFallback()
{
	if (!fallbackWarningPrinted) {
		fallbackWarningPrinted = true;
		if (warningSink) {
			warningSink(FormatString("Some phrases for language '%s' are missing or invalid. English will be used for them.", currentLanguage.c_str()));
		}
	}
}

// localization.cpp NormalizeLanguage
std::string NormalizeLanguage(const char* language)
{
	std::string result = language ? language : "";
	if (result.empty() || result.size() > 16) {
		return {};
	}
	for (char& character : result) {
		if (character == '_') {
			character = '-';
		} else if (std::isalnum(static_cast<unsigned char>(character))) {
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		} else if (character != '-') {
			return {};
		}
	}
	return result;
}

std::set<std::string> Placeholders(std::string_view text)
{
	std::set<std::string> result;
	for (std::size_t start = text.find('{'); start != std::string_view::npos; start = text.find('{', start + 1)) {
		const std::size_t end = text.find('}', start + 1);
		if (end == std::string_view::npos) {
			break;
		}
		const std::string_view name = text.substr(start + 1, end - start - 1);
		if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char character) {
				return std::isalnum(character) || character == '_' || character == '.';
			})) {
			result.emplace(name);
		}
	}
	return result;
}

std::string Apply(std::string_view text, Arguments arguments)
{
	std::string result;
	result.reserve(text.size() + 32);
	for (std::size_t index = 0; index < text.size();) {
		if (text[index] != '{') {
			result += text[index++];
			continue;
		}
		const std::size_t end = text.find('}', index + 1);
		if (end == std::string_view::npos) {
			result.append(text.substr(index));
			break;
		}
		const std::string_view name = text.substr(index + 1, end - index - 1);
		const auto found = std::find_if(arguments.begin(), arguments.end(), [name](const Argument& argument) { return argument.name == name; });
		if (found == arguments.end()) {
			result.append(text.substr(index, end - index + 1));
		} else {
			result += found->value;
		}
		index = end + 1;
	}
	return result;
}

// Minimal reader for the original phrase files: `"Phrases" { "key" "value" ... }` with optional
// // comments and the usual KV escapes (\n, \t, \r, \", \\). Only the first (Phrases) block is
// read; keys and values are taken verbatim otherwise.
bool LoadPhraseFile(const std::string& path, std::unordered_map<std::string, std::string>& out)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	std::stringstream buffer;
	buffer << file.rdbuf();
	const std::string contents = buffer.str();

	std::size_t cursor = 0;
	const auto skipSpace = [&]() {
		while (cursor < contents.size()) {
			const char character = contents[cursor];
			if (std::isspace(static_cast<unsigned char>(character))) {
				++cursor;
			} else if (character == '/' && cursor + 1 < contents.size() && contents[cursor + 1] == '/') {
				while (cursor < contents.size() && contents[cursor] != '\n') {
					++cursor;
				}
			} else {
				break;
			}
		}
	};
	const auto readString = [&](std::string& out) {
		skipSpace();
		if (cursor >= contents.size() || contents[cursor] != '"') {
			return false;
		}
		++cursor;
		out.clear();
		while (cursor < contents.size() && contents[cursor] != '"') {
			char character = contents[cursor++];
			if (character == '\\' && cursor < contents.size()) {
				const char escaped = contents[cursor++];
				switch (escaped) {
				case 'n': character = '\n'; break;
				case 't': character = '\t'; break;
				case 'r': character = '\r'; break;
				default: character = escaped; break;
				}
			}
			out += character;
		}
		if (cursor >= contents.size()) {
			return false;
		}
		++cursor; // closing quote
		return true;
	};

	std::string blockName;
	if (!readString(blockName)) {
		return false;
	}
	skipSpace();
	if (cursor >= contents.size() || contents[cursor] != '{') {
		return false;
	}
	++cursor;
	while (true) {
		skipSpace();
		if (cursor >= contents.size() || contents[cursor] == '}') {
			break;
		}
		std::string key;
		if (!readString(key)) {
			// A stray token (an unquoted key, a brace inside a value) must not discard the whole
			// file: skip to the next quote or the closing brace and continue.
			const std::size_t next = contents.find_first_of("\"}", cursor);
			if (next == std::string::npos) {
				break;
			}
			cursor = next;
			continue;
		}
		std::string value;
		skipSpace();
		if (cursor < contents.size() && contents[cursor] == '{') {
			// Nested block in a phrase file: skip it (the original flattening would too).
			int depth = 1;
			++cursor;
			while (cursor < contents.size() && depth > 0) {
				if (contents[cursor] == '{') {
					++depth;
				} else if (contents[cursor] == '}') {
					--depth;
				}
				++cursor;
			}
			continue;
		}
		if (!readString(value)) {
			return false;
		}
		if (!value.empty()) {
			out[key] = value;
		}
	}
	return true;
}

} // namespace

void Reload(const char* language, const std::string& translationsDir)
{
	phrases.clear();
	fallbackWarningPrinted = false;
	currentLanguage = NormalizeLanguage(language);
	if (currentLanguage.empty()) {
		currentLanguage = "en";
		WarnFallback();
		return;
	}
	const std::string path = translationsDir + "/" + currentLanguage + ".txt";
	if (!LoadPhraseFile(path, phrases)) {
		WarnFallback();
	}
}

void Shutdown()
{
	phrases.clear();
	currentLanguage = "en";
	fallbackWarningPrinted = false;
}

const char* CurrentLanguage() { return currentLanguage.c_str(); }

std::string Get(const char* key, const char* english)
{
	const std::string fallback = english ? english : "";
	const auto found = key ? phrases.find(key) : phrases.end();
	if (found == phrases.end() || Placeholders(found->second) != Placeholders(fallback)) {
		if (found == phrases.end() || found->second != fallback) {
			WarnFallback();
		}
		return fallback;
	}
	return found->second;
}

Text Format(const char* key, const char* english, Arguments arguments)
{
	return {Apply(english ? english : "", arguments), Apply(Get(key, english), arguments)};
}

Text Watermark(Arguments arguments)
{
	constexpr std::string_view english = "This server is protected by {author}'s anti-cheat.";
	const auto found =
		std::find_if(std::begin(watermarkPhrases), std::end(watermarkPhrases), [](const auto& phrase) { return phrase.first == currentLanguage; });
	const std::string_view localized = found == std::end(watermarkPhrases) ? english : found->second;
	return {Apply(english, arguments), Apply(localized, arguments)};
}

std::size_t LoadedPhraseCount() { return phrases.size(); }

bool HasPhrase(const char* key) { return key && phrases.find(key) != phrases.end(); }

// Test/diagnostic seam: where the one-time fallback warning goes (the plugin points it at the log).
void SetWarningSink(std::function<void(const std::string&)> sink)
{
	warningSink = std::move(sink);
}

} // namespace localization
} // namespace cs2ac
