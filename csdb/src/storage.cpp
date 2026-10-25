#include <csdb/storage.hpp>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <thread>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>

#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>

#include <csdb/address.hpp>
#include <csdb/database.hpp>
#if defined(CSDB_USE_ROCKSDB) && defined(CSDB_USE_BERKELEYDB)
#include <csdb/database_berkeleydb.hpp>
#include <csdb/database_rocksdb.hpp>
#elif defined(CSDB_USE_ROCKSDB)
#include <csdb/database_rocksdb.hpp>
#else
#include <csdb/database_berkeleydb.hpp>
#endif
#include <csdb/database_lmdb.hpp>
#include <csdb/internal/shared_data_ptr_implementation.hpp>
#include <csdb/internal/utils.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage_backend.hpp>
#include <csdb/wallet.hpp>

#include "binary_streams.hpp"

using namespace boost::multi_index;

namespace {
struct last_error_struct {
    ::csdb::Storage::Error last_error_ = ::csdb::Storage::NoError;
    std::string last_error_message_;
};

last_error_struct& last_error_map(const void* p) {
    static thread_local ::std::map<const void*, last_error_struct> last_errors_;
    return last_errors_[p];
}
}  // namespace

namespace csdb {

namespace {

struct head_info_t {
    size_t len_;     // Number of blocks in the chain
    PoolHash next_;  // next pool hash, or empty string for genesis
                     // in chain (no parent, chain start).
};
using heads_t = std::map<PoolHash, head_info_t>;
using tails_t = std::map<PoolHash, PoolHash>;

[[maybe_unused]] void update_heads_and_tails(heads_t& heads, tails_t& tails, const PoolHash& cur_hash, const PoolHash& prev_hash) {
    auto ith = heads.find(prev_hash);
    auto itt = tails.find(cur_hash);
    bool eith = (heads.end() != ith);
    bool eitt = (tails.end() != itt);
    if (eith && eitt) {
        // Concat two chains
        assert(1 == heads.count(itt->second));
        head_info_t& ith1 = heads[itt->second];
        ith1.next_ = ith->second.next_;
        ith1.len_ += (1 + ith->second.len_);
        if (!ith->second.next_.is_empty()) {
            /// \todo Check, when assert is called!
            // assert(1 == tails.count(ith->second.next_));
            tails[ith->second.next_] = itt->second;
        }
        heads.erase(ith);
        // Probably we changed tails - don't remove using iterator!
        tails.erase(cur_hash);
    }
    else if (eith && (!eitt)) {
        // Add to the beginning of the chain
        if (!ith->second.next_.is_empty()) {
            /// \todo \todo Check, when assert is called!
            // assert(1 == tails.count(ith->second.next_));
            tails[ith->second.next_] = cur_hash;
        }
        assert(0 == heads.count(cur_hash));
        heads.emplace(cur_hash, head_info_t{ith->second.len_ + 1, ith->second.next_});
        heads.erase(prev_hash);
    }
    else if ((!eith) && eitt) {
        // Add to the end of the chain
        assert(1 == heads.count(itt->second));
        head_info_t& ith1 = heads[itt->second];
        ith1.next_ = prev_hash;
        ++ith1.len_;
        if (!prev_hash.is_empty()) {
            // we don't need assert, because the presence of such tail ith the evidence of the crossing of cycling chains
            // (i.e. we had the chain with the same tail).
            // TODO: Create such cases detection (after utin-tests)
            // assert(0 == tails.count(prev_hash));
            tails.emplace(prev_hash, itt->second);
        }
        tails.erase(cur_hash);
    }
    else {
        // No intersections! Just hang up
        assert(0 == heads.count(cur_hash));
        heads.emplace(cur_hash, head_info_t{1, prev_hash});
        if (!prev_hash.is_empty()) {
            // look TODO to prev thread.
            // assert(0 == tails.count(prev_hash));
            tails.emplace(prev_hash, cur_hash);
        }
    }
}

}  // namespace

class Storage::priv {
public:
    priv() {
        last_error_map(this) = last_error_struct();
    }

    ~priv() {
        if (write_thread.joinable()) {
            // writer drains write_queue before honoring quit.
            {
                std::unique_lock<std::mutex> lock(write_lock);
                quit = true;
            }
            write_cond_var.notify_all();
            write_thread.join();
        }
    }

private:
    bool rescan(Storage::OpenCallback callback);
    void write_routine();

    std::shared_ptr<Database> db = nullptr;
    PoolHash last_hash;     // Last pool's hash
    size_t count_pool = 0;  // Number of transaction's pools inthe storage (initially filled in check)
    cs::Sequence startSequence = 0;

    void set_last_error(Storage::Error error = Storage::NoError, const ::std::string& message = ::std::string());
    void set_last_error(Storage::Error error, const char* message, ...);

    std::thread write_thread;
    bool quit = false;

    std::mutex data_lock;

    // Async write pipeline. Bounded; full queue back-pressures producers.
    // Defaults overridable via Storage::open(...).
    // Used with RocksDB only: BerkeleyDB and LMDB write each block synchronously, as before the
    // RocksDB backend, so the blocks on disk never lag behind the caches and pruning never writes
    // concurrently with a writer thread.
    bool async_writes = false;
    size_t asyncWriteQueueMax_ = 5000;
    size_t writeBatchSize_ = 100;        // pools coalesced into one db->put_batch
    std::deque<Pool> write_queue;
    // The batch the writer has taken from write_queue and is writing now. It stays searchable here,
    // and a removal of the last block waits until it is empty, so that a block is always either in
    // write_queue, here, or in the database, never in two of them.
    std::vector<Pool> in_flight;
    // the last write failed: its blocks are back at the front of write_queue and the writer retries;
    // meanwhile flush() and pool_save() fail at once instead of waiting
    bool write_failing = false;
    // removals of the last block waiting for in_flight to empty: the writer takes no new batch
    // meanwhile, so that they are not starved by a long queue
    size_t removals_waiting = 0;
    std::mutex write_lock;
    std::condition_variable write_cond_var;     // queue non-empty, or quit
    std::condition_variable queue_space_cond;   // queue has space, in_flight emptied, or write_failing changed

    // a queued or in-flight pool matching `match`; the caller holds write_lock
    template <typename Match>
    bool find_queued(Match match, Pool& out) const {
        for (const auto& pool : in_flight) {
            if (match(pool)) { out = pool; return true; }
        }
        for (const auto& pool : write_queue) {
            if (match(pool)) { out = pool; return true; }
        }
        return false;
    }

    struct PoolElement {
        cs::Sequence seq; struct bySequence {};
        PoolHash hash;  struct byHash {};
        Pool pool;
    };

    typedef multi_index_container<
        PoolElement,
        indexed_by<
            hashed_unique<
                tag<PoolElement::bySequence>, member<
                    PoolElement, cs::Sequence, &PoolElement::seq
                >
            >,
            hashed_unique<
                tag<PoolElement::byHash>, member<
                    PoolElement, PoolHash, &PoolElement::hash
                >
            >
        >
    > PoolCache;
    PoolCache pools_cache;
    static const size_t cacheSize = 50;

    void pools_cache_insert(const cs::Sequence& seq, const PoolHash &hash, const Pool &pool) {
        if (pools_cache.size() == cacheSize) {
            auto random = pools_cache.begin();
            pools_cache.erase(random);
        }
        pools_cache.insert({seq, hash, pool});
    }

    friend class ::csdb::Storage;

private signals:
    ReadBlockSignal read_block_event;
    BlockReadingStartedSingal start_reading_event;
    BlockReadingStoppedSignal stop_reading_event;
};

void Storage::priv::set_last_error(Storage::Error error, const ::std::string& message) {
    last_error_struct& les = last_error_map(this);
    les.last_error_ = error;
    les.last_error_message_ = message;
}

void Storage::priv::set_last_error(Storage::Error error, const char* message, ...) {
    last_error_struct& les = last_error_map(this);
    les.last_error_ = error;

    if (nullptr != message) {
        va_list args1;
        va_start(args1, message);
        va_list args2;
        va_copy(args2, args1);
        les.last_error_message_.resize(std::vsnprintf(NULL, 0, message, args1) + 1);
        va_end(args1);
        std::vsnprintf(&(les.last_error_message_[0]), les.last_error_message_.size(), message, args2);
        va_end(args2);
        les.last_error_message_.resize(les.last_error_message_.size() - 1);
    }
    else {
        les.last_error_message_.clear();
    }

    if (error != Storage::Error::NoError) {
        if (!les.last_error_message_.empty()) {
            cserror() << "Storage> error #" << static_cast<int>(error) << ": " << les.last_error_message_;
        }
        else {
            cserror() << "Storage> error #" << static_cast<int>(error);
        }
    }
}

bool Storage::priv::rescan(Storage::OpenCallback callback) {
    last_hash = {};
    count_pool = 0;
    cs::Sequence last_seq_in_db = 0;

    Database::IteratorPtr it = db->new_iterator();
    assert(it);

    it->seek_to_last();
    if (it->is_valid()) {
        cs::Bytes v = it->value();
        Pool p = Pool::from_binary(std::move(v));
        if (p.is_valid()) {
            last_seq_in_db = p.sequence();
            emit start_reading_event(p.sequence());
        }
        else {
            emit start_reading_event(0);
        }
    }
    else {
        emit start_reading_event(0);
    }

    if (startSequence > last_seq_in_db + 1) {
        set_last_error(Storage::StartAfterLastBlock,
                       "the quick-start checkpoint continues at block %llu, the last block stored is %llu",
                       static_cast<unsigned long long>(startSequence), static_cast<unsigned long long>(last_seq_in_db));
        cserror() << "startSequence > last_seq_in_db + 1";
        return false;
    }

    Storage::OpenProgress progress{0};

    bool slowStart = false;

    if (startSequence > 0 && [&]() { it->seek(startSequence - 1); return it->is_valid(); }()) {
        csinfo() << "Storage: quick start";
        count_pool = startSequence - 1;
        progress.poolsProcessed = count_pool;
        if (callback != nullptr) {
            if (callback(progress)) {
                set_last_error(Storage::UserCancelled);
                return false;
            }
        }
    }
    else {
        csinfo() << "Storage: slow start";
        it->seek_to_first();
        slowStart = true;
        // a pruned storage starts after block 0: replaying it onto empty caches would build a wrong
        // state, so such a node can only start from a quick-start checkpoint
        if (it->is_valid() && it->key() != 0) {
            set_last_error(Storage::DataIntegrityError,
                           "Pruned storage starts at block %u: it needs a quick-start checkpoint, a slow start is not possible",
                           it->key());
            cserror() << "Storage: pruned storage starts at block " << it->key()
                      << ", no quick-start checkpoint covers it; restore one in qs/ or resync from scratch";
            return false;
        }
    }

    if (count_pool == last_seq_in_db && !slowStart) {
        cs::Bytes v = it->value();
        Pool p = Pool::from_binary(std::move(v));
        if (p.is_valid()) {
            last_hash = p.hash();
        }
        else {
            set_last_error(Storage::DataIntegrityError, "Data integrity error: corrupted pool %d.", count_pool);
            cserror() << "Please restart node with command : client --set-bc-top " << count_pool - 1;
            return false;
        }
    }

    if (!slowStart) {
        it->next();
    }

    for (; it->is_valid(); it->next()) {
        cs::Bytes v = it->value();

            Pool p = Pool::from_binary(std::move(v));
            if (!p.is_valid()) {
                set_last_error(Storage::DataIntegrityError, "Data integrity error: Corrupted pool %d.", count_pool);
                cserror() << "Please restart node with command : client --set-bc-top " << count_pool - 1;
                return false;
            }
            pools_cache_insert(p.sequence(), p.hash(), p);

            bool test_failed = false;
            last_hash = p.hash();
            if (count_pool != p.sequence()) {
                csdebug() << "count_pool = " << count_pool << ", pool sequence = " << p.sequence();
            }
            count_pool++;

            emit read_block_event(p, &test_failed);
            if (test_failed) {
                set_last_error(Storage::DataIntegrityError, "Data integrity error: client reported violation of logic in pool %d", p.sequence());
                return false;
            }

        progress.poolsProcessed++;

        if (callback != nullptr) {
            if (callback(progress)) {
                set_last_error(Storage::UserCancelled);
                return false;
            }
        }
    }

    emit stop_reading_event();
    return true;
}

void Storage::priv::write_routine() {
    std::unique_lock<std::mutex> lock(write_lock);
    while (true) {
        // a removal waiting for the batch in flight goes first, even when stopping
        write_cond_var.wait(lock, [this] { return removals_waiting == 0 && (quit || !write_queue.empty()); });

        if (write_queue.empty() && quit) {
            return;
        }

        // take up to writeBatchSize_ pools out of the queue; they stay findable in in_flight
        const size_t take = std::min(writeBatchSize_, write_queue.size());
        in_flight.assign(std::make_move_iterator(write_queue.begin()),
                         std::make_move_iterator(write_queue.begin() + static_cast<std::ptrdiff_t>(take)));
        write_queue.erase(write_queue.begin(), write_queue.begin() + static_cast<std::ptrdiff_t>(take));

        // Build payloads outside the lock (a pool shares its data, the copy is cheap); one put_batch
        // coalesces 2*N puts into a single WriteBatch.
        const std::vector<Pool> batch = in_flight;
        lock.unlock();
        std::vector<Database::PendingWrite> requests;
        requests.reserve(batch.size());
        for (const auto& pool : batch) {
            requests.push_back(Database::PendingWrite{
                pool.hash().to_binary(),
                static_cast<uint32_t>(pool.sequence()),
                pool.to_binary()
            });
        }

        const bool written = db->put_batch(requests);

        lock.lock();
        if (written) {
            in_flight.clear();
            write_failing = false;
            queue_space_cond.notify_all();
            continue;
        }

        // not written: put the blocks back in front, in order, so that they are retried, found, and
        // removable; dropping them would leave a hole behind the caches
        write_queue.insert(write_queue.begin(), std::make_move_iterator(in_flight.begin()),
                           std::make_move_iterator(in_flight.end()));
        in_flight.clear();
        write_failing = true;
        queue_space_cond.notify_all();
        if (quit) {
            // stopping: give up, so that the node can stop. These blocks were counted but are not in
            // the database; no checkpoint covers them, since flush() failed while they waited.
            cserror() << "Storage: stopping with " << write_queue.size() << " block(s) not written, from "
                      << write_queue.front().sequence() << " to " << write_queue.back().sequence()
                      << ": " << db->last_error_message();
            return;
        }
        write_cond_var.wait_for(lock, std::chrono::seconds(1), [this] { return quit; });
    }
}

Storage::Storage()
: d(::std::make_shared<priv>()) {
}

Storage::~Storage() {
}

Storage::Storage(WeakPtr ptr) noexcept
: d(ptr.lock()) {
    if (!d) {
        d = ::std::make_shared<priv>();
    }
}

Storage::WeakPtr Storage::weak_ptr() const noexcept {
    return d;
}

Storage::Error Storage::last_error() const {
    return last_error_map(d.get()).last_error_;
}

::std::string Storage::last_error_message() const {
    const last_error_struct& les = last_error_map(d.get());
    if (!les.last_error_message_.empty()) {
        return les.last_error_message_;
    }
    switch (les.last_error_) {
        case NoError:
            return "No error";
        case NotOpen:
            return "Storage is not open";
        case DatabaseError:
            return "Database error: " + db_last_error_message();
        case ChainError:
            return "Chain integrity error";
        case DataIntegrityError:
            return "Data integrity error";
        case UserCancelled:
            return "Operation cancalled by user";
        case InvalidParameter:
            return "Invalid parameter passed to method.";
        default:
            return "Unknown error";
    }
}

Database::Error Storage::db_last_error() const {
    if (d->db) {
        return d->db->last_error();
    }
    return Database::NotOpen;
}

::std::string Storage::db_last_error_message() const {
    if (d->db) {
        return d->db->last_error_message();
    }
    return ::std::string{"Database not specified"};
}

bool Storage::open(const OpenOptions& opt, OpenCallback callback) {
    if (!opt.db) {
        d->set_last_error(DatabaseError, "No valid database driver specified.");
        return false;
    }

    d->db = opt.db;

    if (!d->db->is_open()) {
        d->set_last_error(DatabaseError, "Error open database: %s", d->db->last_error_message().c_str());
        return false;
    }

    if (opt.asyncWriteQueueMax > 0) {
        d->asyncWriteQueueMax_ = opt.asyncWriteQueueMax;
    }
    if (opt.writeBatchSize > 0) {
        d->writeBatchSize_ = opt.writeBatchSize;
    }
    if (opt.asyncWrites) {
        d->async_writes = true;
    }

    // Start async writer (idempotent: re-opens won't spawn a second thread).
    if (d->async_writes && !d->write_thread.joinable()) {
        d->write_thread = std::thread(&Storage::priv::write_routine, d.get());
    }

    if (opt.newBlockchainTop != cs::kWrongSequence) {
        auto seqToRemove = static_cast<uint32_t>(opt.newBlockchainTop + 1);
        auto seqLast = seqToRemove;
        {
            Database::IteratorPtr it = d->db->new_iterator();
            it->seek_to_last();
            if (it->is_valid()) {
                auto key = it->key();
                if (key != std::numeric_limits<uint32_t>::max()) {
                    seqLast = key;
                }
            }
        }

        auto blocksCount = (seqLast >= seqToRemove) ? (seqLast - seqToRemove + 1) : 0;
        if(seqToRemove <= seqLast) {
            cslog() << "start remove " << blocksCount << " blocks: " << seqToRemove << " .. " << seqLast;
        }
        else {
            cslog() << "nothing to remove over " << opt.newBlockchainTop << ", current last block is " << seqLast;
        }

        while (seqToRemove <= seqLast) {
            cs::Bytes poolBinary;
            if (!d->db->get(seqToRemove++, &poolBinary)) {
                continue;
            }
            auto hash = csdb::Pool::hash_from_binary(std::move(poolBinary));
            if (hash.is_empty()) {
                // try remove another way, using next block's previous hash
                cs::Bytes next_block_bytes;
                const uint32_t next_seq = uint32_t(seqToRemove);
                if (!d->db->get(next_seq, &next_block_bytes)) {
                    // cannot, sorry
                    return false;
                }
                size_t dummy = 0;
                auto next_pool = csdb::Pool::meta_from_binary(std::move(next_block_bytes), dummy);
                if (!next_pool.is_valid()) {
                    // cannot, sorry
                    return false;
                }
                if (!d->db->remove(next_pool.previous_hash().to_binary())) {
                    // cannot repair, sorry
                    return false;
                }
                continue;
            }
            if (!d->db->remove(hash.to_binary())) {
                break;
            }

            cslog() << "block " << seqToRemove - 1 << " is removed";
        }

        return true;
    }

    d->startSequence = opt.startSequence;

    if (!d->rescan(callback)) {
        d->db.reset();
        return false;
    }

    d->set_last_error();
    return true;
}

bool Storage::open(
    const ::std::string& path_to_base,
    OpenCallback callback,
    cs::Sequence newBlockchainTop,
    cs::Sequence startReadFrom,
    size_t asyncWriteQueueMax,
    size_t writeBatchSize,
    uint64_t rocksDbBlockCacheBytes,
    uint64_t rocksDbMemtableBytes,
    const std::string& dbBackend
) {
    ::std::string path{path_to_base};
    if (path.empty()) {
        path = ::csdb::internal::app_data_path() + "/CREDITS";
    }

    if (asyncWriteQueueMax > 0) {
        d->asyncWriteQueueMax_ = asyncWriteQueueMax;
    }
    if (writeBatchSize > 0) {
        d->writeBatchSize_ = writeBatchSize;
    }

    std::shared_ptr<Database> db;
    if (dbBackend == "lmdb") {
        // LMDB reuses the space of removed blocks, which suits pruned storage
        auto lmdb = std::make_shared<DatabaseLmdb>();
        if (!lmdb->open(path + "/lmdb")) {
            d->set_last_error(DatabaseError, "Error open database: %s", lmdb->last_error_message().c_str());
            return false;
        }
        OpenOptions opt{lmdb, newBlockchainTop, startReadFrom};
        const bool ok = open(opt, callback);
        if (ok) {
            backend::writeMarker(path, "lmdb");
        }
        return ok;
    }
    const char* backendName = "berkeleydb";   // what the code below opens, recorded in db/.backend
#if defined(CSDB_USE_ROCKSDB) && defined(CSDB_USE_BERKELEYDB)
    // Default berkeleydb; rocksdb is opt-in via [storage] db_backend = rocksdb.
    if (dbBackend == "rocksdb" || dbBackend == "rocks") {
        d->async_writes = true;
        auto rocks = std::make_shared<DatabaseRocksDB>();
        rocks->set_tuning(rocksDbBlockCacheBytes, rocksDbMemtableBytes);
        rocks->open(path);
        db = rocks;
        backendName = "rocksdb";
    } else {
        auto bdb = std::make_shared<DatabaseBerkeleyDB>();
        bdb->open(path);
        db = bdb;
    }
#elif defined(CSDB_USE_ROCKSDB)
    d->async_writes = true;
    auto rocks = std::make_shared<DatabaseRocksDB>();
    rocks->set_tuning(rocksDbBlockCacheBytes, rocksDbMemtableBytes);
    rocks->open(path);
    db = rocks;
    backendName = "rocksdb";
    (void)dbBackend;
#else
    auto bdb = std::make_shared<DatabaseBerkeleyDB>();
    bdb->open(path);
    db = bdb;
    (void)rocksDbBlockCacheBytes;
    (void)rocksDbMemtableBytes;
    (void)dbBackend;
#endif
    OpenOptions opt{db, newBlockchainTop, startReadFrom};
    const bool ok = open(opt, callback);
    if (ok) {
        backend::writeMarker(path, backendName);
    }
    return ok;
}

bool Storage::flush() {
    if (!d || !d->db) return false;
    if (d->async_writes) {
        // every block handed to the writer must be in the database before the caller relies on it
        std::unique_lock<std::mutex> lock(d->write_lock);
        d->queue_space_cond.wait(lock, [this] {
            return (d->write_queue.empty() && d->in_flight.empty()) || d->write_failing || !d->write_thread.joinable();
        });
        if (!d->write_queue.empty() || !d->in_flight.empty()) {
            d->set_last_error(DatabaseError, "%s: %zu block(s) wait in the write queue, the database does not accept writes",
                              funcName(), d->write_queue.size() + d->in_flight.size());
            return false;
        }
    }
    return d->db->flush();
}

void Storage::close() {
    // Drain the async writer before destroying the db to avoid use-after-free in RocksDB's dtor.
    if (d->write_thread.joinable()) {
        {
            std::unique_lock<std::mutex> lock(d->write_lock);
            d->quit = true;
        }
        d->write_cond_var.notify_all();
        d->write_thread.join();
    }
    d->db.reset();
    d->set_last_error();
}

bool Storage::isOpen() const {
    return ((d->db) && (d->db->is_open()));
}

PoolHash Storage::last_hash() const noexcept {
    std::unique_lock<std::mutex> lock(d->data_lock);
    return d->last_hash;
}

size_t Storage::size() const noexcept {
    std::unique_lock<std::mutex> lock(d->data_lock);
    return d->count_pool;
}

bool Storage::pool_save(Pool pool) {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }

    if (!pool.is_valid()) {
        d->set_last_error(InvalidParameter, "%s: Invalid pool passed", funcName());
        return false;
    }

    const PoolHash hash = pool.hash();

    if (d->db->get(hash.to_binary())) {
        d->set_last_error(InvalidParameter, "%s: Pool already present [hash: %s]", funcName(), hash.to_string().c_str());
        return false;
    }

    if (d->last_hash != pool.previous_hash()) {
        d->set_last_error(InvalidParameter, "%s: Trying to save pool with another prev hash [hash: %s]", funcName(), pool.previous_hash().to_string().c_str());
        return false;
    }

    if (!d->async_writes) {
        if (!d->db->put(hash.to_binary(), static_cast<uint32_t>(pool.sequence()), pool.to_binary())) {
            // not stored: do not count it, so the next block cannot be saved on top of a hole
            d->set_last_error(DatabaseError, "%s: cannot write pool %llu: %s", funcName(),
                              static_cast<unsigned long long>(pool.sequence()), d->db->last_error_message().c_str());
            return false;
        }
    }
    else {
        // Hand off to write_routine; backpressure if queue full.
        std::unique_lock<std::mutex> lock(d->write_lock);
        d->queue_space_cond.wait(lock, [this] {
            return d->write_queue.size() < d->asyncWriteQueueMax_ || d->write_failing;
        });
        // refuse only while blocks still wait for the failing database: once removals emptied the
        // queue nothing is failing any more, and the next block is written (or fails) on its own
        if (d->write_failing && (!d->write_queue.empty() || !d->in_flight.empty())) {
            // as in the synchronous path: not stored, so not counted, and the caches do not run ahead
            d->set_last_error(DatabaseError, "%s: cannot queue pool %llu, the database does not accept writes", funcName(),
                              static_cast<unsigned long long>(pool.sequence()));
            return false;
        }
        d->write_queue.push_back(pool);
        d->write_cond_var.notify_one();
    }

    {
        std::unique_lock<std::mutex> lock(d->data_lock);
        ++d->count_pool;
        if (d->last_hash == pool.previous_hash()) {
            d->last_hash = hash;
        }
    }

    d->pools_cache_insert(pool.sequence(), pool.hash(), pool);

    d->set_last_error();
    return true;
}

Pool Storage::pool_load_internal(const PoolHash& hash, const bool metaOnly, size_t& trxCnt) const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return Pool{};
    }

    if (hash.is_empty()) {
        d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
        return Pool{};
    }

    Pool res;
    bool needParseData = true;
    cs::Bytes data;

    const auto &index = d->pools_cache.get<Storage::priv::PoolElement::byHash>();
    auto it = index.find(hash);
    if (it != index.end()) {
        res = (*it).pool;
        if (!res.is_valid()) {
            d->set_last_error(DataIntegrityError, "%s: Error decoding pool [hash: %s]", funcName(), hash.to_string().c_str());
            return Pool{};
        }
        else {
            d->set_last_error();
        }
        trxCnt = res.transactions().size();
        return res;
    }

    if (!d->db->get(hash.to_binary(), &data)) {
        {
            std::unique_lock<std::mutex> lock2(d->write_lock);
            if (d->find_queued([&](const Pool& p) { return p.hash() == hash; }, res)) {
                needParseData = false;
                trxCnt = res.transactions().size();
            }
        }

        if (needParseData && !d->db->get(hash.to_binary(), &data)) {
            d->set_last_error(DatabaseError);
            return Pool{};
        }
    }

    if (needParseData) {
        if (metaOnly) {
            res = Pool::meta_from_binary(std::move(data), trxCnt);
        }
        else {
            res = Pool::from_binary(std::move(data));
            trxCnt = res.transactions().size();
            d->pools_cache_insert(res.sequence(), res.hash(), res);
        }
    }

    if (!res.is_valid()) {
        d->set_last_error(DataIntegrityError, "%s: Error decoding pool [hash: %s]", funcName(), hash.to_string().c_str());
        return Pool{};
    }
    else {
        d->set_last_error();
    }

    return res;
}

bool Storage::write_queue_search(const PoolHash& hash, Pool& res_pool) const {
    std::unique_lock<std::mutex> lock(d->write_lock);
    return d->find_queued([&](const Pool& pool) { return hash == pool.hash(); }, res_pool);
}

void Storage::wait_for_in_flight(std::unique_lock<std::mutex>& lock) {
    // a batch being written may hold the last block: wait until it is in the database, so that the
    // last block is then either the back of the queue or the last block of the database
    ++d->removals_waiting;
    d->queue_space_cond.wait(lock, [this] { return d->in_flight.empty(); });
    --d->removals_waiting;
    d->write_cond_var.notify_all();
}

bool Storage::write_queue_pop(Pool& res_pool) {
    std::unique_lock<std::mutex> lock(d->write_lock);
    wait_for_in_flight(lock);

    if (!d->write_queue.empty()) {
        res_pool = d->write_queue.back();
        d->write_queue.pop_back();
        if (d->write_queue.empty()) {
            d->write_failing = false;    // nothing waits for the database any more
        }
        d->queue_space_cond.notify_all();
        return true;
    }
    return false;
}

bool Storage::write_queue_pop_last(const PoolHash& hash, bool& queued) {
    queued = false;
    std::unique_lock<std::mutex> lock(d->write_lock);
    wait_for_in_flight(lock);
    if (d->write_queue.empty()) {
        return true;    // the last block is in the database
    }
    queued = true;
    if (d->write_queue.back().hash() != hash) {
        d->set_last_error(InvalidParameter, "%s: the last block in the write queue is not %s", funcName(), hash.to_string().c_str());
        return false;
    }
    const Pool last = d->write_queue.back();
    d->write_queue.pop_back();
    if (d->write_queue.empty()) {
        d->write_failing = false;    // nothing waits for the database any more
    }
    d->queue_space_cond.notify_all();
    lock.unlock();

    std::unique_lock<std::mutex> data(d->data_lock);
    --d->count_pool;
    d->last_hash = last.previous_hash();
    return true;
}

Pool Storage::pool_load(const PoolHash& hash) const {
    size_t size;
    return pool_load_internal(hash, false, size);
}

Pool Storage::pool_load(const cs::Sequence sequence) const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return Pool{};
    }

    Pool res;
    bool needParseData = true;
    cs::Bytes data;

    const auto &index = d->pools_cache.get<Storage::priv::PoolElement::bySequence>();
    auto it = index.find(sequence);
    if (it != index.end()) {
        res = (*it).pool;
        if (!res.is_valid()) {
            d->set_last_error(DataIntegrityError);
            return Pool{};
        }
        else {
            d->set_last_error();
        }
        return res;
    }

    if (!d->db->get(static_cast<uint32_t>(sequence), &data)) {
        {
            std::unique_lock<std::mutex> lock2(d->write_lock);
            if (d->find_queued([&](const Pool& p) { return p.sequence() == sequence; }, res)) {
                needParseData = false;
            }
        }

        if (needParseData && !d->db->get(static_cast<uint32_t>(sequence), &data)) {
            d->set_last_error(DatabaseError);
            return Pool{};
        }
    }

    if (needParseData) {
        res = Pool::from_binary(std::move(data));
        if (res.is_valid()) {
            d->pools_cache_insert(res.sequence(), res.hash(), res);
        }
    }

    if (!res.is_valid()) {
        d->set_last_error(DataIntegrityError);
    }
    else {
        d->set_last_error();
    }

    return res;
}

Pool Storage::pool_load_meta(const PoolHash& hash, size_t& cnt) const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return Pool{};
    }

    if (hash.is_empty()) {
        d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
        return Pool{};
    }

    Pool res{};
    bool found = write_queue_search(hash, res);
    if (found) {
        return res;
    }

    cs::Bytes data;
    if (!d->db->get(hash.to_binary(), &data)) {
        d->set_last_error(DatabaseError);
        return Pool{};
    }

    res = Pool::meta_from_binary(std::move(data), cnt);
    if (!res.is_valid()) {
        d->set_last_error(DataIntegrityError, "%s: Error decoding pool [hash: %s]", funcName(), hash.to_string().c_str());
    }
    else {
        d->set_last_error();
    }

    return res;
}

Pool Storage::pool_remove_last() {
    csdebug() << __func__ << ", db size: " << d->count_pool;
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return Pool{};
    }

    Pool res{};
    bool found = write_queue_pop(res);

    if (found) {
        std::unique_lock<std::mutex> lock(d->data_lock);
        --d->count_pool;
        d->last_hash = res.previous_hash();
        return res;
    }
    csinfo() << "Storage: pool not found in write queue";

    if (last_hash().is_empty()) {
        d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
        return Pool{};
    }

    cs::Bytes data;
    if (!d->db->get(last_hash().to_binary(), &data)) {
        csdebug() << "Storage: No data with such hash";
        d->set_last_error(DatabaseError);
        return Pool{};
    }

	// need full block content to discover:
	res = Pool::from_binary(std::move(data));
    if (!res.is_valid()) {
        d->set_last_error(DataIntegrityError, "%s: Error decoding pool meta [hash: %s]", funcName(), last_hash().to_string().c_str());
		return Pool{};
    }
    else {
        d->set_last_error();
    }

	// error nearly impossible
	/*bool ok =*/ d->db->remove(last_hash().to_binary());

    --d->count_pool;
    d->last_hash = res.previous_hash();

    return res;
}

bool Storage::pool_remove_last_repair(cs::Sequence test_sequence, const csdb::PoolHash& test_hash) {
    csinfo() << __func__ << ", db size: " << d->count_pool;
	if (!isOpen()) {
		d->set_last_error(NotOpen);
		return false;
	}
	if (test_sequence == 0) {
		d->set_last_error(InvalidParameter, "%s: Sequence 0 passed", funcName());
		return false;
	}
	if (test_hash.is_empty()) {
		// hash is required!
		d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
		return false;
	}

	// test sequence
	if (test_sequence + 1 != d->count_pool) {
		d->set_last_error(InvalidParameter, "%s: incorrect last sequence passed: %i, storage size: %i", funcName(), test_sequence, d->count_pool);
		return false;
	}

	// the last block may still wait in the write queue: then it is removed from there, and the blocks
	// before it, counted and on their way to the database, are kept
	bool queued = false;
	if (!write_queue_pop_last(test_hash, queued)) {
		return false;
	}
	if (queued) {
		return true;
	}

	// test hash to conform last sequence or absent at all
	uint32_t tmp;
	if (d->db->seq_no(test_hash.to_binary(), &tmp)) {
		if (tmp != test_sequence) {
			// wrong pair (sequence, hash) passed!
			d->set_last_error(InvalidParameter, "%s: incorrect last hash of %d passed", funcName(), tmp);
			return false;
		}
	}
	else {
		// there is no test_hash in seq_no table, continue with remove operation
	}

	// now we have got correct last sequence and last hash
	// TODO: remove operation fails if test_hash not found in (hash->sequence) table!
	if (!d->db->remove(test_hash.to_binary())) {
		// last error have already set
		return false;
	}

	// setup new last sequence & last hash
	--d->count_pool;
	csdb::Pool last = pool_load(test_sequence - 1);
	if (last.is_valid()) {
		d->last_hash = last.hash();
		return true;
	}
	// previous damaged block found, its hash is unknown
	d->last_hash = csdb::PoolHash{};
	d->set_last_error(DataIntegrityError, "%s: Error loading previous pool %d", funcName(), test_sequence - 1);
	return false;
}
// not used
bool Storage::pool_remove_(cs::Sequence testSequence) {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }
    if (testSequence == 0) {
        d->set_last_error(InvalidParameter, "%s: Sequence 0 passed", funcName());
        return false;
    }
    auto testHash = pool_hash(testSequence);
    if (testHash.is_empty()) {
        // hash is required!
        d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
        return false;
    }

    // test sequence
    if (testSequence + 1 != d->count_pool) {
        d->set_last_error(InvalidParameter, "%s: incorrect last sequence passed", funcName());
        return false;
    }

    // the last block may still wait in the write queue: then it is removed from there
    bool queued = false;
    if (!write_queue_pop_last(testHash, queued)) {
        return false;
    }
    if (queued) {
        return true;
    }

    // test hash to conform last sequence or absent at all
    uint32_t tmp;
    if (d->db->seq_no(testHash.to_binary(), &tmp)) {
        if (tmp != testSequence) {
            // wrong pair (sequence, hash) passed!
            d->set_last_error(InvalidParameter, "%s: incorrect last hash of %d passed", funcName(), tmp);
            return false;
        }
    }
    else {
        // there is no test_hash in seq_no table, continue with remove operation
    }

    // now we have got correct last sequence and last hash
    // TODO: remove operation fails if test_hash not found in (hash->sequence) table!
    if (!d->db->remove(testHash.to_binary())) {
        // last error have already set
        return false;
    }

    // setup new last sequence & last hash
    --d->count_pool;
    csdb::Pool last = pool_load(testSequence - 1);
    if (last.is_valid()) {
        d->last_hash = last.hash();
        return true;
    }
    // previous damaged block found, its hash is unknown
    d->last_hash = csdb::PoolHash{};
    d->set_last_error(DataIntegrityError, "%s: Error loading previous pool %d", funcName(), testSequence - 1);
    return false;
}

Wallet Storage::wallet(const Address& addr) const {
    return Wallet::get(addr);
}

const ReadBlockSignal& Storage::readBlockEvent() const {
    return d->read_block_event;
}

const BlockReadingStartedSingal& Storage::readingStartedEvent() const {
    return d->start_reading_event;
}

const BlockReadingStoppedSignal& Storage::readingStoppedEvent() const {
    return d->stop_reading_event;
}

std::vector<Transaction> Storage::transactions(const Address& addr, size_t limit, const TransactionID& offset) const {
    std::vector<Transaction> res;
    res.reserve(limit);

    Pool curPool;
    cs::Sequence curIdx = 0;

    auto seekIt = [this, &curPool, &curIdx](const TransactionID& id) -> bool {
        if (id.is_valid()) {
            curPool = pool_load(id.pool_seq());
            if (curPool.is_valid() && id.index() < curPool.transactions_count()) {
                curIdx = id.index();
                return true;
            }
        }
        return false;
    };

    auto nextIt = [this, &curPool, &curIdx]() -> bool {
        if (curPool.is_valid()) {
            if (curIdx) {
                curIdx--;
                return true;
            }
            else {
                do {
                    curPool = pool_load(curPool.previous_hash());
                } while (curPool.is_valid() && !(curPool.transactions_count()));
                if (curPool.is_valid()) {
                    curIdx = static_cast<cs::Sequence>(curPool.transactions_count() - 1);
                    return true;
                }
            }
        }
        else {
            curPool = pool_load(last_hash());
            while (curPool.is_valid() && !(curPool.transactions_count())) {
                curPool = pool_load(curPool.previous_hash());
            }
            if (curPool.is_valid()) {
                curIdx = static_cast<cs::Sequence>(curPool.transactions_count() - 1);
                return true;
            }
        }
        return false;
    };

    if (offset.is_valid())
        if (!seekIt(offset))
            return res;

    while (res.size() < limit && nextIt()) {
        const Transaction t = curPool.transaction(curIdx);
        if ((t.source() == addr) || (t.target() == addr))
            res.push_back(t);
    }

    return res;
}

Transaction Storage::transaction(const TransactionID& id) const {
    if (!id.is_valid()) {
        d->set_last_error(InvalidParameter, "%s: Transaction id is not valid", __func__);
        return Transaction{};
    }

    return pool_load(id.pool_seq()).transaction(id);
}

Transaction Storage::get_last_by_source(Address source) const noexcept {
    Pool curr = pool_load(last_hash());

    while (curr.is_valid()) {
        const auto& t = curr.get_last_by_source(source);
        if (t.is_valid())
            return t;

        curr = pool_load(curr.previous_hash());
    }

    return Transaction{};
}

Transaction Storage::get_last_by_target(Address target) const noexcept {
    Pool curr = pool_load(last_hash());

    while (curr.is_valid()) {
        const auto& t = curr.get_last_by_target(target);
        if (t.is_valid()) {
            return t;
        }

        curr = pool_load(curr.previous_hash());
    }

    return Transaction{};
}

bool Storage::get_contract_data(const Address& abs_addr /*input*/, cs::Bytes& data /*output*/) const {
    if (!d->db) {
        return false;
    }
    const auto& pk = abs_addr.public_key();
    cs::Bytes bytes(pk.size());
    bytes.assign(pk.cbegin(), pk.cend());
    return d->db->getContractData(bytes, data);
}

bool Storage::update_contract_data(const Address& abs_addr /*input*/, const cs::Bytes& data /*input*/) const {
    if (!d->db) {
        return false;
    }
    const auto& pk = abs_addr.public_key();
    cs::Bytes bytes(pk.size());
    bytes.assign(pk.cbegin(), pk.cend());
    return d->db->updateContractData(bytes, data);
}

bool Storage::pool_prune(const PoolHash& hash) {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }
    // the pool cache is left alone: it is not locked, and an old pool it may still hold stays valid
    if (!d->db->remove(hash.to_binary())) {
        d->set_last_error(DatabaseError, "%s: cannot remove pool %s", funcName(), hash.to_string().c_str());
        return false;
    }
    return true;
}

bool Storage::pool_prune_batch(const std::vector<PoolHash>& hashes) {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }
    std::vector<cs::Bytes> keys;
    keys.reserve(hashes.size());
    for (const auto& hash : hashes) {
        keys.push_back(hash.to_binary());
    }
    // the pool cache is left alone, as in pool_prune
    if (!d->db->remove_batch(keys)) {
        d->set_last_error(DatabaseError, "%s: %s", funcName(), d->db->last_error_message().c_str());
        return false;
    }
    return true;
}

bool Storage::pool_bytes(cs::Sequence sequence, cs::Bytes& out) const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }
    return d->db->get(static_cast<uint32_t>(sequence), &out);
}

cs::Sequence Storage::first_sequence() const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return std::numeric_limits<cs::Sequence>::max();
    }
    auto it = d->db->new_iterator();
    it->seek_to_first();
    return it->is_valid() ? it->key() : std::numeric_limits<cs::Sequence>::max();
}

bool Storage::compact() {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return false;
    }
    if (!d->db->compact()) {
        // not a fault: compaction only returns space, and freed pages are reused for new blocks anyway
        cswarning() << "Storage> compaction skipped: " << d->db->last_error_message();
        return false;
    }
    return true;
}

cs::Sequence Storage::pool_sequence(const PoolHash& hash) const {
    cs::Sequence seq = std::numeric_limits<cs::Sequence>::max();
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return seq;
    }

    if (hash.is_empty()) {
        d->set_last_error(InvalidParameter, "%s: Empty hash passed", funcName());
        return seq;
    }

    uint32_t tmp;
    if (d->db->seq_no(hash.to_binary(), &tmp)) {
        seq = tmp;
    }
    return seq;
}

csdb::PoolHash Storage::pool_hash(cs::Sequence sequence) const {
    if (!isOpen()) {
        d->set_last_error(NotOpen);
        return PoolHash{};
    }

    Pool res;
    cs::Bytes data;

    if (!d->db->get(static_cast<uint32_t>(sequence), &data)) {
        {
            std::unique_lock<std::mutex> lock2(d->write_lock);
            if (d->find_queued([&](const Pool& p) { return p.sequence() == sequence; }, res)) {
                d->set_last_error();
                return res.hash();
            }
        }

        if (!d->db->get(static_cast<uint32_t>(sequence), &data)) {
            d->set_last_error(DatabaseError);
            return PoolHash{};
        }
    }

    res = Pool::from_binary(std::move(data));
    if (!res.is_valid()) {
        d->set_last_error(DataIntegrityError);
        return PoolHash{};
    }
    else {
        d->set_last_error();
    }

    return res.hash();
}

}  // namespace csdb
