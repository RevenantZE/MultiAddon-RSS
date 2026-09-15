#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "rss_asset_flow.h"
#include "rss_asset_preferences.h"

static int s_pass = 0;
static int s_fail = 0;

#define CHECK(expr) do { if (expr) { ++s_pass; } else { ++s_fail; \
	std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); } } while (0)

static bool Equal(const std::vector<std::string> &a, std::initializer_list<const char *> b)
{
	if (a.size() != b.size())
		return false;
	std::size_t i = 0;
	for (std::initializer_list<const char *>::const_iterator it = b.begin(); it != b.end(); ++it, ++i)
		if (a[i] != *it)
			return false;
	return true;
}

int main()
{
	const std::vector<std::string> base{"map", "100", "base", "100", "200"};
	const std::vector<std::string> extra{"100", "200", "map", "100"};
	const auto missingMode = rss_prefs::ResolveConfiguredMode<rss_flow::Mode>(nullptr, true);
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", missingMode,
		rss_flow::ListPurpose::ReplyMount, {}, ""), {"map", "100", "base", "200"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", missingMode,
		rss_flow::ListPurpose::Download, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", missingMode,
		rss_flow::ListPurpose::SignonFilter, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::EffectiveExtra(extra, "map"), {"100", "200"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::Disabled,
		rss_flow::ListPurpose::Download, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::Disabled,
		rss_flow::ListPurpose::ReplyMount, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::MountOnly,
		rss_flow::ListPurpose::Download, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::MountOnly,
		rss_flow::ListPurpose::ReplyMount, {}, ""), {"map", "100", "base", "200"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::MountOnly,
		rss_flow::ListPurpose::SignonFilter, {}, ""), {"map", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::DownloadAndMount,
		rss_flow::ListPurpose::Download, {}, ""), {"map", "100", "base", "200"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::DownloadAndMount,
		rss_flow::ListPurpose::ReplyMount, {"100"}, "200"), {"map", "100", "base", "200"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::DownloadAndMount,
		rss_flow::ListPurpose::ReplyMount, {}, "100"), {"map", "100", "base"}));
	CHECK(Equal(rss_flow::BuildList(base, extra, "map", rss_flow::Mode::DownloadAndMount,
		rss_flow::ListPurpose::SignonFilter, {}, "200"), {"map", "base", "200"}));
	CHECK(rss_flow::ShouldRecordCompletedAddon(rss_flow::Mode::Disabled, false, false));
	CHECK(rss_flow::ShouldRecordCompletedAddon(rss_flow::Mode::MountOnly, false, false));
	CHECK(!rss_flow::ShouldRecordCompletedAddon(rss_flow::Mode::Disabled, true, false));
	CHECK(!rss_flow::ShouldRecordCompletedAddon(rss_flow::Mode::MountOnly, true, false));
	CHECK(!rss_flow::ShouldRecordCompletedAddon(rss_flow::Mode::DownloadAndMount, true, true));
	CHECK(rss_flow::DecideGenericCompletion(rss_flow::Mode::Disabled, false, true, false, true) ==
		rss_flow::GenericCompletionAction::RecordAndClear);
	CHECK(rss_flow::DecideGenericCompletion(rss_flow::Mode::DownloadAndMount, true, true, false, true) ==
		rss_flow::GenericCompletionAction::KeepPending);
	CHECK(rss_flow::DecideGenericCompletion(rss_flow::Mode::DownloadAndMount, true, true, true, true) ==
		rss_flow::GenericCompletionAction::ClearOnly);
	CHECK(rss_flow::DecideGenericCompletion(rss_flow::Mode::DownloadAndMount, true, false, true, true) ==
		rss_flow::GenericCompletionAction::RecordAndClear);
	CHECK(rss_flow::PublishesGenericRssCache(
		rss_flow::DecideGenericCompletion(rss_flow::Mode::DownloadAndMount,
			true, false, true, true), true));
	CHECK(!rss_flow::PublishesGenericRssCache(
		rss_flow::DecideGenericCompletion(rss_flow::Mode::DownloadAndMount,
			true, true, true, true), true));

	const rss_flow::TimePoint t0(std::chrono::seconds(100));
	const rss_flow::Session first{3, 40, 1};
	const rss_flow::Session second{3, 41, 2};
	const rss_flow::Session third{3, 42, 3};
	{
		// The callback can consume only the immutable token captured from the
		// concrete disconnect source. A mismatch preserves the token so the
		// matching callback can consume it exactly once.
		const rss_flow::DisconnectEvidence evidence{5, 6, 7656119, second};
		std::unordered_map<std::uint64_t, rss_flow::DisconnectEvidence> evidenceStore;
		evidenceStore.emplace(7656119, evidence);
		rss_flow::DisconnectEvidence consumed;
		CHECK(rss_flow::TryConsumeDisconnectEvidence(evidenceStore, 5, 6, 7656119,
			second.slot + 1, &second, &consumed) ==
			rss_flow::DisconnectConsumeResult::Mismatch);
		CHECK(evidenceStore.size() == 1 &&
			rss_flow::SameSession(evidenceStore.at(7656119).session, second));
		CHECK(rss_flow::TryConsumeDisconnectEvidence(evidenceStore, 5, 6, 7656119,
			second.slot, &second, &consumed) ==
			rss_flow::DisconnectConsumeResult::Consumed);
		CHECK(evidenceStore.empty() && consumed.providerEpoch == 5 &&
			consumed.serverEpoch == 6 && consumed.steamId == 7656119 &&
			rss_flow::SameSession(consumed.session, second));
		CHECK(rss_flow::TryConsumeDisconnectEvidence(evidenceStore, 5, 6, 7656119,
			second.slot, &second, &consumed) ==
			rss_flow::DisconnectConsumeResult::NotFound);
		CHECK(evidenceStore.empty());
	}
	{
		// Terminal retention owns only flow history. A new connection's shared
		// generic pending addon survives every retention frame.
		rss_flow::Flow failed;
		failed.phase = rss_flow::Phase::Failed;
		failed.terminalAt = t0;
		std::string newConnectionPending = "100";
		CHECK(rss_flow::ApplyTerminalRetention(failed,
			t0 + std::chrono::seconds(59), std::chrono::seconds(60),
			&newConnectionPending) == rss_flow::TerminalRetentionResult::RetainHistory);
		CHECK(newConnectionPending == "100");
		CHECK(rss_flow::ApplyTerminalRetention(failed,
			t0 + std::chrono::seconds(60), std::chrono::seconds(60),
			&newConnectionPending) == rss_flow::TerminalRetentionResult::EraseHistory);
		CHECK(newConnectionPending == "100");
	}
	{
		rss_flow::Flow flow;
		flow.providerEpoch = 5;
		flow.flowId = 9;
		flow.addonGeneration = 7;
		flow.mode = rss_flow::Mode::MountOnly;
		flow.phase = rss_flow::Phase::Queued;
		flow.result = rss_flow::Result::Queued;
		flow.session = first;
		flow.addons = {"100", "200"};
		flow.hardDeadline = t0 + std::chrono::seconds(120);
		CHECK(rss_flow::StartDeferred(flow, 5, 7, first, t0, std::chrono::seconds(15)));
		CHECK(flow.phase == rss_flow::Phase::AwaitingReconnect && flow.reconnectRequested &&
			flow.currentAddon.empty());
		CHECK(!rss_flow::ObserveReconnect(flow, 5, 7, second, t0, std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveDisconnect(flow, 5, 7, first, t0 + std::chrono::milliseconds(500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveReconnect(flow, 5, 7, second, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(flow.phase == rss_flow::Phase::AwaitingActive && flow.completed == 0);
		CHECK(rss_flow::MarkClientActive(flow, second, t0 + std::chrono::seconds(2)));
		CHECK(flow.phase == rss_flow::Phase::AwaitingActive && flow.clientActiveObserved);
		CHECK(rss_flow::MarkMountListDelivered(flow, second, t0 + std::chrono::seconds(2),
			std::chrono::seconds(30)));
		CHECK(flow.phase == rss_flow::Phase::Active && flow.result == rss_flow::Result::Ok &&
			flow.clientActiveObserved);
		CHECK(!rss_flow::CanPublishRssCompletion(flow, 5, 7));
		CHECK(!rss_flow::CanPublishRssCompletion(flow, 6, 7));
		CHECK(!rss_flow::MatchesTimeoutToken(flow, 5, 7, 9, second));
		CHECK(!rss_flow::MarkClientActive(flow, second, t0 + std::chrono::seconds(3)));
	}
	{
		rss_flow::Flow flow;
		flow.providerEpoch = 5;
		flow.flowId = 10;
		flow.addonGeneration = 7;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::Queued;
		flow.result = rss_flow::Result::Queued;
		flow.session = first;
		flow.addons = {"100", "200"};
		flow.hardDeadline = t0 + std::chrono::seconds(120);
		CHECK(rss_flow::StartDeferred(flow, 5, 7, first, t0, std::chrono::seconds(15)));
		CHECK(flow.currentAddon == "100");
		CHECK(rss_flow::MarkStageMessageDelivered(flow, 5, 7, first, "100", t0));
		CHECK(!rss_flow::MarkStageMessageDelivered(flow, 5, 7, first, "100", t0));
		CHECK(rss_flow::ObserveDisconnect(flow, 5, 7, first, t0 + std::chrono::milliseconds(500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveReconnect(flow, 5, 7, second, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(flow.phase == rss_flow::Phase::Staging && flow.completed == 1);
		CHECK(!rss_flow::QueueNextStage(flow, first, t0 + std::chrono::seconds(2),
			std::chrono::seconds(15)));
		CHECK(rss_flow::QueueNextStage(flow, second, t0 + std::chrono::seconds(2),
			std::chrono::seconds(15)) && flow.currentAddon == "200");
		CHECK(rss_flow::MatchesTimeoutToken(flow, 5, 7, 10, second));
		CHECK(!rss_flow::MatchesTimeoutToken(flow, 5, 8, 10, second));
		CHECK(!rss_flow::MatchesTimeoutToken(flow, 5, 7, 10, third));
		CHECK(rss_flow::MarkStageMessageDelivered(flow, 5, 7, second, "200",
			t0 + std::chrono::seconds(2)));
		CHECK(!rss_flow::ObserveDisconnect(flow, 5, 7, first, t0 + std::chrono::milliseconds(2500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveDisconnect(flow, 5, 7, second, t0 + std::chrono::milliseconds(2500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveReconnect(flow, 5, 7, third, t0 + std::chrono::seconds(3),
			std::chrono::seconds(30)));
		CHECK(flow.phase == rss_flow::Phase::AwaitingActive && flow.completed == 2);
		CHECK(!rss_flow::MarkMountListDelivered(flow, second, t0 + std::chrono::seconds(4),
			std::chrono::seconds(30)));
		CHECK(rss_flow::MarkMountListDelivered(flow, third, t0 + std::chrono::seconds(4),
			std::chrono::seconds(30)));
		CHECK(rss_flow::MarkClientActive(flow, third, t0 + std::chrono::seconds(5)));
		CHECK(flow.phase == rss_flow::Phase::Active);
		CHECK(rss_flow::CanPublishRssCompletion(flow, 5, 7));
		CHECK(rss_flow::RemainingSeconds(flow, t0 + std::chrono::seconds(119)) == 1);
		rss_flow::Finish(flow, rss_flow::Phase::Cancelled, rss_flow::Result::Cancelled,
			t0 + std::chrono::seconds(6));
		CHECK(flow.phase == rss_flow::Phase::Cancelled && flow.result == rss_flow::Result::Cancelled);
	}
	{
		rss_flow::Flow flow;
		flow.providerEpoch = 1;
		flow.addonGeneration = 2;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.hardDeadline = t0 + std::chrono::seconds(5);
		CHECK(!rss_flow::StartDeferred(flow, 2, 2, first, t0, std::chrono::seconds(1)));
		CHECK(!rss_flow::StartDeferred(flow, 1, 3, first, t0, std::chrono::seconds(1)));
		CHECK(!rss_flow::StartDeferred(flow, 1, 2, second, t0, std::chrono::seconds(1)));
		CHECK(rss_flow::RemainingSeconds(flow, t0 + std::chrono::milliseconds(4500)) == 1);
		CHECK(rss_flow::RemainingSeconds(flow, t0 + std::chrono::seconds(5)) == 0);
	}
	{
		// ReplyConnection may precede the accepted/authenticated ClientActive.
		rss_flow::Flow flow;
		flow.providerEpoch = 8;
		flow.addonGeneration = 9;
		flow.mode = rss_flow::Mode::MountOnly;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.hardDeadline = t0 + std::chrono::seconds(60);
		CHECK(rss_flow::StartDeferred(flow, 8, 9, first, t0, std::chrono::seconds(15)));
		CHECK(rss_flow::ObserveDisconnect(flow, 8, 9, first, t0 + std::chrono::milliseconds(500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObservePendingReply(flow, second.slot, second.userId,
			t0 + std::chrono::milliseconds(750), std::chrono::seconds(30)));
		CHECK(rss_flow::MarkPendingMountListDelivered(flow, second.slot, second.userId,
			t0 + std::chrono::milliseconds(800)));
		CHECK(rss_flow::ObserveReconnect(flow, 8, 9, second, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(rss_flow::PromotePendingMountList(flow, second, t0 + std::chrono::seconds(2)));
		CHECK(flow.mountListDelivered && !flow.pendingMountListDelivered);
		CHECK(rss_flow::MarkClientActive(flow, second, t0 + std::chrono::seconds(3)));
		CHECK(flow.phase == rss_flow::Phase::Active);
		CHECK(!rss_flow::ObservePendingReply(flow, third.slot, third.userId,
			t0 + std::chrono::seconds(4), std::chrono::seconds(30)));
	}
	{
		// A pending reply for another engine userID must not bind to this session.
		rss_flow::Flow flow;
		flow.providerEpoch = 8;
		flow.addonGeneration = 9;
		flow.mode = rss_flow::Mode::MountOnly;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.hardDeadline = t0 + std::chrono::seconds(60);
		CHECK(rss_flow::StartDeferred(flow, 8, 9, first, t0, std::chrono::seconds(15)));
		CHECK(rss_flow::ObserveDisconnect(flow, 8, 9, first, t0 + std::chrono::milliseconds(500),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObservePendingReply(flow, second.slot, second.userId,
			t0 + std::chrono::milliseconds(750), std::chrono::seconds(30)));
		CHECK(rss_flow::MarkPendingMountListDelivered(flow, second.slot, second.userId,
			t0 + std::chrono::milliseconds(800)));
		CHECK(rss_flow::ObserveReconnect(flow, 8, 9, third, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(!rss_flow::PromotePendingMountList(flow, third, t0 + std::chrono::seconds(2)));
		CHECK(!flow.mountListDelivered && !flow.pendingMountListDelivered);
	}
	{
		// A callback arriving at or after the fixed leave deadline cannot create
		// a fresh reconnect timeout.
		rss_flow::Flow flow;
		flow.providerEpoch = 11;
		flow.flowId = 12;
		flow.addonGeneration = 13;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.addons = {"100"};
		flow.hardDeadline = t0 + std::chrono::seconds(60);
		CHECK(rss_flow::StartDeferred(flow, 11, 13, first, t0, std::chrono::seconds(15)));
		CHECK(!rss_flow::ObserveDisconnect(flow, 11, 13, first, t0 + std::chrono::seconds(15),
			std::chrono::seconds(30)));
		CHECK(!flow.disconnectObserved);
	}
	{
		// Duplicate ReplyConnection evidence does not extend the first 30-second
		// Active deadline, and a callback at the deadline is rejected.
		rss_flow::Flow flow;
		flow.providerEpoch = 11;
		flow.flowId = 13;
		flow.addonGeneration = 13;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.addons = {"100"};
		flow.hardDeadline = t0 + std::chrono::seconds(120);
		CHECK(rss_flow::StartDeferred(flow, 11, 13, first, t0, std::chrono::seconds(15)));
		CHECK(rss_flow::ObserveDisconnect(flow, 11, 13, first, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObserveReconnect(flow, 11, 13, second,
			t0 + std::chrono::seconds(2), std::chrono::seconds(30)));
		CHECK(rss_flow::MarkMountListDelivered(flow, second, t0 + std::chrono::seconds(3),
			std::chrono::seconds(30)));
		const rss_flow::TimePoint firstReplyDeadline = flow.actionDeadline;
		CHECK(rss_flow::MarkMountListDelivered(flow, second, t0 + std::chrono::seconds(4),
			std::chrono::seconds(30)));
		CHECK(flow.actionDeadline == firstReplyDeadline);
		CHECK(!rss_flow::MarkClientActive(flow, second, firstReplyDeadline));
		CHECK(flow.phase == rss_flow::Phase::AwaitingActive);
	}
	{
		// When ReplyConnection is first, its timestamp still owns the 30-second
		// Active deadline; a later ClientActive cannot restart that clock.
		rss_flow::Flow flow;
		flow.providerEpoch = 14;
		flow.flowId = 15;
		flow.addonGeneration = 16;
		flow.mode = rss_flow::Mode::MountOnly;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.addons = {"100"};
		flow.hardDeadline = t0 + std::chrono::seconds(90);
		CHECK(rss_flow::StartDeferred(flow, 14, 16, first, t0, std::chrono::seconds(15)));
		CHECK(rss_flow::ObserveDisconnect(flow, 14, 16, first, t0 + std::chrono::seconds(1),
			std::chrono::seconds(60)));
		CHECK(rss_flow::ObservePendingReply(flow, second.slot, second.userId,
			t0 + std::chrono::seconds(2), std::chrono::seconds(30)));
		CHECK(rss_flow::MarkPendingMountListDelivered(flow, second.slot, second.userId,
			t0 + std::chrono::seconds(2)));
		CHECK(rss_flow::ShouldSuppressRssForGenericFlow(&flow, nullptr));
		CHECK(rss_flow::CanUseFlowReplyMount(flow, 14, 16, nullptr,
			second.slot, second.userId, t0 + std::chrono::seconds(2)));
		CHECK(!rss_flow::CanUseFlowReplyMount(flow, 14, 16, nullptr,
			third.slot, third.userId, t0 + std::chrono::seconds(2)));
		CHECK(rss_flow::ResolveListMode(rss_flow::Mode::MountOnly, true, false,
			&flow, 14, 16, nullptr, second.slot, second.userId,
			t0 + std::chrono::seconds(2)) == rss_flow::Mode::MountOnly);
		CHECK(rss_flow::ResolveListMode(rss_flow::Mode::MountOnly, true, false,
			&flow, 14, 16, nullptr, third.slot, third.userId,
			t0 + std::chrono::seconds(2)) == rss_flow::Mode::Disabled);
		CHECK(rss_flow::ObserveReconnect(flow, 14, 16, second,
			t0 + std::chrono::seconds(3), std::chrono::seconds(60)));
		CHECK(flow.actionDeadline == t0 + std::chrono::seconds(32));
		CHECK(!rss_flow::PromotePendingMountList(flow, second,
			t0 + std::chrono::seconds(32)));
		CHECK(!rss_flow::MarkClientActive(flow, second,
			t0 + std::chrono::seconds(32)));
	}
	{
		// Flow-local completion is usable only by the exact, live nonterminal
		// reply session. Terminal and replacement sessions cannot reuse it.
		rss_flow::Flow flow;
		flow.providerEpoch = 21;
		flow.flowId = 22;
		flow.addonGeneration = 23;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::AwaitingActive;
		flow.session = second;
		flow.addons = {"100"};
		flow.completed = 1;
		flow.hardDeadline = t0 + std::chrono::seconds(60);
		flow.actionDeadline = t0 + std::chrono::seconds(30);
		CHECK(rss_flow::CanUseFlowLocalCompleted(flow, 21, 23, &second,
			second.slot, second.userId, t0));
		CHECK(!rss_flow::CanUseFlowLocalCompleted(flow, 21, 23, &third,
			third.slot, third.userId, t0));
		rss_flow::Finish(flow, rss_flow::Phase::Failed, rss_flow::Result::TimedOut, t0);
		CHECK(!rss_flow::CanUseFlowLocalCompleted(flow, 21, 23, &second,
			second.slot, second.userId, t0));
	}
	{
		// Reply -> generic SendNetMessage -> ClientActive: the first explicit
		// stage marker is single-use while pending mount evidence survives.
		rss_flow::Flow flow;
		flow.providerEpoch = 31;
		flow.flowId = 32;
		flow.addonGeneration = 33;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::Queued;
		flow.session = first;
		flow.addons = {"100"};
		flow.hardDeadline = t0 + std::chrono::seconds(90);
		CHECK(rss_flow::StartDeferred(flow, 31, 33, first, t0, std::chrono::seconds(15)));
		CHECK(rss_flow::MarkStageMessageDelivered(flow, 31, 33, first, "100", t0));
		CHECK(rss_flow::ObserveDisconnect(flow, 31, 33, first, t0 + std::chrono::seconds(1),
			std::chrono::seconds(30)));
		CHECK(rss_flow::ObservePendingReply(flow, second.slot, second.userId,
			t0 + std::chrono::seconds(2), std::chrono::seconds(30)));
		CHECK(rss_flow::MarkPendingMountListDelivered(flow, second.slot, second.userId,
			t0 + std::chrono::seconds(2)));
		CHECK(rss_flow::CanUseFlowCurrentAddon(flow, 31, 33, nullptr,
			second.slot, second.userId, t0 + std::chrono::seconds(2)));
		CHECK(!rss_flow::CanUseFlowCurrentAddon(flow, 31, 33, nullptr,
			third.slot, third.userId, t0 + std::chrono::seconds(2)));
		CHECK(rss_flow::CanUseFlowReplyMount(flow, 31, 33, nullptr,
			second.slot, second.userId, t0 + std::chrono::seconds(2)));
		CHECK(!rss_flow::CanUseFlowReplyMount(flow, 31, 33, nullptr,
			third.slot, third.userId, t0 + std::chrono::seconds(2)));
		CHECK(!rss_flow::MarkStageMessageDelivered(flow, 31, 33, first, "100",
			t0 + std::chrono::seconds(2)));
		CHECK(rss_flow::ObserveReconnect(flow, 31, 33, second,
			t0 + std::chrono::seconds(3), std::chrono::seconds(30)));
		CHECK(rss_flow::PromotePendingMountList(flow, second,
			t0 + std::chrono::seconds(3)));
		CHECK(rss_flow::MarkClientActive(flow, second, t0 + std::chrono::seconds(4)));
		CHECK(flow.phase == rss_flow::Phase::Active);
		CHECK(rss_flow::ShouldSuppressRssForGenericFlow(&flow, &second));
		CHECK(!rss_flow::ShouldSuppressRssForGenericFlow(&flow, &third));
		CHECK(rss_flow::ResolveListMode(rss_flow::Mode::MountOnly, true, false,
			&flow, 31, 33, &third, third.slot, third.userId,
			t0 + std::chrono::seconds(5)) == rss_flow::Mode::MountOnly);
		CHECK(rss_flow::ResolveListMode(rss_flow::Mode::DownloadAndMount, false, true,
			&flow, 31, 33, &second, second.slot, second.userId,
			t0 + std::chrono::seconds(5)) == rss_flow::Mode::Disabled);
		CHECK(!rss_flow::CanUseFlowReplyMount(flow, 31, 33, &second,
			second.slot, second.userId, t0 + std::chrono::seconds(5)));
		CHECK(rss_flow::AppliedModeForDeliveredList(rss_flow::Mode::MountOnly,
			{"100", "200"}, {"map", "100", "200"}) == rss_flow::Mode::MountOnly);
		CHECK(rss_flow::AppliedModeForDeliveredList(rss_flow::Mode::DownloadAndMount,
			{"100", "200"}, {"map", "100"}) == rss_flow::Mode::Disabled);
	}
	{
		// QueueNextStage itself owns the action boundary and cannot grant a new
		// leave window at the exact previous deadline.
		rss_flow::Flow flow;
		flow.providerEpoch = 41;
		flow.flowId = 42;
		flow.addonGeneration = 43;
		flow.mode = rss_flow::Mode::DownloadAndMount;
		flow.phase = rss_flow::Phase::Staging;
		flow.session = second;
		flow.addons = {"100", "200"};
		flow.completed = 1;
		flow.hardDeadline = t0 + std::chrono::seconds(60);
		flow.actionDeadline = t0 + std::chrono::seconds(15);
		CHECK(!rss_flow::QueueNextStage(flow, second, flow.actionDeadline,
			std::chrono::seconds(15)));
		CHECK(flow.phase == rss_flow::Phase::Staging && flow.currentAddon.empty());
	}
	CHECK(rss_flow::CanRunLegacyTimeout(false));
	CHECK(!rss_flow::CanRunLegacyTimeout(true));

	std::printf("pass=%d fail=%d\n", s_pass, s_fail);
	return s_fail ? 1 : 0;
}
