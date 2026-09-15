#include <cstdio>
#include <string>
#include <vector>

#include "rss_asset_preferences.h"

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(expr) do { if (expr) { ++s_pass; } else { ++s_fail; \
	std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

static bool ParseFailsUnchanged(const std::string &text)
{
	rss_prefs::ParsedPreferences out;
	out.sourceVersion = 77;
	out.clients[123ULL] = rss_prefs::Mode::MountOnly;
	const rss_prefs::ParsedPreferences before = out;
	return !rss_prefs::ParseRssAssetPreferences(text, out) &&
		out.sourceVersion == before.sourceVersion && out.clients == before.clients;
}

int main()
{
	{
		rss_prefs::ParsedPreferences out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{/*ok*/\"version\":2,\"clients\":{"
			"\"76561198000000003\":\"download_and_mount\","
			"\"76561198000000001\":\"disabled\","
			"\"76561198000000002\":\"mount_only\",},}", out));
		CHECK(out.sourceVersion == 2 && out.clients.size() == 3);
		CHECK(out.clients[76561198000000001ULL] == rss_prefs::Mode::Disabled);
		CHECK(out.clients[76561198000000002ULL] == rss_prefs::Mode::MountOnly);
		CHECK(out.clients[76561198000000003ULL] == rss_prefs::Mode::DownloadAndMount);
		const std::string canonical = rss_prefs::BuildRssAssetPreferencesJson(out.clients);
		CHECK(canonical.find("76561198000000001") < canonical.find("76561198000000002"));
		rss_prefs::ParsedPreferences roundTrip;
		CHECK(rss_prefs::ParseRssAssetPreferences(canonical, roundTrip));
		CHECK(roundTrip.sourceVersion == 2 && roundTrip.clients == out.clients);
	}
	{
		rss_prefs::ParsedPreferences out;
		CHECK(rss_prefs::ParseRssAssetPreferences(
			"{\"version\":1,\"opt_out_steam_ids\":{\"11\":true,\"22\":false}}", out));
		CHECK(out.sourceVersion == 1 && out.clients.size() == 2);
		CHECK(out.clients[11ULL] == rss_prefs::Mode::MountOnly);
		CHECK(out.clients[22ULL] == rss_prefs::Mode::DownloadAndMount);
	}

	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{\"0\":\"disabled\"}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{\"01\":\"disabled\",\"1\":\"mount_only\"}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{\"18446744073709551616\":\"disabled\"}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{\"1\":\"bad\"}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{},\"unknown\":1}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{},\"clients\":{}}"));
	CHECK(ParseFailsUnchanged("{\"version\":1,\"clients\":{}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"opt_out_steam_ids\":{}}"));
	CHECK(ParseFailsUnchanged("{\"version\":02,\"clients\":{}}"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{}} trailing"));
	CHECK(ParseFailsUnchanged("{\"version\":2,\"clients\":{\"1\\\\2\":\"disabled\"}}"));
	CHECK(ParseFailsUnchanged("/* unterminated"));
	{
		std::string withNul("{\"version\":2,\"clients\":{}}");
		withNul.push_back('\0');
		CHECK(ParseFailsUnchanged(withNul));
	}

	{
		rss_prefs::ModeMap migrated;
		std::vector<std::string> lines{"11\n", "22\r\n"};
		CHECK(rss_prefs::CollectLegacyOptOutIds(lines, true, migrated));
		CHECK(migrated.size() == 2 && migrated[11ULL] == rss_prefs::Mode::MountOnly &&
			migrated[22ULL] == rss_prefs::Mode::MountOnly);
		const rss_prefs::ModeMap before = migrated;
		CHECK(!rss_prefs::CollectLegacyOptOutIds(std::vector<std::string>{" 33\n"}, true, migrated));
		CHECK(migrated == before);
		CHECK(!rss_prefs::CollectLegacyOptOutIds(lines, false, migrated));
		CHECK(migrated == before);
	}

	{
		bool writable = true;
		rss_prefs::ModeMap modes;
		modes[7ULL] = rss_prefs::Mode::Disabled;
		int persistCalls = 0;
		int commitCalls = 0;
		CHECK(rss_prefs::ApplyRssPreferenceChange(writable, modes, 7ULL,
			rss_prefs::Mode::Disabled,
			[&](const rss_prefs::ModeMap &) { ++persistCalls; return true; },
			[&]() { ++commitCalls; }));
		CHECK(persistCalls == 0 && commitCalls == 0);
		CHECK(rss_prefs::ApplyRssPreferenceChange(writable, modes, 7ULL,
			rss_prefs::Mode::MountOnly,
			[&](const rss_prefs::ModeMap &candidate) {
				++persistCalls;
				return candidate.at(7ULL) == rss_prefs::Mode::MountOnly;
			}, [&]() { ++commitCalls; }));
		CHECK(modes[7ULL] == rss_prefs::Mode::MountOnly && persistCalls == 1 && commitCalls == 1);
		const rss_prefs::ModeMap before = modes;
		CHECK(!rss_prefs::ApplyRssPreferenceChange(writable, modes, 7ULL,
			rss_prefs::Mode::DownloadAndMount,
			[&](const rss_prefs::ModeMap &) { ++persistCalls; return false; },
			[&]() { ++commitCalls; }));
		CHECK(!writable && modes == before && persistCalls == 2 && commitCalls == 1);
		CHECK(!rss_prefs::ApplyRssPreferenceChange(writable, modes, 9ULL,
			rss_prefs::Mode::Disabled,
			[&](const rss_prefs::ModeMap &) { ++persistCalls; return true; },
			[&]() { ++commitCalls; }));
		CHECK(persistCalls == 2 && commitCalls == 1 && modes.find(9ULL) == modes.end());
	}
	{
		for (int failStep = 0; failStep < 4; ++failStep)
		{
			int calls[4] = {};
			const bool ok = rss_prefs::RunPreferenceWriteTransaction(
				[&]() { ++calls[0]; return failStep != 0; },
				[&]() { ++calls[1]; return failStep != 1; },
				[&]() { ++calls[2]; return failStep != 2; },
				[&]() { ++calls[3]; return failStep != 3; });
			CHECK(!ok);
			CHECK(calls[0] == 1 && calls[1] == 1 && calls[2] == 1);
			CHECK(calls[3] == (failStep == 3 ? 1 : 0));
		}
		int order = 0;
		CHECK(rss_prefs::RunPreferenceWriteTransaction(
			[&]() { CHECK(order++ == 0); return true; },
			[&]() { CHECK(order++ == 1); return true; },
			[&]() { CHECK(order++ == 2); return true; },
			[&]() { CHECK(order++ == 3); return true; }));
		CHECK(order == 4);
	}

	std::printf("pass=%d fail=%d\n", s_pass, s_fail);
	return s_fail ? 1 : 0;
}
