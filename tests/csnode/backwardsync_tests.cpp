#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/backwardsync.hpp>
#include <csnode/odatastream.hpp>

// cs::BackwardSync on a fake chain of signed blocks 0..500, of which 401..500 are stored, and fake
// peers: what it asks for, from whom, how it stores the replies and when it refuses them.
namespace {

constexpr cs::Sequence kHead = 500;
constexpr cs::Sequence kFirst = 401;

struct Keys {
    std::vector<cs::PublicKey> pub;
    std::vector<cs::PrivateKey> priv;
};

Keys makeKeys(size_t n) {
    EXPECT_TRUE(cscrypto::cryptoInit());
    Keys k;
    for (size_t i = 0; i < n; ++i) {
        cs::PublicKey p;
        k.priv.push_back(cscrypto::generateKeyPair(p));
        k.pub.push_back(p);
    }
    return k;
}

cs::Hash toHash(const csdb::PoolHash& h) {
    cs::Hash out{};
    const auto b = h.to_binary();
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

// block seq after prev, confidants `keys`, all of them signing, confirmed by `confirmers` (the
// confidants of the block before), parsed back from its bytes as a peer's reply would be
csdb::Pool makeBlock(cs::Sequence seq, const csdb::PoolHash& prev, const Keys& keys, const Keys& confirmers,
                     const std::string& payload = "payload", const Keys* signers = nullptr) {
    csdb::Pool p(prev, seq);
    p.add_user_field(7, payload + " " + std::to_string(seq));
    if (seq > 0) {
        p.set_confidants(keys.pub);
        p.add_number_trusted(static_cast<uint8_t>(keys.pub.size()));
        p.add_real_trusted((1ull << keys.pub.size()) - 1);
    }
    if (seq > 1) {
        cs::Bytes trustedToHash;
        cs::ODataStream stream(trustedToHash);
        stream << seq;
        stream << keys.pub;
        const cs::Hash h = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());
        std::vector<cs::Signature> confirmations;
        for (const auto& k : confirmers.priv) {
            confirmations.push_back(cscrypto::generateSignature(k, h.data(), h.size()));
        }
        p.add_number_confirmations(static_cast<uint8_t>(confirmers.pub.size()));
        p.add_confirmation_mask((1ull << confirmers.pub.size()) - 1);
        p.add_round_confirmations(confirmations);
    }
    if (seq > 0) {
        auto probe = p.clone();
        EXPECT_TRUE(probe.compose());
        const cs::Hash hash = toHash(probe.hash());
        std::vector<cs::Signature> sigs;
        for (const auto& k : (signers ? signers : &keys)->priv) {
            sigs.push_back(cscrypto::generateSignature(k, hash.data(), hash.size()));
        }
        p.set_signatures(sigs);
    }
    EXPECT_TRUE(p.compose());
    return csdb::Pool::from_binary(p.to_binary());
}

struct Chain {
    Keys keys = makeKeys(3);
    Keys others = makeKeys(3);
    std::vector<csdb::Pool> blocks;
    // block 350's round confirmations come from other keys than block 349's confidants
    Chain() {
        csdb::PoolHash prev;
        for (cs::Sequence s = 0; s <= kHead; ++s) {
            blocks.push_back(makeBlock(s, prev, keys, s == 350 ? others : keys));
            prev = blocks.back().hash();
        }
    }
    // the same block with a signature by another key: same hash, signatures that do not verify
    csdb::Pool badSignature(cs::Sequence s) const {
        return makeBlock(s, blocks[s].previous_hash(), keys, s == 350 ? others : keys, "payload", &others);
    }
    // another block of that sequence: another hash
    csdb::Pool otherContent(cs::Sequence s) const {
        return makeBlock(s, blocks[s].previous_hash(), keys, keys, "altered");
    }
};

const Chain& chain() {
    static const Chain c;
    return c;
}

// the tests store every buffered block in one call unless they say otherwise
cs::BackwardSync::Settings settingsOf(size_t batch = 100, size_t rate = 1000, size_t perTurn = 1000) {
    cs::BackwardSync::Settings s;
    s.batch = batch;
    s.rate = rate;
    s.storePerTurn = perTurn;
    s.turnBudgetMs = 0;   // no time limit unless a test sets one
    return s;
}

cs::PublicKey peerKey(uint8_t n) {
    cs::PublicKey k{};
    k.fill(n);
    return k;
}

class BackfillTest : public ::testing::Test {
protected:
    void SetUp() override {
        first_ = kFirst;
        for (uint8_t i = 1; i <= 6; ++i) {
            peers_.push_back(peerKey(i));
        }
        make();
    }

    void make(cs::BackwardSync::Settings settings = settingsOf()) {
        cs::BackwardSync::Hooks hooks;
        hooks.anchor = [this] {
            return cs::BackwardSync::Anchor{first_, first_ == 0 ? csdb::PoolHash{} : chain().blocks[first_].previous_hash()};
        };
        hooks.store = [this](const csdb::Pool& pool) {
            if (failStore_) {
                return false;
            }
            now_ += storeDelayMs_;
            EXPECT_EQ(pool.sequence() + 1, first_) << "not strictly downwards";
            EXPECT_EQ(pool.hash(), chain().blocks[pool.sequence()].hash());
            first_ = pool.sequence();
            ++storedCount_;
            return true;
        };
        hooks.flush = [this] { ++flushes_; return true; };
        hooks.floor = [this] { return floor_; };
        hooks.sendRequest = [this](const cs::PublicKey& to, const cs::PoolsRequestedSequences& seqs) {
            requests_.emplace_back(to, seqs);
        };
        hooks.clock = [this] { return now_; };
        hooks.onLog = [this](const std::string& line) { logs_.push_back(line); };
        sync_ = std::make_unique<cs::BackwardSync>(std::move(hooks), settings);
    }

    cs::BackwardSync::Context context() const {
        cs::BackwardSync::Context ctx;
        for (const auto& p : peers_) {
            ctx.peers.push_back({p, kHead});
        }
        ctx.round = kHead + 1;
        ctx.lastSeq = kHead;
        ctx.forwardSyncRunning = forwardSync_;
        ctx.bootstrapping = bootstrapping_;
        return ctx;
    }
    void tick(uint64_t advanceMs = 500) {
        now_ += advanceMs;
        sync_->tick(context());
    }
    cs::PoolsBlock blocksFor(const cs::PoolsRequestedSequences& seqs) const {
        cs::PoolsBlock out;
        for (auto s : seqs) {
            out.push_back(chain().blocks[s]);
        }
        return out;
    }
    // every request so far answered in full, in the order asked
    void answerAll() {
        auto pending = std::move(requests_);
        requests_.clear();
        for (auto& [peer, seqs] : pending) {
            sync_->onBlocks(peer, blocksFor(seqs));
        }
    }
    size_t logged(const std::string& text) const {
        return static_cast<size_t>(std::count_if(logs_.begin(), logs_.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; }));
    }
    // ticks and answers until the backfill is done or `limit` simulated seconds pass
    void runToEnd(uint64_t limitMs = 600'000) {
        for (uint64_t t = 0; t < limitMs && !sync_->stats().done; t += 500) {
            tick();
            answerAll();
        }
    }

    std::vector<cs::PublicKey> peers_;
    std::unique_ptr<cs::BackwardSync> sync_;
    std::vector<std::pair<cs::PublicKey, cs::PoolsRequestedSequences>> requests_;
    std::vector<std::string> logs_;
    cs::Sequence first_ = kFirst;
    cs::Sequence floor_ = 0;
    uint64_t now_ = 1'000'000;
    size_t storedCount_ = 0;
    size_t flushes_ = 0;
    bool failStore_ = false;
    uint64_t storeDelayMs_ = 0;   // a slow disk: simulated time spent in every store
    bool forwardSync_ = false;
    bool bootstrapping_ = false;
};

}  // namespace

// (1) four batches, four peers, ascending inside, consecutive downwards from the first stored block
TEST_F(BackfillTest, FourBatchesGoToFourPeers) {
    tick();
    ASSERT_EQ(requests_.size(), 4u);
    std::set<cs::PublicKey> asked;
    cs::Sequence expectedHi = kFirst - 1;
    for (const auto& [peer, seqs] : requests_) {
        asked.insert(peer);
        ASSERT_EQ(seqs.size(), 100u);
        EXPECT_TRUE(std::is_sorted(seqs.begin(), seqs.end()));
        EXPECT_EQ(seqs.back(), expectedHi);
        EXPECT_EQ(seqs.front(), expectedHi - 99);
        expectedHi -= 100;
        EXPECT_TRUE(sync_->hasRequestTo(peer));
    }
    EXPECT_EQ(asked.size(), 4u);
    EXPECT_EQ(logged("BACKFILL: full node with blocks 401..500: fetching 0..400"), 1u);
    tick();
    EXPECT_EQ(requests_.size(), 4u) << "at most four batches in flight";
}

// (2) replies out of order are stored strictly downwards, down to block 0 (the genesis block
// without signatures), and the end is logged once
TEST_F(BackfillTest, OutOfOrderRepliesAreStoredDownwardsToBlockZero) {
    tick();
    ASSERT_EQ(requests_.size(), 4u);
    sync_->onBlocks(requests_[1].first, blocksFor(requests_[1].second));
    EXPECT_EQ(first_, kFirst) << "batch 2 waits for batch 1";
    EXPECT_EQ(sync_->stats().buffered, 100u);
    sync_->onBlocks(requests_[0].first, blocksFor(requests_[0].second));
    EXPECT_EQ(first_, 201u);
    requests_.clear();
    runToEnd();
    EXPECT_EQ(first_, 0u);
    EXPECT_TRUE(sync_->stats().done);
    EXPECT_FALSE(sync_->isActive());
    EXPECT_EQ(storedCount_, kFirst);
    EXPECT_EQ(logged("BACKFILL: full history stored"), 1u);
    EXPECT_GE(flushes_, 1u) << "flushed at the end";
    // block 350 carries confirmations by other keys: (10) counted, stored all the same
    EXPECT_EQ(sync_->stats().confirmationErrors, 1u);
    EXPECT_EQ(logged("the round confirmations of block 350"), 1u);
    tick();
    EXPECT_TRUE(requests_.empty());
}

// (3) an altered signature, an altered block: refused, the peer marked, the blocks asked for again
// from another peer
TEST_F(BackfillTest, BlocksThatDoNotVerifyAreRefused) {
    tick();
    auto [badPeer, seqs] = requests_[0];
    auto blocks = blocksFor(seqs);
    blocks[seqs.size() - 1] = chain().badSignature(seqs.back());   // block 400
    requests_.clear();
    sync_->onBlocks(badPeer, std::move(blocks));
    EXPECT_EQ(first_, kFirst);
    EXPECT_EQ(sync_->stats().rejected, 1u);
    EXPECT_EQ(logged("block 400 from peer"), 1u);
    EXPECT_EQ(logged("rejected (signatures"), 1u);
    EXPECT_GE(logged("marked for 1 h"), 1u);

    tick();
    ASSERT_FALSE(requests_.empty());
    EXPECT_NE(requests_[0].first, badPeer);
    EXPECT_EQ(requests_[0].second.back(), 400u) << "the refused batch first";
    auto again = blocksFor(requests_[0].second);
    again.back() = chain().otherContent(400);
    const auto secondBad = requests_[0].first;
    requests_.clear();
    sync_->onBlocks(secondBad, std::move(again));
    EXPECT_EQ(logged("rejected (hash)"), 1u);

    runToEnd();
    EXPECT_EQ(first_, 0u);
    for (const auto& [peer, s] : requests_) {
        EXPECT_NE(peer, badPeer);
    }
}

// blocks nobody asked for, and blocks outside the batch, are dropped
TEST_F(BackfillTest, UnaskedBlocksAreDropped) {
    tick();
    const auto stranger = peerKey(99);
    sync_->onBlocks(stranger, blocksFor({400, 399, 398}));
    EXPECT_EQ(sync_->stats().dropped, 3u);
    EXPECT_EQ(sync_->stats().buffered, 0u);
    const auto [peer, seqs] = requests_[0];
    sync_->onBlocks(peer, blocksFor({250, 251}));   // another batch's blocks
    EXPECT_EQ(sync_->stats().dropped, 5u);
    EXPECT_EQ(first_, kFirst);
    EXPECT_TRUE(sync_->hasRequestTo(peer));
}

// (4) a peer that never answers: after 30 s its batch goes to another peer, and it waits 60 s
TEST_F(BackfillTest, ASilentPeerIsReplaced) {
    tick();
    const auto silent = requests_[0].first;
    const auto seqs = requests_[0].second;
    for (size_t i = 1; i < requests_.size(); ++i) {
        sync_->onBlocks(requests_[i].first, blocksFor(requests_[i].second));
    }
    requests_.clear();
    tick(29'000);
    for (const auto& r : requests_) {
        EXPECT_NE(r.second.back(), seqs.back()) << "asked again before 30 s";
    }
    requests_.clear();
    tick(1'500);
    ASSERT_FALSE(requests_.empty());
    EXPECT_EQ(requests_[0].second, seqs);
    EXPECT_NE(requests_[0].first, silent);
    EXPECT_FALSE(sync_->hasRequestTo(silent));
    answerAll();
    // the silent peer gets nothing for 60 s
    for (int i = 0; i < 100; ++i) {
        tick();
        for (const auto& r : requests_) {
            EXPECT_NE(r.first, silent);
        }
        answerAll();
    }
}

// (5) AwaitAnswer: that peer gets one batch every 6 s, the others every 200 ms; DuplicatedRequest
// delays it 10 s; NoSuchBlocks after AwaitAnswer leaves it out for good
TEST_F(BackfillTest, OrderThirtyFivePeersArePaced) {
    peers_.resize(2);
    make(settingsOf(10, 0));
    tick();
    ASSERT_EQ(requests_.size(), 2u);
    const auto paced = requests_[0].first;
    const auto free = requests_[1].first;
    sync_->onSyncroMessage(paced, cs::SyncroMessage::AwaitAnswer);
    EXPECT_EQ(logged("answers with AwaitAnswer"), 1u);
    answerAll();
    std::map<cs::PublicKey, std::vector<uint64_t>> askedAt;
    for (int i = 0; i < 24; ++i) {   // 12 s
        tick();
        for (const auto& r : requests_) {
            askedAt[r.first].push_back(now_);
            if (r.first == paced) {
                // its anti-duplicate log is active: every request gets AwaitAnswer first
                sync_->onSyncroMessage(paced, cs::SyncroMessage::AwaitAnswer);
            }
        }
        answerAll();
    }
    ASSERT_GE(askedAt[paced].size(), 1u);
    EXPECT_LE(askedAt[paced].size(), 2u);
    EXPECT_GE(askedAt[free].size(), 10u);

    // DuplicatedRequest: its batch goes elsewhere, nothing to it for 10 s
    tick();
    sync_->onSyncroMessage(free, cs::SyncroMessage::AwaitAnswer);
    ASSERT_TRUE(sync_->hasRequestTo(free));
    {
        sync_->onSyncroMessage(free, cs::SyncroMessage::DuplicatedRequest);
        EXPECT_FALSE(sync_->hasRequestTo(free));
        EXPECT_EQ(logged("refused a request as a duplicate"), 1u);
        requests_.clear();
        for (int i = 0; i < 18; ++i) {   // 9 s
            tick();
            for (const auto& r : requests_) {
                EXPECT_NE(r.first, free);
            }
            answerAll();
        }
    }

    // NoSuchBlocks after AwaitAnswer: never asked again
    requests_.clear();
    for (int i = 0; i < 40; ++i) {
        tick();
        if (sync_->hasRequestTo(paced)) {
            break;
        }
        answerAll();
    }
    ASSERT_TRUE(sync_->hasRequestTo(paced));
    {
        sync_->onSyncroMessage(paced, cs::SyncroMessage::AwaitAnswer);
        sync_->onSyncroMessage(paced, cs::SyncroMessage::NoSuchBlocks);
        EXPECT_EQ(logged("keeps our request in its log: not used again"), 1u);
        requests_.clear();
        for (int i = 0; i < 20; ++i) {
            tick(3'600'000);
            for (const auto& r : requests_) {
                EXPECT_NE(r.first, paced);
            }
            answerAll();
        }
    }
}

// (6) the buffer is bounded: a reply is taken only up to eight batches waiting to be stored
TEST_F(BackfillTest, TheBufferIsBounded) {
    make(settingsOf(50, 0));   // batches of 50, no rate limit: at most 400 blocks wait
    tick();
    ASSERT_EQ(requests_.size(), 4u);
    // the batch the storage waits for never comes; the others do, and more are asked for
    const auto held = requests_[0];
    requests_.erase(requests_.begin());
    for (int i = 0; i < 40; ++i) {
        answerAll();
        tick();
        requests_.erase(std::remove_if(requests_.begin(), requests_.end(),
                                       [&](const auto& r) { return r.second == held.second; }), requests_.end());
    }
    EXPECT_GT(sync_->stats().buffered, 100u);
    EXPECT_LE(sync_->stats().buffered, 400u);
    EXPECT_EQ(first_, kFirst);
}

// (7) forward sync, a snapshot fetch: no new request, replies still stored
TEST_F(BackfillTest, PausedWhileTheNodeIsNotSynced) {
    tick();
    const auto pending = requests_;
    requests_.clear();
    forwardSync_ = true;
    tick();
    EXPECT_TRUE(requests_.empty());
    EXPECT_EQ(logged("BACKFILL: paused (the node syncs forward)"), 1u);
    for (const auto& [peer, seqs] : pending) {
        sync_->onBlocks(peer, blocksFor(seqs));
    }
    EXPECT_EQ(first_, 1u) << "the replies are stored while paused";
    tick();
    EXPECT_TRUE(requests_.empty());
    forwardSync_ = false;
    tick();
    EXPECT_EQ(logged("BACKFILL: resumed"), 1u);
    EXPECT_FALSE(requests_.empty());

    bootstrapping_ = true;
    make();
    requests_.clear();
    tick();
    EXPECT_TRUE(requests_.empty());
}

// (8) a new BackwardSync on the same chain (a restart) goes on below the first stored block
TEST_F(BackfillTest, ARestartResumesBelowTheFirstStoredBlock) {
    tick();
    sync_->onBlocks(requests_[0].first, blocksFor(requests_[0].second));
    ASSERT_EQ(first_, 301u);
    make();
    requests_.clear();
    tick();
    ASSERT_FALSE(requests_.empty());
    EXPECT_EQ(requests_[0].second.back(), 300u);
    EXPECT_EQ(logged("with blocks 301..500: fetching 0..300"), 1u);
}

// (9) backfill_rate: no more blocks asked for than the rate allows, beyond the burst of 4 batches
TEST_F(BackfillTest, TheRateIsRespected) {
    make(settingsOf(5, 20));
    size_t asked = 0;
    const uint64_t start = now_;
    for (int i = 0; i < 20; ++i) {   // 10 s
        tick();
        for (const auto& r : requests_) {
            asked += r.second.size();
        }
        answerAll();
    }
    const double seconds = static_cast<double>(now_ - start) / 1000.0;
    EXPECT_LE(asked, static_cast<size_t>(4 * 5 + 20 * seconds));
    EXPECT_GE(asked, static_cast<size_t>(20 * (seconds - 1)));
}

// a pruned node stops at its floor
TEST_F(BackfillTest, APrunedNodeStopsAtItsFloor) {
    floor_ = 250;
    runToEnd();
    EXPECT_EQ(first_, 250u);
    EXPECT_EQ(logged("BACKFILL: stored down to the floor 250 (pruned node)"), 1u);
    EXPECT_EQ(logged("pruned node with blocks 401..500: fetching 250..400"), 1u);
    for (const auto& [peer, seqs] : requests_) {
        EXPECT_GE(seqs.front(), 250u);
    }
}

// a node whose storage starts at its floor, or at block 0, does nothing
TEST_F(BackfillTest, NothingToFetch) {
    floor_ = kFirst;
    tick();
    EXPECT_TRUE(requests_.empty());
    EXPECT_FALSE(sync_->stats().started);
    first_ = 0;
    floor_ = 0;
    make();
    tick();
    EXPECT_TRUE(requests_.empty());
    EXPECT_FALSE(sync_->isActive());
}

// a storage that refuses the block (a full disk): the block is kept and stored once it accepts it
TEST_F(BackfillTest, AStorageFailureIsRetried) {
    tick();
    failStore_ = true;
    sync_->onBlocks(requests_[0].first, blocksFor(requests_[0].second));
    EXPECT_EQ(first_, kFirst);
    EXPECT_EQ(logged("could not be stored: trying again in 60 s"), 1u);
    failStore_ = false;
    tick(30'000);
    EXPECT_EQ(first_, kFirst) << "retried before 60 s";
    tick(31'000);
    EXPECT_EQ(first_, 301u);
}

// stop() ends it: no request, nothing stored
TEST_F(BackfillTest, StopEndsIt) {
    sync_->stop();
    tick();
    EXPECT_TRUE(requests_.empty());
    EXPECT_FALSE(sync_->isActive());
}

// at most storePerTurn blocks per call: the processor thread also runs consensus; the rest is
// stored on the next ticks
TEST_F(BackfillTest, TheWorkPerTurnIsBounded) {
    make(settingsOf(100, 0, 30));
    tick();
    sync_->onBlocks(requests_[0].first, blocksFor(requests_[0].second));
    EXPECT_EQ(first_, kFirst - 30);
    tick();
    EXPECT_EQ(first_, kFirst - 60);
    tick();
    tick();
    EXPECT_EQ(first_, kFirst - 100);
    EXPECT_GT(sync_->stats().busyUs, 0u);
    EXPECT_GT(sync_->stats().longestTurnUs, 0u);
}

// a slow store ends the turn once it has used its time, however many blocks it may still store;
// such a turn is logged with where its time went
TEST_F(BackfillTest, ASlowTurnStopsAtItsTimeBudget) {
    auto settings = settingsOf(100, 0, 50);
    settings.turnBudgetMs = 30;
    make(settings);
    storeDelayMs_ = 12;
    tick();
    sync_->onBlocks(requests_[0].first, blocksFor(requests_[0].second));
    // 12 ms a block: 36 ms after the third block, past the budget of 30 ms
    EXPECT_EQ(kFirst - first_, 3u);
    storeDelayMs_ = 0;
    tick();
    EXPECT_EQ(kFirst - first_, 53u);   // a fast turn stores up to storePerTurn
    EXPECT_EQ(sync_->stats().slowTurns, 0u);

    // a turn over kSlowTurnMs is counted and logged with where its time went
    storeDelayMs_ = 210;
    tick();
    EXPECT_EQ(kFirst - first_, 54u);   // one block, then the budget is spent
    EXPECT_EQ(sync_->stats().slowTurns, 1u);
    EXPECT_EQ(logged("BACKFILL: slow turn of 210 ms (1 so far): 1 blocks"), 1u);
}

// L7: pruning raised the floor above blocks waiting in the buffer: the backfill ends, nothing is
// stored below the floor
TEST_F(BackfillTest, AFloorRaisedByPruningEndsIt) {
    floor_ = 100;
    tick();
    const auto pending = requests_;
    floor_ = 420;   // pruning removed blocks up to 419: the bottom of the window is above the next block
    sync_->onBlocks(pending[0].first, blocksFor(pending[0].second));
    EXPECT_TRUE(sync_->stats().done);
    EXPECT_EQ(first_, kFirst);
    EXPECT_EQ(logged("could not be stored"), 0u);
}

// L5: a peer that once answered AwaitAnswer and then serves a batch without it is no longer paced
TEST_F(BackfillTest, PacingEndsWhenAPeerServesWithoutAwaitAnswer) {
    peers_.resize(1);
    make(settingsOf(10, 0));
    tick();
    const auto peer = requests_[0].first;
    sync_->onSyncroMessage(peer, cs::SyncroMessage::AwaitAnswer);
    answerAll();
    tick();
    EXPECT_TRUE(requests_.empty()) << "paced: 6 s";
    tick(6'000);
    ASSERT_EQ(requests_.size(), 1u);
    answerAll();   // no AwaitAnswer this time
    tick();
    EXPECT_EQ(requests_.size(), 1u) << "not paced any more";
}

// L4: messages for requests that are not the backfill's leave no trace
TEST_F(BackfillTest, OtherSyncroMessagesAreIgnored) {
    const auto stranger = peerKey(99);
    sync_->onSyncroMessage(stranger, cs::SyncroMessage::NoSuchBlocks);   // not started
    tick();
    sync_->onSyncroMessage(stranger, cs::SyncroMessage::NoSuchBlocks);
    sync_->onSyncroMessage(stranger, cs::SyncroMessage::DuplicatedRequest);
    EXPECT_EQ(logged("BACKFILL: peer"), 0u);
}
