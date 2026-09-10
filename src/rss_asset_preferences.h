#pragma once
// RSS asset preference helpers (R2).
// STL-only, C++17, -fno-exceptions, -fno-rtti compatible.
// No Valve/Metamod headers. No C++20 API. No exception-based conversion. No RTTI.
// Internal Steam ID type is unsigned long long. Callers convert explicitly to
// Valve uint64 sets with element-wise static_cast (public ABI types unchanged).

#include <cstddef>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace rss_prefs {

typedef unsigned long long RssSteamId;

namespace detail {

inline bool IsDigitChar(char c) { return c >= 48 && c <= 57; }

inline bool IsWsChar(char c)
{
	return c == 32 || c == 9 || c == 10 || c == 13;
}

// Skip whitespace and //-style / /*-style comments. Comments act as
// whitespace: tokens must still match consecutively (tr/**/ue is rejected
// because true never appears consecutively). Returns false on NUL,
// unterminated block comment, or unterminated line handling fault.
inline bool SkipWsAndComments(const std::string &doc, std::size_t &pos)
{
	const std::size_t len = doc.size();
	while (pos < len)
	{
		const char c = doc[pos];
		if (c == 0)
			return false;
		if (IsWsChar(c))
		{
			++pos;
			continue;
		}
		if (c == 47 && pos + 1 < len && doc[pos + 1] == 47)
		{
			pos += 2;
			while (pos < len && doc[pos] != 10 && doc[pos] != 13)
			{
				if (doc[pos] == 0)
					return false;
				++pos;
			}
			continue;
		}
		if (c == 47 && pos + 1 < len && doc[pos + 1] == 42)
		{
			pos += 2;
			bool bClosed = false;
			while (pos < len)
			{
				if (doc[pos] == 0)
					return false;
				if (doc[pos] == 42 && pos + 1 < len && doc[pos + 1] == 47)
				{
					pos += 2;
					bClosed = true;
					break;
				}
				++pos;
			}
			if (!bClosed)
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

// Parse double-quoted string with no escapes. Content must not contain
// backslash or NUL and must terminate. Returns content in out.
inline bool ParseQuotedString(const std::string &doc, std::size_t &pos, std::string &out)
{
	out.clear();
	if (!ConsumeChar(doc, pos, 34))
		return false;
	while (pos < doc.size())
	{
		const char c = doc[pos];
		if (c == 0)
			return false;
		if (c == 92)
			return false;
		if (c == 34)
		{
			++pos;
			return true;
		}
		out.push_back(c);
		++pos;
	}
	return false;
}

inline bool IsTokenDelimiter(const std::string &doc, std::size_t pos)
{
	if (pos >= doc.size())
		return true;
	const char c = doc[pos];
	return c == 32 || c == 9 || c == 10 || c == 13 || c == 44 || c == 125 || c == 58 ||
		c == 47 || c == 34;
}

// Strict decimal uint64 parse: non-empty digits, no zero value, overflow-checked.
inline bool ParseDecimalUll(const std::string &text, RssSteamId &out)
{
	if (text.empty() || text.size() > 20)
		return false;
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		if (!IsDigitChar(text[i]))
			return false;
	}
	RssSteamId value = 0;
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		const unsigned digit = static_cast<unsigned>(text[i] - 48);
		if (value > (18446744073709551615ULL - digit) / 10ULL)
			return false;
		value = value * 10ULL + digit;
	}
	if (value == 0ULL)
		return false;
	out = value;
	return true;
}

} // namespace detail

// Build canonical JSONC text for a set of opt-out IDs.
inline std::string BuildRssAssetPreferencesJson(const std::set<RssSteamId> &optOutIds)
{
	std::string json;
	json += "{\n";
	json += "  // true skips the five staged RSS asset checks and fast-mounts cached assets.\n";
	json += "  // Missing IDs default to ON and use the normal staged download flow.\n";
	json += "  \"version\": 1,\n";
	json += "  \"opt_out_steam_ids\": {\n";
	std::size_t index = 0;
	for (std::set<RssSteamId>::const_iterator it = optOutIds.begin(); it != optOutIds.end(); ++it)
	{
		char numBuf[24];
		int numLen = 0;
		RssSteamId v = *it;
		if (v == 0ULL)
		{
			numBuf[0] = 48;
			numLen = 1;
		}
		else
		{
			char rev[24];
			int revLen = 0;
			while (v > 0ULL)
			{
				rev[revLen++] = static_cast<char>(48 + (v % 10ULL));
				v /= 10ULL;
			}
			for (int i = revLen - 1; i >= 0; --i)
				numBuf[numLen++] = rev[i];
		}
		numBuf[numLen] = 0;
		char szLine[64];
		int n = snprintf(szLine, sizeof(szLine), "    \"%s\": true%s\n", numBuf,
			(++index < optOutIds.size()) ? "," : "");
		if (n > 0)
			json.append(szLine, static_cast<std::size_t>(n));
	}
	json += "  }\n}\n";
	return json;
}

// Strict JSONC preference parse. On success publishes temp set into out and
// returns true. On any failure returns false and leaves out unchanged.
inline bool ParseRssAssetPreferences(const std::string &doc, std::set<RssSteamId> &out)
{
	for (std::size_t i = 0; i < doc.size(); ++i)
	{
		if (doc[i] == 0)
			return false;
	}
	std::set<RssSteamId> parsed;
	std::set<RssSteamId> seenIds;
	std::size_t pos = 0;
	if (!detail::SkipWsAndComments(doc, pos))
		return false;
	if (!detail::ConsumeChar(doc, pos, 123))
		return false;
	bool bSeenVersion = false;
	bool bSeenOptOut = false;
	bool bNeedPair = true;
	while (true)
	{
		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == 125)
		{
			++pos;
			break;
		}
		if (!bNeedPair)
			return false;
		std::string key;
		if (!detail::ParseQuotedString(doc, pos, key))
			return false;
		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (!detail::ConsumeChar(doc, pos, 58))
			return false;
		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (key == "version")
		{
			if (bSeenVersion)
				return false;
			if (pos >= doc.size() || doc[pos] != 49)
				return false;
			++pos;
			if (!detail::IsTokenDelimiter(doc, pos))
				return false;
			bSeenVersion = true;
		}
		else if (key == "opt_out_steam_ids")
		{
			if (bSeenOptOut)
				return false;
			if (!detail::ConsumeChar(doc, pos, 123))
				return false;
			bool bInnerNeedPair = true;
			while (true)
			{
				if (!detail::SkipWsAndComments(doc, pos))
					return false;
				if (pos < doc.size() && doc[pos] == 125)
				{
					++pos;
					break;
				}
				if (!bInnerNeedPair)
					return false;
				std::string idText;
				if (!detail::ParseQuotedString(doc, pos, idText))
					return false;
				RssSteamId id = 0ULL;
				if (!detail::ParseDecimalUll(idText, id))
					return false;
				if (!seenIds.insert(id).second)
					return false;
				if (!detail::SkipWsAndComments(doc, pos))
					return false;
				if (!detail::ConsumeChar(doc, pos, 58))
					return false;
				if (!detail::SkipWsAndComments(doc, pos))
					return false;
				bool bValue = false;
				if (pos + 4 <= doc.size() && doc.compare(pos, 4, "true") == 0 &&
					detail::IsTokenDelimiter(doc, pos + 4))
				{
					bValue = true;
					pos += 4;
				}
				else if (pos + 5 <= doc.size() && doc.compare(pos, 5, "false") == 0 &&
					detail::IsTokenDelimiter(doc, pos + 5))
				{
					bValue = false;
					pos += 5;
				}
				else
				{
					return false;
				}
				if (bValue)
					parsed.insert(id);
				if (!detail::SkipWsAndComments(doc, pos))
					return false;
				if (pos < doc.size() && doc[pos] == 44)
				{
					++pos;
					bInnerNeedPair = true;
					continue;
				}
				if (pos < doc.size() && doc[pos] == 125)
				{
					++pos;
					break;
				}
				return false;
			}
			bSeenOptOut = true;
		}
		else
		{
			return false;
		}
		if (!detail::SkipWsAndComments(doc, pos))
			return false;
		if (pos < doc.size() && doc[pos] == 44)
		{
			++pos;
			bNeedPair = true;
			continue;
		}
		if (pos < doc.size() && doc[pos] == 125)
		{
			++pos;
			break;
		}
		return false;
	}
	if (!bSeenVersion || !bSeenOptOut)
		return false;
	if (!detail::SkipWsAndComments(doc, pos))
		return false;
	if (pos != doc.size())
		return false;
	out = parsed;
	return true;
}

// Pure legacy TXT collector. lines holds ReadLine results in order, bReadOk is
// the post-loop IsOk state. Partial reads (bReadOk false) fail with out
// unchanged and no publish.
inline bool CollectLegacyOptOutIds(const std::vector<std::string> &lines, bool bReadOk,
	std::set<RssSteamId> &out)
{
	if (!bReadOk)
		return false;
	std::set<RssSteamId> parsed;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		const std::string &line = lines[i];
		if (line.empty())
			return false;
		std::string digits = line;
		while (!digits.empty() && (digits.back() == 10 || digits.back() == 13))
			digits.pop_back();
		if (digits.empty())
			return false;
		RssSteamId id = 0ULL;
		if (!detail::ParseDecimalUll(digits, id))
			return false;
		if (parsed.find(id) != parsed.end())
			return false;
		parsed.insert(id);
	}
	out = parsed;
	return true;
}

// Pure setter state machine used directly by the product setter and by tests.
// Order: latch check -> same-value check -> temp membership change ->
// persistence -> success cache. Persistence failure rolls membership back and
// clears the latch. Cache runs exactly once on persistence success; product
// cache callbacks clear RSS downloaded/pending caches only for ON transitions.
template <typename PersistFn, typename CacheFn>
inline bool ApplyRssPreferenceChange(bool &bWritableLatch, std::set<RssSteamId> &members,
	RssSteamId steamId, bool bEnabled, PersistFn persist, CacheFn onPersisted)
{
	if (steamId == 0ULL)
		return false;
	if (!bWritableLatch)
		return false;
	const bool bWasEnabled = (members.find(steamId) == members.end());
	if (bWasEnabled == bEnabled)
		return true;
	if (bEnabled)
		members.erase(steamId);
	else
		members.insert(steamId);
	if (!persist())
	{
		if (bWasEnabled)
			members.erase(steamId);
		else
			members.insert(steamId);
		bWritableLatch = false;
		return false;
	}
	onPersisted();
	return true;
}

} // namespace rss_prefs
