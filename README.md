# MultiAddon-RSS

RSS addon download preferences on Source2ZE/MultiAddonManager's KHook implementation.

The exact upstream base is **e85a483807b8ecac60f56e7b995c434162e989aa**.
The earlier RSS integration used 18e8b90a53e2e1440bf79ee83c76490d4fcbb3f6.
Those commits have divergent history but the same complete Git tree:
`5e4183887d1dfb74951be9429d04154bf2115029`. This repository reapplies the RSS
source changes on e85a483; no old RSS binary is relabeled as a new upstream build.

## Scope

- Preserve MultiAddonManager003 and its existing virtual interface.
- Offer MultiAddonManager004 with the two appended RSS preference methods.
- OFF excludes allowlisted RSS assets from required staged download checks while
  retaining their mounting path. It does not exclude the current Workshop map.
- ON retains the required addon flow. Preference writes are game-thread-only.
- Persistence failure rolls membership back, clears the writable latch, and
  does not run the success cache callback. Missing preferences default to ON.
- JSONC parsing validates uint64 IDs, tokens and comments. Numeric duplicate IDs
  are rejected even when their first value is false, before publishing output.

The RSS allowlist remains in source. The packaged config is the neutral upstream
example, with no automatic RSS mounts; the private operational RSS config is
not distributed. Configure your own addon IDs and timeout policy. Existing
installations must preserve their configuration and preference data on upgrade.
This packaging choice does not change the RSS preference code or original local
configuration. Config values are loaded once on plugin load.

## Build and validation

See BUILDING_RSS.md, dependencies.lock.json and THIRD_PARTY_NOTICES.md.
Linux x86-64 SteamRT3 is the release target. Source commits and artifact hashes
are recorded in each release. No Windows/SteamRT4 result or successful live
server load is implied by compilation. No production deployment is performed.
The old 67-check test suite is retained with two false-first duplicate rejection
checks. Tests use the product helper directly and preserve caller output on
failure. They do not emulate engine/KHook/network runtime behavior.

Upstream CI is retained as a non-executable reference under docs/ rather than
activated for this fork. No Pages or automatic release workflow is enabled.

## License and provenance

This is a modified distribution of https://github.com/Source2ZE/MultiAddonManager.
Original copyright notices and GPL version 3 are preserved in LICENSE and source.
RSS changes and test additions are distributed under GPL-3.0-only. KHook and other
dependencies have their own notices and pinned upstream sources. This project
contains no ModSharp code, game binary, user preferences, credentials or dumps.
All new project commit messages use English without Conventional Commit prefixes.
