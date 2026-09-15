#pragma once

// STL-only RSS preference v1/v2 parser and transactional state helpers.
// C++17, -fno-exceptions and -fno-rtti compatible.

#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rss_prefs {

typedef unsigned long long RssSteamId;

enum class Mode : unsigned int
{
	Disabled = 0,
	MountOnly = 1,
	DownloadAndMount = 2
};

typedef std::map<RssSteamId, Mode> ModeMap;

struct ParsedPreferences
{
	unsigned int sourceVersion = 0;
	ModeMap clients;
};

inline bool IsValidMode(Mode mode)
{
	return mode == Mode::Disabled || mode == Mode::MountOnly ||
		mode == Mode::DownloadAndMount;
}

namespace detail {

inline bool IsDigitChar(char c) { return c >= '0' && c <= '9'; }
inline bool IsWsChar(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

inline bool SkipWsAndComments(const std::string &doc, std::size_t &pos)
{
	while (pos < doc.size())
	{
		const char c = doc[pos];
		if (c == 0)
			return false;
		if (IsWsChar(c))
		{
			++pos;
			continue;
		}
		if (c == '/' && pos + 1 < doc.size() && doc[pos + 1] == '/')
		{
			pos += 2;
			while (pos < doc.size() && doc[pos] != '\n' && doc[pos] != '\r')
			{
				if (doc[pos] == 0)
					return false;
				++pos;
			}
			continue;
		}
		if (c == '/' && pos + 1 < doc.size() && doc[pos + 1] == '*')
		{
			pos += 2;
			bool closed = false;
			while (pos < doc.size())
			{
				if (doc[pos] == 0)
					return false;
				if (doc[pos] == '*' && pos + 1 < doc.size() && doc[pos + 1] == '/')
				{
					pos += 2;
					closed = true;
					break;
				}
				++pos;
			}
			if (!closed)
				return false;
			continue;
		}
		break;
	}
	return true;
}

inline bool ConsumeChar(const std::string &doc, std::size_t &pos, char want)
{
	if (pos >= doc.size() || doc[pos] != want)
		return false;
	++pos;
	return true;
}

inline bool ParseQuotedString(const std::string &doc, std::size_t &pos, std::string &out)
{
	out.clear();
	if (!ConsumeChar(doc, pos, '"'))
		return false;
	while (pos < doc.size())
	{
		const char c = doc[pos++];
		if (c == 0 || c == '\\')
			return false;
		if (c == '"')
			return true;
		out.push_back(c);
	}
	return false;
}

inline bool IsTokenDelimiter(const std::string &doc, std::size_t pos)
{
	if (pos >= doc.size())
		return true;
	const char c = doc[pos];
	return IsWsChar(c) || c == ',' || c == '}' || c == ':' || c == '/' || c == '"';
}

inline bool ParseDecimalUll(const std::string &text, RssSteamId &out)
{
	if (text.empty() || text.size() > 20)
		return false;
	RssSteamId value = 0;
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (!IsDigitChar(text[i]))
			return false;
		const unsigned int digit = static_cast<unsigned int>(text[i] - '0');
		if (value > (18446744073709551615ULL - digit) / 10ULL)
			return false;
		value = value * 10ULL + digit;
	}
	if (value == 0ULL)
		return false;
	out = value;
	return true;
}

inline bool ParseBool(const std::string &doc, std::size_t &pos, bool &out)
{
	if (pos + 4 <= doc.size() && doc.compare(pos, 4, "true") == 0 &&
		IsTokenDelimiter(doc, pos + 4))
	{
		out = true;
		pos += 4;
		return true;
	}
	if (pos + 5 <= doc.size() && doc.compare(pos, 5, "false") == 0 &&
		IsTokenDelimiter(doc, pos + 5))
	{
		out = false;
		pos += 5;
		return true;
	}
	return false;
}

inline bool ParseModeText(const std::string &text, Mode &mode)
{
	if (text == "disabled")
		mode = Mode::Disabled;
	else if (text == "mount_only")
		mode = Mode::MountOnly;
	else if (text == "download_and_mount")
		mode = Mode::DownloadAndMount;
	else
		return false;
	return true;
}

template <typename ValueParser>
inline bool ParseClientObject(const std::string &doc, std::size_t &pos,
	std::set<RssSteamId> &seenIds, ModeMap &out, ValueParser parseValue)
{
	if (!ConsumeChar(doc, pos, '{'))
		return false;
	while (true)
	{
		if (!SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == '}')
		{
			++pos;
			return true;
		}
		std::string idText;
		if (!ParseQuotedString(doc, pos, idText))
			return false;
		RssSteamId id = 0;
		if (!ParseDecimalUll(idText, id) || !seenIds.insert(id).second)
			return false;
		if (!SkipWsAndComments(doc, pos) || !ConsumeChar(doc, pos, ':') ||
			!SkipWsAndComments(doc, pos))
			return false;
		Mode mode = Mode::Disabled;
		if (!parseValue(doc, pos, mode))
			return false;
		out[id] = mode;
		if (!SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == ',')
		{
			++pos;
			continue;
		}
		if (pos < doc.size() && doc[pos] == '}')
		{
			++pos;
			return true;
		}
		return false;
	}
}

} // namespace detail

inline const char *ModeText(Mode mode)
{
	switch (mode)
	{
		case Mode::Disabled: return "disabled";
		case Mode::MountOnly: return "mount_only";
		case Mode::DownloadAndMount: return "download_and_mount";
	}
	return "disabled";
}

inline std::string BuildRssAssetPreferencesJson(const ModeMap &clients)
{
	std::string json;
	json += "{\n";
	json += "  // Missing Steam IDs default to disabled.\n";
	json += "  \"version\": 2,\n";
	json += "  \"clients\": {\n";
	std::size_t index = 0;
	for (ModeMap::const_iterator it = clients.begin(); it != clients.end(); ++it)
	{
		char line[112];
		const int n = std::snprintf(line, sizeof(line), "    \"%llu\": \"%s\"%s\n",
			it->first, ModeText(it->second), (++index < clients.size()) ? "," : "");
		if (n > 0)
			json.append(line, static_cast<std::size_t>(n));
	}
	json += "  }\n}\n";
	return json;
}

inline bool ParseRssAssetPreferences(const std::string &doc, ParsedPreferences &out)
{
	for (std::size_t i = 0; i < doc.size(); ++i)
		if (doc[i] == 0)
			return false;

	unsigned int version = 0;
	bool seenVersion = false;
	bool seenClients = false;
	bool seenOptOut = false;
	ModeMap clients;
	ModeMap optOut;
	std::set<RssSteamId> seenClientIds;
	std::set<RssSteamId> seenOptOutIds;
	std::size_t pos = 0;
	if (!detail::SkipWsAndComments(doc, pos) || !detail::ConsumeChar(doc, pos, '{'))
		return false;

	while (true)
	{
		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == '}')
		{
			++pos;
			break;
		}
		std::string key;
		if (!detail::ParseQuotedString(doc, pos, key) ||
			!detail::SkipWsAndComments(doc, pos) || !detail::ConsumeChar(doc, pos, ':') ||
			!detail::SkipWsAndComments(doc, pos))
			return false;

		if (key == "version")
		{
			if (seenVersion || pos >= doc.size() || (doc[pos] != '1' && doc[pos] != '2'))
				return false;
			version = static_cast<unsigned int>(doc[pos++] - '0');
			if (!detail::IsTokenDelimiter(doc, pos))
				return false;
			seenVersion = true;
		}
		else if (key == "clients")
		{
			if (seenClients)
				return false;
			auto parser = [](const std::string &text, std::size_t &at, Mode &mode)
			{
				std::string value;
				return detail::ParseQuotedString(text, at, value) && detail::ParseModeText(value, mode);
			};
			if (!detail::ParseClientObject(doc, pos, seenClientIds, clients, parser))
				return false;
			seenClients = true;
		}
		else if (key == "opt_out_steam_ids")
		{
			if (seenOptOut)
				return false;
			auto parser = [](const std::string &text, std::size_t &at, Mode &mode)
			{
				bool value = false;
				if (!detail::ParseBool(text, at, value))
					return false;
				mode = value ? Mode::MountOnly : Mode::DownloadAndMount;
				return true;
			};
			if (!detail::ParseClientObject(doc, pos, seenOptOutIds, optOut, parser))
				return false;
			seenOptOut = true;
		}
		else
		{
			return false;
		}

		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == ',')
		{
			++pos;
			continue;
		}
		if (pos < doc.size() && doc[pos] == '}')
		{
			++pos;
			break;
		}
		return false;
	}

	if (!detail::SkipWsAndComments(doc, pos) || pos != doc.size() || !seenVersion)
		return false;
	ParsedPreferences parsed;
	parsed.sourceVersion = version;
	if (version == 1)
	{
		if (!seenOptOut || seenClients)
			return false;
		parsed.clients.swap(optOut);
	}
	else if (version == 2)
	{
		if (!seenClients || seenOptOut)
			return false;
		parsed.clients.swap(clients);
	}
	else
	{
		return false;
	}
	out = parsed;
	return true;
}

inline bool CollectLegacyOptOutIds(const std::vector<std::string> &lines, bool readOk,
	ModeMap &out)
{
	if (!readOk)
		return false;
	ModeMap parsed;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		std::string digits = lines[i];
		while (!digits.empty() && (digits.back() == '\n' || digits.back() == '\r'))
			digits.pop_back();
		RssSteamId id = 0;
		if (!detail::ParseDecimalUll(digits, id) || parsed.find(id) != parsed.end())
			return false;
		parsed[id] = Mode::MountOnly;
	}
	out.swap(parsed);
	return true;
}

template <typename WriteFn, typename FlushFn, typename CloseFn, typename RenameFn>
inline bool RunPreferenceWriteTransaction(WriteFn writeTemp, FlushFn flushTemp,
	CloseFn closeTemp, RenameFn renameTemp)
{
	// Flush and close are attempted after every opened temp write. Rename is
	// reachable only when all durability steps succeeded.
	const bool wrote = writeTemp();
	const bool flushed = flushTemp();
	const bool closed = closeTemp();
	return wrote && flushed && closed && renameTemp();
}

template <typename PersistFn, typename CommitFn>
inline bool ApplyRssPreferenceChange(bool &writable, ModeMap &modes, RssSteamId steamId,
	Mode mode, PersistFn persist, CommitFn committed)
{
	if (!writable || steamId == 0ULL || !IsValidMode(mode))
		return false;
	ModeMap::const_iterator current = modes.find(steamId);
	if (current != modes.end() && current->second == mode)
		return true;
	ModeMap candidate = modes;
	candidate[steamId] = mode;
	if (!persist(candidate))
	{
		writable = false;
		return false;
	}
	modes.swap(candidate);
	committed();
	return true;
}

} // namespace rss_prefs
