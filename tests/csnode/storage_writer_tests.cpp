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

// The asynchronous block writer of csdb::Storage (used with RocksDB), on an in-memory database whose
// put_batch can be held or made to fail.
namespace {

using namespace std::chrono_literals;

class MemoryDb : public csdb::Database {
public:
    bool is_open() const override { return true; }

    bool put(const cs::Bytes& key, uint32_t seq, const cs::Bytes& value) override {
        std::lock_guard<std::mutex> l(m_);
        byHash_[key] = {seq, value};
        bySeq_[seq] = value;
        return true;
    }
    bool get(const cs::Bytes& key, cs::Bytes* value = nullptr) override {
        std::lock_guard<std::mutex> l(m_);
        auto it = byHash_.find(key);
        if (it == byHash_.end()) return false;
        if (value) *value = it->second.second;
        return true;
    }
    bool get(const uint32_t seq, cs::Bytes* value = nullptr) override {
        std::lock_guard<std::mutex> l(m_);
        auto it = bySeq_.find(seq);
        if (it == bySeq_.end()) return false;
        if (value) *value = it->second;
        return true;
    }
    bool remove(const cs::Bytes& key) override {
        std::lock_guard<std::mutex> l(m_);
        auto it = byHash_.find(key);
        if (it == byHash_.end()) return false;
        bySeq_.erase(it->second.first);
        byHash_.erase(it);
        return true;
    }
    bool seq_no(const cs::Bytes& key, uint32_t* value) override {
        std::lock_guard<std::mutex> l(m_);
        auto it = byHash_.find(key);
        if (it == byHash_.end()) return false;
        if (value) *value = it->second.first;
        return true;
    }
    bool write_batch(const ItemList&) override { return true; }
    bool updateContractData(const cs::Bytes&, const cs::Bytes&) override { return true; }
    bool getContractData(const cs::Bytes&, cs::Bytes&) override { return false; }

    // put_batch waits while held, fails while failing; calls are counted
    bool put_batch(const std::vector<PendingWrite>& items) override {
        std::unique_lock<std::mutex> l(gate_);
        ++calls_;
        gateCond_.notify_all();
        gateCond_.wait(l, [this] { return permits_ > 0 || !held_; });
        if (held_) --permits_;
        if (failing_) return false;
        l.unlock();
        return Database::put_batch(items);
    }

    void hold() { std::lock_guard<std::mutex> l(gate_); held_ = true; permits_ = 0; }
    void permit(int n) { std::lock_guard<std::mutex> l(gate_); permits_ += n; gateCond_.notify_all(); }
    void release() { std::lock_guard<std::mutex> l(gate_); held_ = false; gateCond_.notify_all(); }
    void fail(bool on) { std::lock_guard<std::mutex> l(gate_); failing_ = on; }
    bool waitCalls(int n, std::chrono::milliseconds limit = 5s) {
        std::unique_lock<std::mutex> l(gate_);
        return gateCond_.wait_for(l, limit, [&] { return calls_ >= n; });
    }

    size_t stored() { std::lock_guard<std::mutex> l(m_); return byHash_.size(); }
    bool has(const csdb::PoolHash& h) { return get(h.to_binary()); }

    class EmptyIterator : public Iterator {
    public:
        bool is_valid() const override { return false; }
        void seek_to_first() override {}
        void seek_to_last() override {}
        void seek(const cs::Bytes&) override {}
        void seek(const uint32_t) override {}
        void next() override {}
        void prev() override {}
        uint32_t key() const override { return 0; }
        cs::Bytes value() const override { return {}; }
    };
    IteratorPtr new_iterator() override { return std::make_shared<EmptyIterator>(); }

private:
    std::mutex m_;
    std::map<cs::Bytes, std::pair<uint32_t, cs::Bytes>> byHash_;
    std::map<uint32_t, cs::Bytes> bySeq_;

    std::mutex gate_;
    std::condition_variable gateCond_;
    bool held_ = false;
    int permits_ = 0;
    bool failing_ = false;
    int calls_ = 0;
};

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
