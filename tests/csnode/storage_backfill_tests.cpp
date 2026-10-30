#include "gtest/gtest.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <csdb/database.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage.hpp>
#include <csdb/storage_backend.hpp>

#include "memorydb.hpp"

// Storage::pool_save_backfill (backward sync, step 1): a storage that starts after block 0, as a
// pruned node or a node from a snapshot has it, stores the blocks below its first one, on every
// backend, without touching the bookkeeping of its head.
namespace fs = std::filesystem;

namespace {

constexpr cs::Sequence kFirst = 50;
constexpr cs::Sequence kLast = 60;

// blocks 0..count-1 of one chain
std::vector<csdb::Pool> chain(size_t count) {
    std::vector<csdb::Pool> pools;
    csdb::PoolHash prev;
    for (size_t i = 0; i < count; ++i) {
        csdb::Pool p(prev, i);
        p.add_user_field(1, "block " + std::to_string(i));
        EXPECT_TRUE(p.compose());
        prev = p.hash();
        pools.push_back(p);
    }
    return pools;
}

void putRange(csdb::Database& db, const std::vector<csdb::Pool>& pools, cs::Sequence from, cs::Sequence to) {
    for (cs::Sequence s = from; s <= to; ++s) {
        ASSERT_TRUE(db.put(pools[s].hash().to_binary(), static_cast<uint32_t>(s), pools[s].to_binary()));
    }
}

fs::path freshDir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("cs_backfill_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// a storage on blocks 50..60 of `pools`, opened as a node opens a pruned one: a quick start that
// continues after block 60
bool openPruned(csdb::Storage& storage, const fs::path& dir, const std::string& backend) {
    return storage.open(dir.string(), nullptr, cs::kWrongSequence, kLast + 1, 5000, 100, 0, 0, backend);
}

class StorageBackfill : public ::testing::TestWithParam<std::string> {
protected:
    void SetUp() override {
        pools_ = chain(kLast + 2);
        dir_ = freshDir(GetParam());
        {
            auto db = csdb::backend::open(GetParam(), dir_.string());
            ASSERT_TRUE(db && db->is_open());
            putRange(*db, pools_, kFirst, kLast);
            db->flush();
        }
        ASSERT_TRUE(openPruned(storage_, dir_, GetParam())) << storage_.last_error_message();
        ASSERT_EQ(storage_.first_sequence(), kFirst);
    }
    void TearDown() override {
        if (storage_.isOpen()) {
            storage_.close();
        }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    std::vector<csdb::Pool> pools_;
    fs::path dir_;
    csdb::Storage storage_;
};

}  // namespace

// Blocks 49 and 48 are stored below 50: they are found by sequence and by hash at once, the first
// stored block follows them, and the head (last hash, count) is what it was.
TEST_P(StorageBackfill, StoresBlocksDownwardsAndLeavesTheHeadAlone) {
    const size_t sizeBefore = storage_.size();
    EXPECT_EQ(storage_.last_hash(), pools_[kLast].hash());

    ASSERT_TRUE(storage_.pool_save_backfill(pools_[49])) << storage_.last_error_message();
    EXPECT_EQ(storage_.first_sequence(), 49u);
    ASSERT_TRUE(storage_.pool_save_backfill(pools_[48])) << storage_.last_error_message();
    EXPECT_EQ(storage_.first_sequence(), 48u);

    EXPECT_EQ(storage_.size(), sizeBefore);
    EXPECT_EQ(storage_.last_hash(), pools_[kLast].hash());

    for (cs::Sequence s : {48u, 49u}) {
        const csdb::Pool bySeq = storage_.pool_load(s);
        ASSERT_TRUE(bySeq.is_valid()) << s;
        EXPECT_EQ(bySeq.hash(), pools_[s].hash());
        EXPECT_EQ(storage_.pool_load(pools_[s].hash()).sequence(), s);
        EXPECT_EQ(storage_.pool_sequence(pools_[s].hash()), s);
        EXPECT_EQ(storage_.pool_hash(s), pools_[s].hash());
        cs::Bytes bytes;
        ASSERT_TRUE(storage_.pool_bytes(s, bytes));
        EXPECT_EQ(bytes, pools_[s].to_binary());
    }
    EXPECT_EQ(storage_.pool_load(kLast).hash(), pools_[kLast].hash());
}

// What is stored already, or not directly below a stored block, is refused and changes nothing.
TEST_P(StorageBackfill, RefusesStoredBlocksAndGaps) {
    ASSERT_TRUE(storage_.pool_save_backfill(pools_[49]));

    EXPECT_FALSE(storage_.pool_save_backfill(pools_[49]));
    EXPECT_EQ(storage_.last_error(), csdb::Storage::InvalidParameter);
    EXPECT_FALSE(storage_.pool_save_backfill(pools_[55]));
    EXPECT_EQ(storage_.last_error(), csdb::Storage::InvalidParameter);
    EXPECT_FALSE(storage_.pool_save_backfill(pools_[47])) << "block 48 is not stored";
    EXPECT_EQ(storage_.last_error(), csdb::Storage::InvalidParameter);
    EXPECT_FALSE(storage_.pool_save_backfill(pools_[kLast + 1])) << "the next block goes through pool_save";
    EXPECT_FALSE(storage_.pool_save_backfill(csdb::Pool{}));

    EXPECT_EQ(storage_.first_sequence(), 49u);
    EXPECT_FALSE(storage_.pool_load(47).is_valid());
    EXPECT_EQ(storage_.last_hash(), pools_[kLast].hash());
}

// The head keeps growing as before while the bottom grows downwards.
TEST_P(StorageBackfill, TheNextBlockIsStillSavedOnTop) {
    ASSERT_TRUE(storage_.pool_save_backfill(pools_[49]));
    const size_t sizeBefore = storage_.size();
    ASSERT_TRUE(storage_.pool_save(pools_[kLast + 1])) << storage_.last_error_message();
    ASSERT_TRUE(storage_.flush());
    EXPECT_EQ(storage_.size(), sizeBefore + 1);
    EXPECT_EQ(storage_.last_hash(), pools_[kLast + 1].hash());
    EXPECT_EQ(storage_.pool_load(kLast + 1).hash(), pools_[kLast + 1].hash());
    EXPECT_EQ(storage_.first_sequence(), 49u);
}

// A pruned node switched to full: the blocks pruning removed are stored again where they were (on
// BerkeleyDB each one fills its deleted record).
TEST_P(StorageBackfill, PrunedBlocksAreStoredAgain) {
    std::vector<csdb::PoolHash> pruned;
    for (cs::Sequence s = kFirst; s < 55; ++s) {
        pruned.push_back(pools_[s].hash());
    }
    ASSERT_TRUE(storage_.pool_prune_batch(pruned));
    ASSERT_EQ(storage_.first_sequence(), 55u);
    EXPECT_FALSE(storage_.pool_save_backfill(pools_[53])) << "block 54 is not stored";

    for (cs::Sequence s = 55; s-- > 52;) {
        ASSERT_TRUE(storage_.pool_save_backfill(pools_[s])) << s << ": " << storage_.last_error_message();
    }
    EXPECT_EQ(storage_.first_sequence(), 52u);
    for (cs::Sequence s = 52; s < 55; ++s) {
        EXPECT_EQ(storage_.pool_load(s).hash(), pools_[s].hash()) << s;
        EXPECT_EQ(storage_.pool_sequence(pools_[s].hash()), s);
    }
    EXPECT_EQ(storage_.last_hash(), pools_[kLast].hash());
}

// Down to block 0, then a restart: the storage opens with every block, first block 0.
TEST_P(StorageBackfill, ReachesBlockZeroAndOpensAgain) {
    for (cs::Sequence s = kFirst; s-- > 0;) {
        ASSERT_TRUE(storage_.pool_save_backfill(pools_[s])) << s << ": " << storage_.last_error_message();
    }
    EXPECT_EQ(storage_.first_sequence(), 0u);
    ASSERT_TRUE(storage_.flush());
    storage_.close();

    csdb::Storage reopened;
    ASSERT_TRUE(openPruned(reopened, dir_, GetParam())) << reopened.last_error_message();
    EXPECT_EQ(reopened.first_sequence(), 0u);
    EXPECT_EQ(reopened.last_hash(), pools_[kLast].hash());
    for (cs::Sequence s = 0; s <= kLast; ++s) {
        ASSERT_EQ(reopened.pool_load(s).hash(), pools_[s].hash()) << s;
    }
    reopened.close();
}

INSTANTIATE_TEST_CASE_P(AllBackends, StorageBackfill, ::testing::Values("berkeleydb", "lmdb", "rocksdb"));

// With the asynchronous writer (RocksDB) a backfilled block goes to the database at once, not into
// the write queue, even while the writer is busy with the head.
TEST(StorageBackfillAsync, TheBlockIsInTheDatabaseAtOnce) {
    const auto pools = chain(kLast + 3);
    auto db = std::make_shared<MemoryDb>();
    putRange(*db, pools, kFirst, kLast);

    csdb::Storage storage;
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.startSequence = kLast + 1;
    opt.writeBatchSize = 100;
    opt.asyncWrites = true;
    ASSERT_TRUE(storage.open(opt)) << storage.last_error_message();
    ASSERT_EQ(storage.first_sequence(), kFirst);
    ASSERT_EQ(storage.last_hash(), pools[kLast].hash());

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[kLast + 1]));
    ASSERT_TRUE(db->waitCalls(1));              // the writer holds block 61
    ASSERT_TRUE(storage.pool_save(pools[kLast + 2]));   // block 62 waits in the queue

    ASSERT_TRUE(storage.pool_save_backfill(pools[49])) << storage.last_error_message();
    EXPECT_TRUE(db->has(pools[49].hash())) << "the backfilled block waited for the writer";
    EXPECT_EQ(storage.first_sequence(), 49u);
    EXPECT_EQ(storage.last_hash(), pools[kLast + 2].hash());
    EXPECT_FALSE(db->has(pools[kLast + 2].hash()));

    db->release();
    ASSERT_TRUE(storage.flush());
    EXPECT_TRUE(db->has(pools[kLast + 2].hash()));
    EXPECT_EQ(db->stored(), static_cast<size_t>(kLast - kFirst + 1 + 3));
    storage.close();
}

// A database that refuses the write (a full disk): false with the database's error, nothing stored.
TEST(StorageBackfillAsync, AFailedWriteIsReported) {
    class RefusingDb : public MemoryDb {
    public:
        bool refuse = false;
        bool put(const cs::Bytes& key, uint32_t seq, const cs::Bytes& value) override {
            if (refuse) {
                set_last_error(csdb::Database::IOError, "no space left");
                return false;
            }
            return MemoryDb::put(key, seq, value);
        }
    };
    const auto pools = chain(kLast + 1);
    auto db = std::make_shared<RefusingDb>();
    putRange(*db, pools, kFirst, kLast);

    csdb::Storage storage;
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.startSequence = kLast + 1;
    ASSERT_TRUE(storage.open(opt)) << storage.last_error_message();

    db->refuse = true;
    EXPECT_FALSE(storage.pool_save_backfill(pools[49]));
    EXPECT_EQ(storage.last_error(), csdb::Storage::DatabaseError);
    EXPECT_NE(storage.last_error_message().find("no space left"), std::string::npos) << storage.last_error_message();
    EXPECT_FALSE(db->has(pools[49].hash()));
    EXPECT_EQ(storage.first_sequence(), kFirst);

    db->refuse = false;
    EXPECT_TRUE(storage.pool_save_backfill(pools[49]));
    storage.close();
}
