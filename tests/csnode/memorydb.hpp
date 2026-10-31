#ifndef TESTS_CSNODE_MEMORYDB_HPP
#define TESTS_CSNODE_MEMORYDB_HPP

#include <chrono>
#include <condition_variable>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <csdb/database.hpp>
#include <csdb/pool.hpp>

// An in-memory block database for csdb::Storage tests, whose put_batch (the asynchronous writer's
// call, as with RocksDB) can be held or made to fail. Its iterator walks a copy of the blocks taken
// when it is created, in sequence order, so that a storage can open on blocks put beforehand.
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
    bool waitCalls(int n, std::chrono::milliseconds limit = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> l(gate_);
        return gateCond_.wait_for(l, limit, [&] { return calls_ >= n; });
    }

    size_t stored() { std::lock_guard<std::mutex> l(m_); return byHash_.size(); }
    bool has(const csdb::PoolHash& h) { return get(h.to_binary()); }

    class MapIterator : public Iterator {
    public:
        explicit MapIterator(std::map<uint32_t, cs::Bytes> blocks)
        : blocks_(std::move(blocks)), it_(blocks_.end()) {}
        bool is_valid() const override { return it_ != blocks_.end(); }
        void seek_to_first() override { it_ = blocks_.begin(); }
        void seek_to_last() override { it_ = blocks_.empty() ? blocks_.end() : std::prev(blocks_.end()); }
        void seek(const cs::Bytes&) override { it_ = blocks_.end(); }
        void seek(const uint32_t seq) override { it_ = blocks_.find(seq); }
        void next() override { if (it_ != blocks_.end()) ++it_; }
        void prev() override { it_ = (it_ == blocks_.begin() || it_ == blocks_.end()) ? blocks_.end() : std::prev(it_); }
        uint32_t key() const override { return is_valid() ? it_->first : 0; }
        cs::Bytes value() const override { return is_valid() ? it_->second : cs::Bytes{}; }

    private:
        std::map<uint32_t, cs::Bytes> blocks_;
        std::map<uint32_t, cs::Bytes>::const_iterator it_;
    };
    IteratorPtr new_iterator() override {
        std::lock_guard<std::mutex> l(m_);
        return std::make_shared<MapIterator>(bySeq_);
    }

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

#endif  // TESTS_CSNODE_MEMORYDB_HPP
