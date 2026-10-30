#include "gtest/gtest.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <csdb/database.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage.hpp>

#include "memorydb.hpp"

// The asynchronous block writer of csdb::Storage (used with RocksDB), on an in-memory database whose
// put_batch can be held or made to fail.
namespace {

using namespace std::chrono_literals;

std::vector<csdb::Pool> chain(size_t count) {
    std::vector<csdb::Pool> pools;
    csdb::PoolHash prev;
    for (size_t i = 0; i < count; ++i) {
        csdb::Pool p(prev, i);
        p.add_user_field(1, std::string(100, static_cast<char>('a' + i)));
        EXPECT_TRUE(p.compose());
        prev = p.hash();
        pools.push_back(p);
    }
    return pools;
}

bool openAsync(csdb::Storage& storage, const std::shared_ptr<MemoryDb>& db) {
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.writeBatchSize = 100;
    opt.asyncWrites = true;
    return storage.open(opt);
}

// runs f on its own thread; false if it did not return within the limit (the thread is left behind)
template <typename F>
bool returnsWithin(F f, std::chrono::milliseconds limit) {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    std::thread([f, done]() mutable { f(); done->set_value(); }).detach();
    return fut.wait_for(limit) == std::future_status::ready;
}

}  // namespace

// The last block is in the batch being written when the node removes it (fork resolution): it must
// end up removed from the database too, and the writer must not pop more than it took.
TEST(StorageWriter, RemovingTheLastBlockWhileItIsWrittenRemovesItFromTheDatabase) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(4);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));          // the writer holds [0]
    for (size_t i = 1; i < 4; ++i) ASSERT_TRUE(storage.pool_save(pools[i]));
    db->permit(1);                          // [0] lands, the writer takes [1, 2, 3]
    ASSERT_TRUE(db->waitCalls(2));

    std::promise<csdb::Pool> removed;
    auto removedFut = removed.get_future();
    std::thread remover([&] { removed.set_value(storage.pool_remove_last()); });
    std::this_thread::sleep_for(100ms);     // the removal runs while [1, 2, 3] is being written
    db->release();
    remover.join();

    const csdb::Pool last = removedFut.get();
    ASSERT_TRUE(last.is_valid());
    EXPECT_EQ(last.hash(), pools[3].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_FALSE(db->has(pools[3].hash())) << "a removed block was written anyway";
    EXPECT_TRUE(db->has(pools[2].hash()));
    EXPECT_EQ(storage.size(), 3u);
    EXPECT_EQ(storage.last_hash(), pools[2].hash());
    EXPECT_EQ(db->stored(), 3u);
    storage.close();
}

// A block removed from the queue is no longer counted.
TEST(StorageWriter, RemovingAQueuedBlockUncountsIt) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));

    std::promise<csdb::Pool> removed;
    auto removedFut = removed.get_future();
    std::thread remover([&] { removed.set_value(storage.pool_remove_last()); });
    std::this_thread::sleep_for(100ms);
    db->release();
    remover.join();

    EXPECT_EQ(removedFut.get().hash(), pools[2].hash());
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.last_hash(), pools[1].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_EQ(db->stored(), 2u);
    storage.close();
}

// The repair of the last block while it still waits in the queue removes that block only; the blocks
// before it stay counted and reach the database.
TEST(StorageWriter, RepairOfAQueuedLastBlockKeepsTheBlocksBeforeIt) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));

    std::promise<bool> repaired;
    auto repairedFut = repaired.get_future();
    std::thread repairer([&] { repaired.set_value(storage.pool_remove_last_repair(2, pools[2].hash())); });
    std::this_thread::sleep_for(100ms);
    db->release();
    repairer.join();

    EXPECT_TRUE(repairedFut.get());
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.last_hash(), pools[1].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_TRUE(db->has(pools[1].hash())) << "a counted block was dropped from the queue";
    EXPECT_FALSE(db->has(pools[2].hash()));
    storage.close();
}

// The database refuses every write (disk full): flush() and pool_save() report it instead of waiting,
// and close() returns, so that the node can save no checkpoint ahead of the disk and can stop.
TEST(StorageWriter, PersistentWriteFailureNeitherHangsNorStops) {
    auto db = std::make_shared<MemoryDb>();
    auto storage = std::make_shared<csdb::Storage>();
    ASSERT_TRUE(openAsync(*storage, db));
    const auto pools = chain(2);

    db->fail(true);
    ASSERT_TRUE(storage->pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));

    bool flushed = true;
    ASSERT_TRUE(returnsWithin([storage, &flushed] { flushed = storage->flush(); }, 5s)) << "flush() hangs";
    EXPECT_FALSE(flushed);
    bool saved = true;
    ASSERT_TRUE(returnsWithin([storage, p = pools[1], &saved] { saved = storage->pool_save(p); }, 5s)) << "pool_save() hangs";
    EXPECT_FALSE(saved) << "a block was queued while the database refuses writes";
    EXPECT_EQ(storage->size(), 1u);
    ASSERT_TRUE(returnsWithin([storage] { storage->close(); }, 5s)) << "close() hangs";
}

// When the database accepts writes again the queued blocks are written and the storage works on.
TEST(StorageWriter, RecoversWhenWritesSucceedAgain) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->fail(true);
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    EXPECT_FALSE(storage.flush());
    db->fail(false);

    // the writer retries within a second or so
    bool flushed = false;
    for (int i = 0; i < 50 && !flushed; ++i) {
        std::this_thread::sleep_for(100ms);
        flushed = storage.flush();
    }
    ASSERT_TRUE(flushed);
    EXPECT_TRUE(db->has(pools[0].hash()));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));
    ASSERT_TRUE(storage.flush());
    EXPECT_EQ(db->stored(), 3u);
    EXPECT_EQ(storage.size(), 3u);
    storage.close();
}

// The only block waiting for a failing database is removed (fork resolution): nothing fails any more,
// so once the disk is freed the next block must be accepted, not refused until a restart.
TEST(StorageWriter, RemovingTheLastQueuedBlockEndsTheFailure) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(2);

    db->fail(true);
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    EXPECT_FALSE(storage.flush());
    const csdb::Pool removed = storage.pool_remove_last();
    EXPECT_EQ(removed.hash(), pools[0].hash());
    EXPECT_EQ(storage.size(), 0u);
    db->fail(false);

    ASSERT_TRUE(storage.pool_save(pools[0])) << "the storage still refuses writes after the queue emptied";
    ASSERT_TRUE(storage.flush());
    EXPECT_TRUE(db->has(pools[0].hash()));
    storage.close();
}
