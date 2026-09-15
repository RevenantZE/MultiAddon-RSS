#pragma once

// Pure C++17 state/list helpers for the explicit RSS asset flow.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rss_flow {

enum class Mode : std::uint32_t { Disabled = 0, MountOnly = 1, DownloadAndMount = 2 };
enum class Phase : std::uint32_t { None = 0, Queued, AwaitingReconnect, Staging, AwaitingActive, Active, Failed, Cancelled };
enum class Result : std::uint32_t { Ok = 0, Queued, InvalidArgument, Unavailable, NotReady, Busy, NoAddons, StoreReadOnly, PersistFailed, SendFailed, TimedOut, Cancelled, ConfigChanged, SessionChanged, AuthenticationFailed };
enum class ListPurpose : std::uint32_t { Download = 0, ReplyMount, SignonFilter };

typedef std::chrono::steady_clock Clock;
typedef Clock::time_point TimePoint;

struct Session
{
	std::int32_t slot = -1;
	std::int32_t userId = -1;
	std::uint64_t serial = 0;
};

inline bool SameSession(const Session &a, const Session &b)
{
	return a.slot == b.slot && a.userId == b.userId && a.serial != 0 && a.serial == b.serial;
}

struct Flow
{
	std::uint64_t providerEpoch = 0;
	std::uint64_t flowId = 0;
	std::uint32_t addonGeneration = 0;
	Mode mode = Mode::Disabled;
	Phase phase = Phase::None;
	Result result = Result::Ok;
	Session session;
	std::vector<std::string> addons;
	std::uint32_t completed = 0;
	std::string currentAddon;
	TimePoint hardDeadline{};
	TimePoint actionDeadline{};
	TimePoint terminalAt{};
	TimePoint pendingReplyDeadline{};
	bool reconnectRequested = false;
	bool disconnectObserved = false;
	bool stageMessageDelivered = false;
	bool mountListDelivered = false;
	bool clientActiveObserved = false;
	bool pendingReplyObserved = false;
	bool pendingMountListDelivered = false;
	std::int32_t pendingReplySlot = -1;
	std::int32_t pendingReplyUserId = -1;
};

inline bool IsTerminal(Phase phase)
{
	return phase == Phase::Active || phase == Phase::Failed || phase == Phase::Cancelled;
}

enum class TerminalRetentionResult : std::uint32_t
{
	NotTerminal = 0,
	RetainHistory,
	EraseHistory
};

inline TerminalRetentionResult ApplyTerminalRetention(const Flow &flow, TimePoint now,
	std::chrono::seconds retention, std::string *sharedPendingAddon)
{
	// Terminal flow retention never owns the current connection's shared pending
	// addon. Keeping the pointer in this common production/test helper makes that
	// non-mutation contract directly regression-testable.
	(void)sharedPendingAddon;
	if (!IsTerminal(flow.phase))
		return TerminalRetentionResult::NotTerminal;
	if (flow.terminalAt != TimePoint{} && now - flow.terminalAt >= retention)
		return TerminalRetentionResult::EraseHistory;
	return TerminalRetentionResult::RetainHistory;
}

inline bool IsWithinActionDeadline(const Flow &flow, TimePoint now)
{
	return flow.actionDeadline != TimePoint{} && now < flow.actionDeadline &&
		now < flow.hardDeadline;
}

inline bool ShouldRecordCompletedAddon(Mode mode, bool rssAddon, bool explicitRssPending)
{
	return !explicitRssPending && (mode == Mode::DownloadAndMount || !rssAddon);
}

enum class GenericCompletionAction : std::uint32_t
{
	KeepPending = 0,
	ClearOnly,
	RecordAndClear
};

inline GenericCompletionAction DecideGenericCompletion(Mode mode, bool rssAddon,
	bool hasExplicitFlow, bool allowGenericRss, bool withinTimeout)
{
	if (rssAddon && !allowGenericRss)
		return GenericCompletionAction::KeepPending;
	if (rssAddon && hasExplicitFlow)
		return GenericCompletionAction::ClearOnly;
	return withinTimeout && ShouldRecordCompletedAddon(mode, rssAddon, false) ?
		GenericCompletionAction::RecordAndClear : GenericCompletionAction::ClearOnly;
}

inline bool PublishesGenericRssCache(GenericCompletionAction action, bool rssAddon)
{
	return rssAddon && action == GenericCompletionAction::RecordAndClear;
}

inline bool ShouldSuppressRssForGenericFlow(const Flow *flow, const Session *currentSession)
{
	if (!flow || flow->flowId == 0)
		return false;
	return !IsTerminal(flow->phase) ||
		(currentSession && SameSession(flow->session, *currentSession));
}

inline Mode AppliedModeForDeliveredList(Mode configured,
	const std::vector<std::string> &effectiveRss,
	const std::vector<std::string> &delivered)
{
	if (configured == Mode::Disabled || effectiveRss.empty())
		return Mode::Disabled;
	for (std::size_t i = 0; i < effectiveRss.size(); ++i)
		if (std::find(delivered.begin(), delivered.end(), effectiveRss[i]) == delivered.end())
			return Mode::Disabled;
	return configured;
}

struct DisconnectEvidence
{
	std::uint64_t providerEpoch = 0;
	std::uint64_t serverEpoch = 0;
	std::uint64_t steamId = 0;
	Session session;
};

inline bool MatchesDisconnectCallback(const DisconnectEvidence &evidence,
	std::uint64_t providerEpoch, std::uint64_t serverEpoch, std::uint64_t steamId,
	std::int32_t slot, const Session &stored)
{
	return evidence.providerEpoch == providerEpoch && evidence.serverEpoch == serverEpoch &&
		evidence.steamId == steamId && evidence.session.slot == slot &&
		SameSession(evidence.session, stored);
}

enum class DisconnectConsumeResult : std::uint32_t
{
	NotFound = 0,
	Mismatch,
	Consumed
};

template <typename EvidenceMap>
inline DisconnectConsumeResult TryConsumeDisconnectEvidence(
	EvidenceMap &evidenceBySteamId,
	std::uint64_t providerEpoch, std::uint64_t serverEpoch, std::uint64_t steamId,
	std::int32_t slot, const Session *stored, DisconnectEvidence *consumed)
{
	typename EvidenceMap::iterator it = evidenceBySteamId.find(steamId);
	if (it == evidenceBySteamId.end())
		return DisconnectConsumeResult::NotFound;
	if (!stored || !MatchesDisconnectCallback(it->second, providerEpoch, serverEpoch,
		steamId, slot, *stored))
		return DisconnectConsumeResult::Mismatch;
	if (consumed)
		*consumed = it->second;
	evidenceBySteamId.erase(it);
	return DisconnectConsumeResult::Consumed;
}

inline bool CanUseFlowCurrentAddon(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session *boundSession, std::int32_t clientSlot,
	std::int32_t clientUserId, TimePoint now)
{
	if (IsTerminal(flow.phase) || flow.flowId == 0 || flow.mode != Mode::DownloadAndMount ||
		flow.providerEpoch != providerEpoch || flow.addonGeneration != addonGeneration ||
		flow.currentAddon.empty() || !IsWithinActionDeadline(flow, now))
		return false;
	if (flow.phase != Phase::AwaitingReconnect)
		return false;
	if (!flow.disconnectObserved && boundSession && SameSession(flow.session, *boundSession))
		return boundSession->slot == clientSlot && boundSession->userId == clientUserId;
	return flow.disconnectObserved && flow.pendingReplyObserved &&
		flow.pendingReplySlot == clientSlot && flow.pendingReplyUserId == clientUserId &&
		flow.pendingReplyDeadline != TimePoint{} && now < flow.pendingReplyDeadline;
}

inline bool CanUseFlowReplyMount(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session *boundSession, std::int32_t replySlot,
	std::int32_t replyUserId, TimePoint now)
{
	if (IsTerminal(flow.phase) || flow.flowId == 0 ||
		flow.providerEpoch != providerEpoch || flow.addonGeneration != addonGeneration ||
		!IsWithinActionDeadline(flow, now))
		return false;
	if (flow.phase == Phase::AwaitingActive && boundSession &&
		SameSession(flow.session, *boundSession))
		return boundSession->slot == replySlot && boundSession->userId == replyUserId;
	return flow.phase == Phase::AwaitingReconnect && flow.disconnectObserved &&
		flow.pendingReplyObserved && flow.pendingReplySlot == replySlot &&
		flow.pendingReplyUserId == replyUserId && flow.pendingReplyDeadline != TimePoint{} &&
		now < flow.pendingReplyDeadline;
}

inline Mode ResolveListMode(Mode configured, bool replyMount, bool genericPath,
	const Flow *flow, std::uint64_t providerEpoch, std::uint32_t addonGeneration,
	const Session *boundSession, std::int32_t slot, std::int32_t userId, TimePoint now)
{
	Mode resolved = configured;
	if (replyMount && flow && !IsTerminal(flow->phase) &&
		!CanUseFlowReplyMount(*flow, providerEpoch, addonGeneration, boundSession,
			slot, userId, now))
		resolved = Mode::Disabled;
	if (genericPath && ShouldSuppressRssForGenericFlow(flow, boundSession))
		resolved = Mode::Disabled;
	return resolved;
}

inline bool CanPublishRssCompletion(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration)
{
	return flow.phase == Phase::Active && flow.result == Result::Ok &&
		flow.mode == Mode::DownloadAndMount &&
		flow.providerEpoch == providerEpoch && flow.addonGeneration == addonGeneration &&
		flow.completed >= flow.addons.size() && flow.mountListDelivered &&
		flow.clientActiveObserved;
}

inline bool MatchesTimeoutToken(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, std::uint64_t flowId, const Session &session)
{
	return !IsTerminal(flow.phase) && flow.providerEpoch == providerEpoch &&
		flow.addonGeneration == addonGeneration && flow.flowId == flowId &&
		SameSession(flow.session, session);
}

inline bool CanRunLegacyTimeout(bool hasAnyExplicitFlow)
{
	return !hasAnyExplicitFlow;
}

inline void Finish(Flow &flow, Phase phase, Result result, TimePoint now)
{
	flow.phase = phase;
	flow.result = result;
	flow.terminalAt = now;
	flow.currentAddon.clear();
	flow.pendingReplyObserved = false;
	flow.pendingMountListDelivered = false;
	flow.pendingReplySlot = -1;
	flow.pendingReplyUserId = -1;
	flow.pendingReplyDeadline = TimePoint{};
}

inline std::uint32_t RemainingSeconds(const Flow &flow, TimePoint now)
{
	if (flow.hardDeadline == TimePoint{} || now >= flow.hardDeadline)
		return 0;
	const Clock::duration left = flow.hardDeadline - now;
	const std::chrono::seconds whole = std::chrono::duration_cast<std::chrono::seconds>(left);
	const bool partial = std::chrono::duration_cast<Clock::duration>(whole) < left;
	const std::uint64_t seconds = static_cast<std::uint64_t>(whole.count()) + (partial ? 1ULL : 0ULL);
	return seconds > 0xffffffffULL ? 0xffffffffU : static_cast<std::uint32_t>(seconds);
}

inline bool IsEffectiveRss(const std::string &id, const std::vector<std::string> &extra,
	const std::string &workshopMap)
{
	if (!workshopMap.empty() && id == workshopMap)
		return false;
	return std::find(extra.begin(), extra.end(), id) != extra.end();
}

inline void AddUnique(std::vector<std::string> &out, const std::string &id)
{
	if (!id.empty() && std::find(out.begin(), out.end(), id) == out.end())
		out.push_back(id);
}

inline std::vector<std::string> EffectiveExtra(const std::vector<std::string> &extra,
	const std::string &workshopMap)
{
	std::vector<std::string> out;
	for (std::size_t i = 0; i < extra.size(); ++i)
		if ((workshopMap.empty() || extra[i] != workshopMap))
			AddUnique(out, extra[i]);
	return out;
}

inline std::vector<std::string> BuildList(const std::vector<std::string> &base,
	const std::vector<std::string> &extra, const std::string &workshopMap, Mode mode,
	ListPurpose purpose, const std::vector<std::string> &completed,
	const std::string &currentAddon)
{
	std::vector<std::string> out;
	for (std::size_t i = 0; i < base.size(); ++i)
	{
		const bool rss = IsEffectiveRss(base[i], extra, workshopMap);
		bool include = !rss;
		if (rss && mode == Mode::MountOnly && purpose == ListPurpose::ReplyMount)
			include = true;
		if (rss && mode == Mode::DownloadAndMount)
		{
			if (purpose == ListPurpose::Download)
				include = true;
			else if (purpose == ListPurpose::SignonFilter)
				include = currentAddon.empty() || base[i] == currentAddon;
			else
				include = std::find(completed.begin(), completed.end(), base[i]) != completed.end() ||
					base[i] == currentAddon;
		}
		if (include)
			AddUnique(out, base[i]);
	}

	return out;
}

inline bool CanUseFlow(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session &session)
{
	return !IsTerminal(flow.phase) && flow.providerEpoch == providerEpoch &&
		flow.addonGeneration == addonGeneration && SameSession(flow.session, session);
}

inline bool StartDeferred(Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session &session, TimePoint now,
	std::chrono::seconds leaveTimeout)
{
	if (flow.phase != Phase::Queued || !CanUseFlow(flow, providerEpoch, addonGeneration, session) ||
		now >= flow.hardDeadline)
		return false;
	flow.phase = Phase::AwaitingReconnect;
	flow.result = Result::Queued;
	flow.reconnectRequested = true;
	flow.disconnectObserved = false;
	flow.stageMessageDelivered = false;
	flow.pendingReplyObserved = false;
	flow.pendingMountListDelivered = false;
	flow.pendingReplySlot = -1;
	flow.pendingReplyUserId = -1;
	flow.pendingReplyDeadline = TimePoint{};
	flow.actionDeadline = std::min(flow.hardDeadline, now + leaveTimeout);
	if (flow.mode == Mode::DownloadAndMount && flow.completed < flow.addons.size())
		flow.currentAddon = flow.addons[flow.completed];
	return true;
}

inline bool MarkStageMessageDelivered(Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session &session, const std::string &addon,
	TimePoint now)
{
	if (flow.phase != Phase::AwaitingReconnect || flow.mode != Mode::DownloadAndMount ||
		flow.providerEpoch != providerEpoch || flow.addonGeneration != addonGeneration ||
		flow.stageMessageDelivered || addon.empty() || flow.currentAddon != addon ||
		!SameSession(flow.session, session) || !IsWithinActionDeadline(flow, now))
		return false;
	flow.stageMessageDelivered = true;
	return true;
}

inline bool ObserveDisconnect(Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session &session, TimePoint now,
	std::chrono::seconds reconnectTimeout)
{
	if (flow.phase != Phase::AwaitingReconnect || flow.disconnectObserved ||
		flow.providerEpoch != providerEpoch || flow.addonGeneration != addonGeneration ||
		!SameSession(flow.session, session) || !IsWithinActionDeadline(flow, now))
		return false;
	flow.disconnectObserved = true;
	flow.actionDeadline = std::min(flow.hardDeadline, now + reconnectTimeout);
	return true;
}

inline bool ObserveReconnect(Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session &newSession, TimePoint now,
	std::chrono::seconds stageTimeout)
{
	if (flow.phase != Phase::AwaitingReconnect || !flow.disconnectObserved || flow.providerEpoch != providerEpoch ||
		flow.addonGeneration != addonGeneration || !IsWithinActionDeadline(flow, now))
		return false;
	flow.session = newSession;
	flow.mountListDelivered = false;
	flow.clientActiveObserved = false;
	flow.stageMessageDelivered = false;
	if (flow.mode == Mode::DownloadAndMount && !flow.currentAddon.empty())
	{
		++flow.completed;
		flow.currentAddon.clear();
	}
	if (flow.mode == Mode::MountOnly || flow.completed >= flow.addons.size())
	{
		flow.phase = Phase::AwaitingActive;
		if (flow.pendingReplyObserved && flow.pendingReplyDeadline != TimePoint{})
			flow.actionDeadline = std::min(flow.actionDeadline, flow.pendingReplyDeadline);
	}
	else
	{
		flow.phase = Phase::Staging;
		flow.actionDeadline = std::min(flow.hardDeadline, now + stageTimeout);
	}
	return true;
}

inline bool QueueNextStage(Flow &flow, const Session &session, TimePoint now,
	std::chrono::seconds leaveTimeout)
{
	if (flow.phase != Phase::Staging || !SameSession(flow.session, session) ||
		flow.completed >= flow.addons.size() || !IsWithinActionDeadline(flow, now))
		return false;
	flow.phase = Phase::AwaitingReconnect;
	flow.disconnectObserved = false;
	flow.stageMessageDelivered = false;
	flow.pendingReplyObserved = false;
	flow.pendingMountListDelivered = false;
	flow.pendingReplySlot = -1;
	flow.pendingReplyUserId = -1;
	flow.pendingReplyDeadline = TimePoint{};
	flow.currentAddon = flow.addons[flow.completed];
	flow.actionDeadline = std::min(flow.hardDeadline, now + leaveTimeout);
	return true;
}

inline bool MarkMountListDelivered(Flow &flow, const Session &session, TimePoint now,
	std::chrono::seconds activeTimeout)
{
	if (flow.phase != Phase::AwaitingActive || IsTerminal(flow.phase) ||
		!SameSession(flow.session, session) || !IsWithinActionDeadline(flow, now))
		return false;
	if (!flow.mountListDelivered)
	{
		flow.mountListDelivered = true;
		flow.actionDeadline = std::min(flow.hardDeadline, now + activeTimeout);
	}
	if (flow.clientActiveObserved)
		Finish(flow, Phase::Active, Result::Ok, now);
	return true;
}

inline bool MarkClientActive(Flow &flow, const Session &session, TimePoint now)
{
	if (flow.phase != Phase::AwaitingActive || IsTerminal(flow.phase) ||
		!SameSession(flow.session, session) || !IsWithinActionDeadline(flow, now))
		return false;
	flow.clientActiveObserved = true;
	if (flow.mountListDelivered)
		Finish(flow, Phase::Active, Result::Ok, now);
	return true;
}

inline bool ObservePendingReply(Flow &flow, std::int32_t slot, std::int32_t userId,
	TimePoint now, std::chrono::seconds activeTimeout)
{
	if (flow.phase != Phase::AwaitingReconnect || IsTerminal(flow.phase) ||
		!flow.disconnectObserved || slot < 0 || userId < 0 ||
		!IsWithinActionDeadline(flow, now))
		return false;
	if (flow.pendingReplyObserved)
		return flow.pendingReplySlot == slot && flow.pendingReplyUserId == userId &&
			flow.pendingReplyDeadline != TimePoint{} && now < flow.pendingReplyDeadline;
	flow.pendingReplyObserved = true;
	flow.pendingReplySlot = slot;
	flow.pendingReplyUserId = userId;
	flow.pendingReplyDeadline = std::min(flow.hardDeadline, now + activeTimeout);
	return true;
}

inline bool MarkPendingMountListDelivered(Flow &flow, std::int32_t slot,
	std::int32_t userId, TimePoint now)
{
	if (!flow.pendingReplyObserved || flow.pendingReplySlot != slot ||
		flow.pendingReplyUserId != userId || !IsWithinActionDeadline(flow, now) ||
		flow.pendingReplyDeadline == TimePoint{} || now >= flow.pendingReplyDeadline)
		return false;
	flow.pendingMountListDelivered = true;
	return true;
}

inline bool CanUseFlowLocalCompleted(const Flow &flow, std::uint64_t providerEpoch,
	std::uint32_t addonGeneration, const Session *boundSession, std::int32_t replySlot,
	std::int32_t replyUserId, TimePoint now)
{
	if (IsTerminal(flow.phase) || flow.flowId == 0 || flow.mode != Mode::DownloadAndMount ||
		flow.providerEpoch != providerEpoch || flow.addonGeneration != addonGeneration ||
		!IsWithinActionDeadline(flow, now))
		return false;
	if (flow.phase == Phase::AwaitingActive && boundSession &&
		SameSession(flow.session, *boundSession))
		return boundSession->slot == replySlot && boundSession->userId == replyUserId;
	return flow.phase == Phase::AwaitingReconnect && flow.disconnectObserved &&
		flow.pendingReplyObserved && flow.pendingReplySlot == replySlot &&
		flow.pendingReplyUserId == replyUserId && flow.pendingReplyDeadline != TimePoint{} &&
		now < flow.pendingReplyDeadline;
}

inline bool PromotePendingMountList(Flow &flow, const Session &session, TimePoint now)
{
	const TimePoint firstReplyDeadline = flow.pendingReplyDeadline;
	const bool matches = flow.pendingReplyObserved && flow.pendingMountListDelivered &&
		flow.pendingReplySlot == session.slot && flow.pendingReplyUserId == session.userId;
	flow.pendingReplyObserved = false;
	flow.pendingMountListDelivered = false;
	flow.pendingReplySlot = -1;
	flow.pendingReplyUserId = -1;
	flow.pendingReplyDeadline = TimePoint{};
	if (!matches || firstReplyDeadline == TimePoint{} || now >= firstReplyDeadline ||
		flow.phase != Phase::AwaitingActive || !SameSession(flow.session, session))
		return false;
	flow.mountListDelivered = true;
	flow.actionDeadline = std::min(flow.hardDeadline, firstReplyDeadline);
	if (flow.clientActiveObserved)
		Finish(flow, Phase::Active, Result::Ok, now);
	return true;
}

} // namespace rss_flow
