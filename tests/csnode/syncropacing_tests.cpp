#include <gtest/gtest.h>

#include <csnode/syncropacing.hpp>

// Forward sync against responders with an active anti-duplicate log (special order 35), known only
// from the SyncroMsg they send.
namespace {
cs::PublicKey key(uint8_t n) {
    cs::PublicKey k{};
    k.fill(n);
    return k;
}
}  // namespace

// a peer that never sent a SyncroMsg is asked as before
TEST(SyncroPacing, APeerWithoutSyncroMessagesIsNotPaced) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onRequest(p, 400, 1000);
    pacing.onReply(p, 1100);
    EXPECT_FALSE(pacing.active(p));
    EXPECT_FALSE(pacing.anyActive());
    EXPECT_TRUE(pacing.allows(p, 100, 1200));
}

// after a reply ending at 400, a request ending at or below 400 waits until the responder's entry
// expired; a higher one goes at once
TEST(SyncroPacing, ALowerRequestWaitsForTheResponderEntry) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 900);
    pacing.onRequest(p, 400, 1000);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 1010);
    pacing.onReply(p, 1100);
    EXPECT_TRUE(pacing.active(p));
    EXPECT_TRUE(pacing.allows(p, 500, 1200));
    EXPECT_FALSE(pacing.allows(p, 100, 1200));
    EXPECT_FALSE(pacing.allows(p, 400, 1100 + cs::SyncroPacing::kSentLifeMs - 1));
    EXPECT_TRUE(pacing.allows(p, 100, 1100 + cs::SyncroPacing::kSentLifeMs));
    // the higher request becomes the entry
    pacing.onRequest(p, 500, 1200);
    pacing.onReply(p, 1300);
    EXPECT_FALSE(pacing.allows(p, 450, 2000));
    EXPECT_TRUE(pacing.allows(p, 450, 1300 + cs::SyncroPacing::kSentLifeMs));
}

// a lower request sent while the entry lives does not lower what the responder logged
TEST(SyncroPacing, ALowerRequestDoesNotReplaceTheEntry) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 0);
    pacing.onRequest(p, 400, 1000);
    pacing.onReply(p, 1100);
    pacing.onRequest(p, 100, 1200);   // sent anyway (e.g. by an earlier build): refused by the responder
    pacing.onSyncroMessage(p, cs::SyncroMessage::DuplicatedRequest, 1250);
    EXPECT_FALSE(pacing.allows(p, 400, 1300));
    EXPECT_TRUE(pacing.allows(p, 401, 1300));
    EXPECT_TRUE(pacing.allows(p, 100, 1250 + cs::SyncroPacing::kSentLifeMs));
}

// NoSuchBlocks after AwaitAnswer: the responder keeps the entry for ever, every request up to it is
// refused (here for an hour); higher ones are not
TEST(SyncroPacing, NoSuchBlocksAfterAwaitAnswerIsForEver) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::NoSuchBlocks, 0);   // a pruned peer, no request of ours: nothing
    pacing.onRequest(p, 300, 1000);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 1010);
    pacing.onSyncroMessage(p, cs::SyncroMessage::NoSuchBlocks, 1020);
    const uint64_t later = 1020 + cs::SyncroPacing::kRefusedForMs - 1;
    EXPECT_FALSE(pacing.allows(p, 300, later));
    EXPECT_FALSE(pacing.allows(p, 1, later));
    EXPECT_TRUE(pacing.allows(p, 301, later));
    // an hour later it is asked again (its log may have been cleared by a restart)
    EXPECT_TRUE(pacing.allows(p, 1, 1020 + cs::SyncroPacing::kRefusedForMs));
    // a plain NoSuchBlocks (refused before it logged anything) leaves no such mark
    const auto q = key(2);
    pacing.onSyncroMessage(q, cs::SyncroMessage::AwaitAnswer, 0);
    pacing.onRequest(q, 300, 1000);
    pacing.onSyncroMessage(q, cs::SyncroMessage::NoSuchBlocks, 1020);
    EXPECT_TRUE(pacing.allows(q, 100, 2000));
}

// peers are independent
TEST(SyncroPacing, PeersAreIndependent) {
    cs::SyncroPacing pacing;
    pacing.onSyncroMessage(key(1), cs::SyncroMessage::AwaitAnswer, 0);
    pacing.onRequest(key(1), 400, 1000);
    pacing.onReply(key(1), 1100);
    pacing.onRequest(key(2), 400, 1000);
    EXPECT_FALSE(pacing.allows(key(1), 100, 1200));
    EXPECT_TRUE(pacing.allows(key(2), 100, 1200));
    pacing.forget(key(1));
    EXPECT_TRUE(pacing.allows(key(1), 100, 1200));
}

// the first request goes out before the peer ever sent a SyncroMsg: its answer still counts
TEST(SyncroPacing, ARequestSentBeforeThePeersFirstSyncroMessage) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onRequest(p, 300, 1000);
    EXPECT_FALSE(pacing.active(p));
    EXPECT_TRUE(pacing.inFlight(p, 1001));
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 1010);
    pacing.onSyncroMessage(p, cs::SyncroMessage::NoSuchBlocks, 1020);
    EXPECT_TRUE(pacing.active(p));
    EXPECT_FALSE(pacing.inFlight(p, 1030));
    EXPECT_FALSE(pacing.allows(p, 200, 5000));
    EXPECT_TRUE(pacing.allows(p, 301, 5000));
}

// a DuplicatedRequest with no request of ours in flight marks the peer active, nothing else
TEST(SyncroPacing, ADuplicatedRequestWithNoRecordedRequest) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::DuplicatedRequest, 0);
    EXPECT_TRUE(pacing.active(p));
    EXPECT_TRUE(pacing.allows(p, 100, 1));
}

// answers to another request (the backfill's) while no forward request is in flight change nothing
TEST(SyncroPacing, AnswersToTheBackfillLeaveTheForwardStateAlone) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 0);
    pacing.onRequest(p, 400, 1000);
    pacing.onReply(p, 1100);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 2000);
    pacing.onSyncroMessage(p, cs::SyncroMessage::NoSuchBlocks, 2010);
    pacing.onSyncroMessage(p, cs::SyncroMessage::DuplicatedRequest, 2020);
    EXPECT_TRUE(pacing.allows(p, 100, 1100 + cs::SyncroPacing::kSentLifeMs));
    EXPECT_TRUE(pacing.allows(p, 500, 2100));
}

// a forward request without an answer for 30 s is no longer in flight: a later answer is not its
TEST(SyncroPacing, ARequestWithoutAnswerExpires) {
    cs::SyncroPacing pacing;
    const auto p = key(1);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 0);
    pacing.onRequest(p, 300, 1000);
    pacing.onSyncroMessage(p, cs::SyncroMessage::AwaitAnswer, 1010);
    EXPECT_TRUE(pacing.inFlight(p, 1000 + cs::SyncroPacing::kInFlightMs - 1));
    EXPECT_FALSE(pacing.inFlight(p, 1000 + cs::SyncroPacing::kInFlightMs));
    pacing.onSyncroMessage(p, cs::SyncroMessage::NoSuchBlocks, 1000 + cs::SyncroPacing::kInFlightMs);
    EXPECT_TRUE(pacing.allows(p, 100, 1000 + cs::SyncroPacing::kInFlightMs + cs::SyncroPacing::kSentLifeMs));
}
