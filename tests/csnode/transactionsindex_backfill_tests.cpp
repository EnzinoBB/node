#include "gtest/gtest.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <csdb/address.hpp>
#include <csdb/amount.hpp>
#include <csdb/amount_commission.hpp>
#include <csdb/currency.hpp>
#include <csdb/pool.hpp>
#include <csdb/transaction.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/transactionsindex.hpp>

// The transaction index of backward sync (the design's section 6): blocks indexed downwards, below
// the blocks a pruned lineage or a rebuild left indexed, while readers walk the histories.
namespace fs = std::filesystem;

namespace {

constexpr cs::Sequence kNone = cs::kWrongSequence;

csdb::Address wallet(uint8_t n) {
    cs::PublicKey key{};
    key.fill(n);
    return csdb::Address::from_public_key(key);
}

const csdb::Address A = wallet(0xA1);
const csdb::Address B = wallet(0xB2);
const csdb::Address C = wallet(0xC3);

class TrxIndexBackfill : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("cs_trxindex_backfill_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        index_ = std::make_unique<cs::TransactionsIndex>(chain_, dir_.string());
        index_->setChainView({[this](const csdb::Address& a) {
                                  auto it = last_.find(a);
                                  return it == last_.end() ? kNone : it->second;
                              },
                              [this] { return first_.load(); }});
    }
    void TearDown() override {
        index_->close();
        index_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    // block seq with one transaction from each wallet to itself
    void put(cs::Sequence seq, const std::vector<csdb::Address>& wallets) {
        csdb::Pool pool(csdb::PoolHash{}, seq);
        int64_t id = 1;
        for (const auto& w : wallets) {
            csdb::Transaction t(id++, w, w, csdb::Currency{}, csdb::Amount(1), csdb::AmountCommission(0.0),
                                csdb::AmountCommission(0.0), cs::Signature{});
            EXPECT_TRUE(pool.add_transaction(t));
            cs::Sequence& last = last_[w];
            last = std::max(last, seq);
        }
        blocks_[seq] = pool;
    }
    // what the storage answers: the blocks put, empty blocks elsewhere
    csdb::Pool load(cs::Sequence seq) const {
        auto it = blocks_.find(seq);
        return it == blocks_.end() ? csdb::Pool(csdb::PoolHash{}, seq) : it->second;
    }
    std::function<csdb::Pool(cs::Sequence)> loader() const {
        return [this](cs::Sequence seq) { return load(seq); };
    }
    // the index a pruned node leaves: built forward over from..to, then the blocks below `first`
    // pruned (their own entries removed, the pointers to them kept)
    void prunedLineage(cs::Sequence from, cs::Sequence to, cs::Sequence first) {
        ASSERT_TRUE(index_->rebuildFrom(from, to, loader(), nullptr));
        for (cs::Sequence s = from; s < first; ++s) {
            index_->onPruneBlock(load(s));
        }
        first_ = first;
    }
    // the backfill stores blocks from..to downwards: each one is indexed, then stored
    void backfill(cs::Sequence from, cs::Sequence to) {
        for (cs::Sequence s = from + 1; s-- > to;) {
            ASSERT_EQ(first_.load(), s + 1);
            index_->indexBackfilledBlock(load(s));
            first_ = s;
        }
    }
    // a history walk as the API does it, from the wallet's last transaction
    std::vector<cs::Sequence> history(const csdb::Address& w) const {
        std::vector<cs::Sequence> out;
        auto it = last_.find(w);
        if (it == last_.end()) {
            return out;
        }
        for (cs::Sequence cur = it->second; cur != kNone && out.size() < 100'000;) {
            out.push_back(cur);
            const cs::Sequence prev = index_->getPrevTransBlock(w, cur);
            EXPECT_NE(prev, cur) << "an entry points to itself";
            if (prev != kNone && prev >= cur) {
                break;
            }
            cur = prev;
        }
        return out;
    }
    cs::Sequence prev(const csdb::Address& w, cs::Sequence seq) const {
        return index_->getPrevTransBlock(w, seq);
    }

    const csdb::Address genesis_ = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start_ = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain_{genesis_, start_};
    fs::path dir_;
    std::unique_ptr<cs::TransactionsIndex> index_;
    std::map<cs::Sequence, csdb::Pool> blocks_;
    std::map<csdb::Address, cs::Sequence> last_;
    std::atomic<cs::Sequence> first_{0};
};

}  // namespace

// 1. A pruned lineage: entries of A at 200 and 250 remain, 200 names the pruned block 150.
TEST_F(TrxIndexBackfill, PrunedLineageGrowsDownwards) {
    for (cs::Sequence s : {100u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    prunedLineage(100, 250, 180);
    EXPECT_EQ(prev(A, 200), 150u);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150}));
    EXPECT_EQ(index_->backfillFloor(), kNone);

    backfill(179, 150);
    EXPECT_EQ(prev(A, 200), 150u) << "a pointer above the tail changed";
    EXPECT_EQ(prev(A, 150), kNone);
    EXPECT_EQ(index_->backfillFloor(), 150u);

    backfill(149, 100);
    EXPECT_EQ(prev(A, 150), 100u);
    EXPECT_EQ(prev(A, 100), kNone);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150, 100}));
    EXPECT_EQ(index_->backfillFloor(), 100u);
}

// 2. An index rebuilt from the stored blocks: A's oldest stored entry names no block.
TEST_F(TrxIndexBackfill, RebuiltIndexGrowsDownwards) {
    for (cs::Sequence s : {100u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    ASSERT_TRUE(index_->rebuildFrom(180, 250, loader(), nullptr));
    first_ = 180;
    EXPECT_EQ(prev(A, 200), kNone);

    backfill(179, 150);
    EXPECT_EQ(prev(A, 200), 150u);
    EXPECT_EQ(prev(A, 150), kNone);
    backfill(149, 100);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150, 100}));
}

// 3. A wallet whose transactions all lie below the stored blocks has no entry: its history starts
// with its last transaction once that block is stored.
TEST_F(TrxIndexBackfill, AWalletFirstSeenLate) {
    put(120, {B});
    put(110, {B});
    put(200, {A});
    prunedLineage(100, 200, 180);
    EXPECT_EQ(prev(B, 120), kNone);

    backfill(179, 120);
    EXPECT_EQ(prev(B, 120), kNone);
    EXPECT_EQ(history(B), (std::vector<cs::Sequence>{120}));
    backfill(119, 100);
    EXPECT_EQ(history(B), (std::vector<cs::Sequence>{120, 110}));
}

// 4. A block indexed again (a crash between the index and the storage) changes nothing.
TEST_F(TrxIndexBackfill, IndexingAgainIsIdempotent) {
    for (cs::Sequence s : {100u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    prunedLineage(100, 250, 180);
    backfill(179, 150);
    index_->indexBackfilledBlock(load(150));
    EXPECT_EQ(prev(A, 200), 150u);
    EXPECT_EQ(prev(A, 150), kNone);

    backfill(149, 100);
    index_->indexBackfilledBlock(load(150));
    index_->indexBackfilledBlock(load(100));
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150, 100}));
    for (cs::Sequence s : {100u, 150u, 200u, 250u}) {
        EXPECT_NE(prev(A, s), s);
    }
}

// 5. A tail record whose entry is gone (pruning removed it): the tail is found by walking.
TEST_F(TrxIndexBackfill, AStaleTailRecordIsWalkedPast) {
    for (cs::Sequence s : {130u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    prunedLineage(100, 250, 180);
    backfill(179, 150);                     // the tail record of A names 150
    index_->onPruneBlock(load(150));        // ... whose entry goes
    first_ = 151;
    EXPECT_EQ(prev(A, 200), 150u);

    backfill(150, 130);
    EXPECT_EQ(prev(A, 200), 150u);          // block 150 again: the walk found 200 as the tail
    EXPECT_EQ(prev(A, 150), 130u);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150, 130}));
}

// 6. The floor above the first stored block at start: the blocks in between are indexed again.
TEST_F(TrxIndexBackfill, RepairOfTheFloorIndexesTheBlocksLeftOut) {
    for (cs::Sequence s : {125u, 140u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    for (cs::Sequence s : {130u, 145u, 260u}) {
        put(s, {C});
    }
    prunedLineage(100, 260, 180);
    backfill(179, 150);
    EXPECT_EQ(index_->repairBackfillFloor(150, loader()), 0u);
    EXPECT_EQ(index_->repairBackfillFloor(151, loader()), 0u);

    first_ = 120;                           // stored down to 120, indexed down to 150
    EXPECT_EQ(index_->repairBackfillFloor(120, loader()), 30u);
    EXPECT_EQ(index_->backfillFloor(), 120u);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{250, 200, 150, 140, 125}));
    EXPECT_EQ(history(C), (std::vector<cs::Sequence>{260, 145, 130}));
}

// 7. Readers walk the histories while 10,000 blocks are indexed below them: no entry ever points to
// itself or up, and a history never gets shorter.
TEST_F(TrxIndexBackfill, ReadersSeeHistoriesGrow) {
    constexpr cs::Sequence kTop = 10'000;
    for (cs::Sequence s = 1; s <= kTop + 1; ++s) {
        put(s, s % 3 == 0 ? std::vector<csdb::Address>{A, C} : std::vector<csdb::Address>{A});
    }
    ASSERT_TRUE(index_->rebuildFrom(kTop + 1, kTop + 1, loader(), nullptr));
    first_ = kTop + 1;

    std::atomic<bool> done{false};
    std::atomic<size_t> walks{0};
    std::atomic<size_t> faults{0};
    std::thread reader([&] {
        size_t longest = 0;
        while (!done) {
            size_t length = 0;
            for (cs::Sequence cur = kTop + 1; cur != kNone; ++length) {
                const cs::Sequence p = index_->getPrevTransBlock(A, cur);
                if (p != kNone && p >= cur) {
                    ++faults;
                    break;
                }
                cur = p;
            }
            if (length < longest) {
                ++faults;
            }
            longest = std::max(longest, length);
            ++walks;
        }
    });
    backfill(kTop, 1);
    done = true;
    reader.join();

    EXPECT_EQ(faults.load(), 0u);
    EXPECT_GT(walks.load(), 0u);
    EXPECT_EQ(history(A).size(), static_cast<size_t>(kTop + 1));
    EXPECT_EQ(history(C).size(), static_cast<size_t>(kTop / 3));
}

// 8. The tail records and the floor are not entries: --set-bc-top (trimToFloor) leaves them, and
// the backfill goes on from them.
TEST_F(TrxIndexBackfill, TrimToFloorKeepsTailsAndFloor) {
    for (cs::Sequence s : {99u, 100u, 150u, 200u, 250u}) {
        put(s, {A});
    }
    prunedLineage(100, 250, 180);
    backfill(179, 100);
    index_->trimToFloor(200);
    EXPECT_EQ(prev(A, 250), kNone) << "entries above the floor go";
    EXPECT_EQ(prev(A, 200), 150u);
    EXPECT_EQ(index_->backfillFloor(), 100u);

    last_[A] = 200;
    backfill(99, 99);
    EXPECT_EQ(prev(A, 100), 99u);
    EXPECT_EQ(history(A), (std::vector<cs::Sequence>{200, 150, 100, 99}));
}
