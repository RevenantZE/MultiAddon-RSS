// All IDs below are synthetic parser fixtures, not collected player records.
// R2 strict parser + state machine tests. STL only, C++17, -fno-exceptions.
// Product header under test is included directly; the product cpp must use the
// same header functions (verified by static gates separately).
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "rss_asset_preferences.h"

static int s_fail = 0;
static int s_pass = 0;

#define CHECK(cond) do { \
	if (cond) { ++s_pass; } \
	else { ++s_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

typedef std::set<rss_prefs::RssSteamId> IdSet;

static bool ParseFailsUnchanged(const std::string &doc)
{
	IdSet out;
	out.insert(12345ULL);
	IdSet snapshot = out;
	const bool ok = rss_prefs::ParseRssAssetPreferences(doc, out);
	return !ok && out == snapshot;
}

int main()
{
	// ---- Valid documents ----
	{
		IdSet out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\"version\": 1, \"opt_out_steam_ids\": {\"111\": true, \"222\": false}}", out));
		CHECK(out.size() == 1 && out.count(111ULL) == 1);
	}
	{
		// Order swapped, comments, trailing commas, empty set.
		IdSet out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\n"
			"  // leading comment\n"
			"  \"opt_out_steam_ids\": {\n"
			"    \"76561198000000001\": true,\n"
			"  }, /* block */\n"
			"  \"version\": 1,\n"
			"}", out));
		CHECK(out.size() == 1 && out.count(76561198000000001ULL) == 1);
	}
	{
		IdSet out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\"version\":1,\"opt_out_steam_ids\":{}}", out));
		CHECK(out.empty());
	}
	{
		// Writer roundtrip.
		IdSet want;
		want.insert(3782730321ULL);
		want.insert(3777226686ULL);
		want.insert(18446744073709551615ULL);
		const std::string built = rss_prefs::BuildRssAssetPreferencesJson(want);
		IdSet got;
		CHECK(rss_prefs::ParseRssAssetPreferences(built, got));
		CHECK(got == want);
	}

	// ---- Strict rejections (each must fail and preserve output set) ----
	CHECK(ParseFailsUnchanged("{\"version\": 01, \"opt_out_steam_ids\": {}}"));       // 01
	CHECK(ParseFailsUnchanged("{\"version\": 1.0, \"opt_out_steam_ids\": {}}"));      // 1.0
	CHECK(ParseFailsUnchanged("{\"version\": 1e0, \"opt_out_steam_ids\": {}}"));      // 1e0
	CHECK(ParseFailsUnchanged("{\"version\": \"1\", \"opt_out_steam_ids\": {}}"));    // "1"
	CHECK(ParseFailsUnchanged("{\"version\": true, \"opt_out_steam_ids\": {}}"));     // true
	CHECK(ParseFailsUnchanged("{\"version\": 2, \"opt_out_steam_ids\": {}}"));        // 2
	CHECK(ParseFailsUnchanged("{\"version\": 1}"));                                   // missing key
	CHECK(ParseFailsUnchanged("{\"opt_out_steam_ids\": {}}"));                        // missing version
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"version\": 1, \"opt_out_steam_ids\": {}}")); // dup key
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {}, \"opt_out_steam_ids\": {}}")); // dup key
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"bogus\": 1, \"opt_out_steam_ids\": {}}"));  // unknown key
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": True}}"));     // True
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": 1}}"));        // 1
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": \"true\"}}")); // "true"
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": null}}"));     // null
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"12\\\\34\": true}}")); // escaped key
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": tr/**/ue}}"));    // token join
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": true}} garbage")); // trailing garbage
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": true}} /* oops")); // unterminated comment
	CHECK(ParseFailsUnchanged("{\"version\": 1, /* oops \"opt_out_steam_ids\": {}}"));            // unterminated mid-doc
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": true, \"01\": true}}")); // numeric dup
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"7\": true, \"7\": false}}")); // text dup
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"0\": true}}"));        // zero id
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"\": true}}"));         // empty key
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"18446744073709551616\": true}}")); // overflow
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"18446744073709551616\": false}}")); // max+1 false still fails
	{
		// uint64 max true/false each; max+1 already rejected above.
		IdSet out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\"version\": 1, \"opt_out_steam_ids\": {\"18446744073709551615\": true}}", out));
		CHECK(out.size() == 1 && out.count(18446744073709551615ULL) == 1);
		IdSet out2;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\"version\": 1, \"opt_out_steam_ids\": {\"18446744073709551615\": false}}", out2));
		CHECK(out2.empty());
	}
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"12a\": true}}"));      // non-digit
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\" 12\": true}}"));      // space padded
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": true}"));         // unclosed top
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1\": true}"));         // unclosed inner
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": {\"1: true}}"));          // unclosed string
	CHECK(ParseFailsUnchanged("junk {\"version\": 1, \"opt_out_steam_ids\": {}}"));               // leading garbage
	CHECK(ParseFailsUnchanged("{\"version\": 1, \"opt_out_steam_ids\": [], \"x\": 1}"));         // array + unknown
	{
		std::string nulDoc("{\"version\": 1, \"opt_out_steam_ids\": {}}");
		nulDoc.insert(5, 1, '\0');
		CHECK(ParseFailsUnchanged(nulDoc)); // NUL inside
	}
	{
		std::string nulEnd("{\"version\": 1, \"opt_out_steam_ids\": {}}");
		nulEnd.push_back('\0');
		CHECK(ParseFailsUnchanged(nulEnd)); // NUL at end
	}

	// False-valued entries still occupy a numeric key. Failure preserves output.
	CHECK(ParseFailsUnchanged("{\"version\":1,\"opt_out_steam_ids\":{\"1\":false,\"1\":true}}"));
	CHECK(ParseFailsUnchanged("{\"version\":1,\"opt_out_steam_ids\":{\"1\":false,\"01\":true}}"));

	// ---- State machine ----
	{
		// Latch false blocks, zero callbacks.
		bool latch = false;
		IdSet members;
		int persistCalls = 0, cacheCalls = 0;
		const bool ok = rss_prefs::ApplyRssPreferenceChange(latch, members, 9ULL, false,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; });
		CHECK(!ok && persistCalls == 0 && cacheCalls == 0 && members.empty() && !latch);
	}
	{
		// Latch false + same value still false.
		bool latch = false;
		IdSet members;
		int persistCalls = 0, cacheCalls = 0;
		const bool ok = rss_prefs::ApplyRssPreferenceChange(latch, members, 9ULL, true,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; });
		CHECK(!ok && persistCalls == 0 && cacheCalls == 0);
	}
	{
		// Same value with latch: true, no callbacks.
		bool latch = true;
		IdSet members;
		int persistCalls = 0, cacheCalls = 0;
		CHECK(rss_prefs::ApplyRssPreferenceChange(latch, members, 9ULL, true,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; }));
		CHECK(persistCalls == 0 && cacheCalls == 0 && latch);
	}
	{
		// Disable with persist ok: membership + exactly one cache.
		bool latch = true;
		IdSet members;
		int persistCalls = 0, cacheCalls = 0;
		CHECK(rss_prefs::ApplyRssPreferenceChange(latch, members, 42ULL, false,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; }));
		CHECK(members.count(42ULL) == 1 && persistCalls == 1 && cacheCalls == 1 && latch);
		// Enable back: removal + one cache.
		CHECK(rss_prefs::ApplyRssPreferenceChange(latch, members, 42ULL, true,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; }));
		CHECK(members.empty() && persistCalls == 2 && cacheCalls == 2 && latch);
	}
	{
		// Persist failure: rollback + latch cleared + no cache.
		bool latch = true;
		IdSet members;
		members.insert(7ULL);
		int persistCalls = 0, cacheCalls = 0;
		const bool ok = rss_prefs::ApplyRssPreferenceChange(latch, members, 7ULL, true,
			[&]() { ++persistCalls; return false; }, [&]() { ++cacheCalls; });
		CHECK(!ok && members.count(7ULL) == 1 && persistCalls == 1 && cacheCalls == 0 && !latch);
		// Latch stays cleared for the next attempt.
		const bool ok2 = rss_prefs::ApplyRssPreferenceChange(latch, members, 8ULL, false,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; });
		CHECK(!ok2 && persistCalls == 1 && cacheCalls == 0 && members.count(8ULL) == 0);
	}
	{
		// Zero id rejected.
		bool latch = true;
		IdSet members;
		int persistCalls = 0, cacheCalls = 0;
		CHECK(!rss_prefs::ApplyRssPreferenceChange(latch, members, 0ULL, false,
			[&]() { ++persistCalls; return true; }, [&]() { ++cacheCalls; }));
		CHECK(persistCalls == 0 && cacheCalls == 0 && latch);
	}
	{
		// Legacy partial read: no publish.
		IdSet out;
		out.insert(5ULL);
		const IdSet snapshot = out;
		std::vector<std::string> lines;
		lines.push_back("111\n");
		CHECK(!rss_prefs::CollectLegacyOptOutIds(lines, false, out));
		CHECK(out == snapshot);
	}
	{
		// Legacy ok: publishes clean ids; rejects blanks/padding.
		IdSet out;
		std::vector<std::string> lines;
		lines.push_back("111\n");
		lines.push_back("222\r\n");
		CHECK(rss_prefs::CollectLegacyOptOutIds(lines, true, out));
		CHECK(out.size() == 2 && out.count(111ULL) == 1 && out.count(222ULL) == 1);
		std::vector<std::string> bad;
		bad.push_back("\n");
		CHECK(!rss_prefs::CollectLegacyOptOutIds(bad, true, out));
		CHECK(out.size() == 2);
		std::vector<std::string> bad2;
		bad2.push_back(" 333\n");
		CHECK(!rss_prefs::CollectLegacyOptOutIds(bad2, true, out));
		CHECK(out.size() == 2);
	}

	std::printf("pass=%d fail=%d\n", s_pass, s_fail);
	return s_fail ? 1 : 0;
}
