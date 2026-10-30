#include <transactionsindex.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <csdb/internal/utils.hpp>
#include <csdb/pool.hpp>
#include <csdb/transaction.hpp>
#include <lib/system/logger.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/transactionsiterator.hpp>

namespace {
constexpr const char* kDbPath = "/indexdb";
// Reserved keys; lengths can't collide with the 40-byte (pubkey + sequence) entries.
constexpr const char* kLastIndexedKey   = "__last_indexed__";
constexpr const char* kIncompleteKey    = "__incomplete__";
constexpr const char* kCompleteKey      = "__walked_complete__";
constexpr const char* kSchemaVersionKey = "__schema_version__";
// backward sync: the lowest block indexed downwards
constexpr const char* kBackfillFloorKey = "__backfill_floor__";
constexpr uint32_t    kCurrentSchema    = 2;  // bumped when trimToFloor was added
constexpr size_t      kTrimBatchSize    = 50000;
constexpr size_t      kTrxIndexKeySize  = 40;  // 32 (pubkey) + 8 (seq)

auto getTrxIndexKey(const cs::PublicKey& _pubKey, cs::Sequence _seq) {
    cs::Bytes ret(_pubKey.begin(), _pubKey.end());
    ret.resize(ret.size() + sizeof(_seq));

    auto ptr = reinterpret_cast<uint8_t*>(&_seq);
    std::copy(ptr, ptr + sizeof(_seq), ret.begin() + _pubKey.size());
    return ret;
}

// backward sync: the lowest indexed block of a wallet, 't' + public key (33 bytes, never the length
// of an entry, so the scans that look at entries only skip it)
cs::Bytes getTailKey(const cs::PublicKey& _pubKey) {
    cs::Bytes ret;
    ret.reserve(1 + _pubKey.size());
    ret.push_back('t');
    ret.insert(ret.end(), _pubKey.begin(), _pubKey.end());
    return ret;
}
} // namespace

namespace cs {

TransactionsIndex::TransactionsIndex(BlockChain& _bc, const std::string& _path, bool _recreate)
    : bc_(_bc),
      rootPath_(_path),
      db_(std::make_unique<Lmdb>(_path + kDbPath)) {
    // last_indexed lives inside the same LMDB as the index entries (replaces
    // the old mmap'd /last_indexed file, which was not crash-safe and went
    // stale even on clean shutdown — the OS lazily flushed Boost mapped_file
    // pages, so on restart its value lagged the LMDB and forced a full reindex).
    init();
    if (_recreate) {
        recreate_ = true;
    }
    else if (!loadLastIndexedFromDb()) {
        // an index written by an earlier build, or by the official node, keeps its watermark in the
        // old last_indexed file: take it over instead of rebuilding the index (and slow-starting).
        // pinFloor() marks the index incomplete if that watermark lags the quick-start checkpoint.
        // Not when this index has a watermark of its own that invalidate() cleared: it was found
        // inconsistent and must be rebuilt, whatever the old file says.
        const bool invalidated = db_->isOpen() && db_->isKeyExists(kLastIndexedKey);
        if (!invalidated && loadLastIndexedFromLegacyFile()) {
            cslog() << "trxIndex: last indexed block " << lastIndexedPool_
                    << " taken over from the legacy last_indexed file";
            updateLastIndexed();
        }
        else {
            recreate_ = true;
        }
    }
    loadIncompleteFromDb();
    loadCompleteFlagFromDb();
    if (db_->isOpen() && !db_->isKeyExists(kSchemaVersionKey)) {
        db_->insert(kSchemaVersionKey, kCurrentSchema);
    }
    if (indexIncomplete_) {
        cslog() << "TRACE: trxIndex __incomplete__ flag set on load — Trusted role gated until the index is rebuilt at this start";
    }
    if (!indexComplete_ && !recreate_ && lastIndexedPool_ >= 1000) {
        cslog() << "TRACE: trxIndex has no completion flag (an earlier build, or trimmed after an unclean stop); it is kept";
    }
}

bool TransactionsIndex::recreate() const {
    return recreate_;
}

bool TransactionsIndex::isReady() const {
    if (indexIncomplete_) return false;
    if (lastIndexedPool_ == kWrongSequence) return false;
    return true;
}

bool TransactionsIndex::looksEmpty() const {
    if (!db_->isOpen()) return false;
    return lastIndexedPool_ == kWrongSequence;
}

void TransactionsIndex::forceRebuild() {
    lastIndexedPool_ = kWrongSequence;
    recreate_ = true;
    if (indexIncomplete_) {
        indexIncomplete_ = false;
        updateIncompleteFlag();
    }
    if (indexComplete_) {
        indexComplete_ = false;
        updateCompleteFlag();
    }
    updateLastIndexed();
}

void TransactionsIndex::onStartReadFromDb(Sequence _lastWrittenPoolSeq) {
    if (recreate_ || rebuildDeferred_) {
        return;
    }
    if (lastIndexedPool_ == kWrongSequence) {
        return;
    }
    if (lastIndexedPool_ != _lastWrittenPoolSeq) {
        cslog() << "TRACE: trxIndex onStartReadFromDb lastIndexedPool=" << lastIndexedPool_
                << " lastWrittenPoolSeq=" << _lastWrittenPoolSeq
                << " gap=" << (static_cast<long long>(lastIndexedPool_)
                               - static_cast<long long>(_lastWrittenPoolSeq));
    }
    if (lastIndexedPool_ <= _lastWrittenPoolSeq) {
        return;
    }
    // trxIndex ahead of chain head; trim entries above the floor.
    trimToFloor(_lastWrittenPoolSeq);
}

void TransactionsIndex::onReadFromDb(const csdb::Pool& _pool) {
    if (rebuildDeferred_) {
        return;
    }
    if (_pool.sequence() == 0 && recreate_) {
        reset();
        init();
    }

    if (recreate_ || lastIndexedPool_ < _pool.sequence()) {
        updateFromNextBlock(_pool);
    }
}

void TransactionsIndex::onDbReadFinished() {
    if (rebuildDeferred_) {
        return;
    }
    if (recreate_) {
        recreate_ = false;
        if (indexIncomplete_) {
            indexIncomplete_ = false;
            updateIncompleteFlag();
            cslog() << "trxIndex __incomplete__ cleared by slow-start — Trusted role unblocked";
        }
        if (!indexComplete_) {
            indexComplete_ = true;
            updateCompleteFlag();
            cslog() << "trxIndex marked complete by slow-start walk";
        }
        lapoos_.clear();
        cslog() << "Recreated index 0 -> " << lastIndexedPool_
                << ". Continue to keep it actual from new blocks.";
    }
    else {
        updateLastIndexed();
    }
}

void TransactionsIndex::onRemoveBlock(const csdb::Pool& _pool) {
    std::set<csdb::Address> uniqueAddresses;
    std::vector<std::pair<cs::PublicKey, csdb::TransactionID>> updates;

    auto lbd = [&_pool, &updates, &uniqueAddresses, this](const csdb::Address& _addr, cs::Sequence _sq) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);

        if (uniqueAddresses.insert(key).second) {
            auto it = TransactionsIterator(bc_, _addr, _pool);
            bool found = false;

            for (; it.isValid(); it.next()) {
                if (it->id().pool_seq() < _sq) {
                    updates.push_back(std::make_pair(key.public_key(), it->id()));
                    found = true;
                    break;
                }
            }
            if (!found) {
                updates.push_back(std::make_pair(key.public_key(),
                                                 csdb::TransactionID(kWrongSequence, kWrongSequence)));
            }
            removeLastTransBlock(key.public_key(), _sq);
        }
    };

    for (const auto& t : _pool.transactions()) {
        lbd(t.source(), lastIndexedPool_);
        lbd(t.target(), lastIndexedPool_);
    }
    --lastIndexedPool_;
    updateLastIndexed();

    if (updates.size()) {
        bc_.updateLastTransactions(updates);
    }
}

void TransactionsIndex::update(const csdb::Pool& _pool) {
    updateFromNextBlock(_pool);
}

void TransactionsIndex::invalidate() {
    lastIndexedPool_ = kWrongSequence;
    updateLastIndexed();
}

void TransactionsIndex::close() {
    if (db_->isOpen()) {
        db_->flush();
        db_->close();
    }
}

void TransactionsIndex::flush() {
    if (db_->isOpen()) {
        db_->flush();
    }
}

void TransactionsIndex::deferRebuild() {
    rebuildDeferred_ = true;
}

bool TransactionsIndex::rebuildDeferred() const {
    return rebuildDeferred_;
}

bool TransactionsIndex::rebuildFrom(Sequence first, Sequence last, const std::function<csdb::Pool(Sequence)>& load,
                                    const std::function<bool()>& stopRequested) {
    reset();
    init();
    if (db_->isOpen()) {
        db_->insert(kSchemaVersionKey, kCurrentSchema);
    }
    // the walk of a slow start, over the stored blocks only: an address's oldest entry then names
    // no previous block, as the entries pruning leaves behind
    recreate_ = true;
    lapoos_.clear();
    lastIndexedPool_ = kWrongSequence;
    // the walk writes the watermark block by block: until it ends the index is marked incomplete, so
    // that a stop or a kill in the middle makes the next start rebuild it again
    indexIncomplete_ = true;
    updateIncompleteFlag();
    size_t walked = 0;
    size_t missing = 0;
    constexpr Sequence kProgressEvery = 1'000'000;
    cslog() << "trxIndex: rebuilding from the stored blocks " << first << ".." << last;
    for (Sequence seq = first; first <= last && seq <= last; ++seq) {
        if (seq % kProgressEvery == 0 && seq != first) {
            if (stopRequested && stopRequested()) {
                // the index stays marked incomplete: the next start rebuilds it again
                recreate_ = false;
                rebuildDeferred_ = false;
                lapoos_.clear();
                db_->flush();
                cslog() << "trxIndex: rebuild stopped at block " << seq;
                return false;
            }
            cslog() << "trxIndex: rebuilt up to block " << seq << " of " << last;
        }
        const csdb::Pool pool = load(seq);
        if (!pool.is_valid()) {
            ++missing;
            continue;
        }
        updateFromNextBlock(pool);
        ++walked;
    }
    recreate_ = false;
    rebuildDeferred_ = false;
    lapoos_.clear();
    lastIndexedPool_ = last;
    indexIncomplete_ = false;
    updateIncompleteFlag();
    indexComplete_ = true;
    updateCompleteFlag();
    updateLastIndexed();
    db_->flush();
    if (missing > 0) {
        cswarning() << "trxIndex: " << missing << " blocks of " << first << ".." << last << " could not be read";
    }
    cslog() << "trxIndex rebuilt from the stored blocks " << first << ".." << last << " (" << walked << " blocks)";
    return true;
}

void TransactionsIndex::pinFloor(Sequence floor) {
    if (floor == kWrongSequence) {
        return;
    }
    if (lastIndexedPool_ == kWrongSequence || lastIndexedPool_ < floor) {
        const Sequence prev = (lastIndexedPool_ == kWrongSequence) ? 0 : lastIndexedPool_;
        cslog() << "TRACE: trxIndex pinFloor lifting lastIndexedPool from "
                << lastIndexedPool_ << " to " << floor;
        // gap left unwalked → consensus would propose a divergent trx set. gate Trusted.
        if (recreate_ || prev + 1 < floor) {
            indexIncomplete_ = true;
            cslog() << "TRACE: trxIndex marked __incomplete__ — Trusted role gated until the next start rebuilds it";
            updateIncompleteFlag();
        }
        lastIndexedPool_ = floor;
        recreate_ = false;
        updateLastIndexed();
        if (db_->isOpen()) {
            db_->flush();
        }
    }
}

void TransactionsIndex::trimToFloor(Sequence floor) {
    if (!db_->isOpen()) return;
    if (floor == kWrongSequence) return;
    if (lastIndexedPool_ != kWrongSequence && lastIndexedPool_ <= floor) return;

    cslog() << "trxIndex: trimToFloor " << lastIndexedPool_ << " -> " << floor
            << " (deleting entries with curr_seq > floor)";

    auto& env = db_->env();
    size_t deleted = 0;
    size_t rewrittenAtFloor = 0;

    // transactions on env() directly, outside cs::Lmdb's lock against a growth of the map: this
    // runs only at start-up (--set-bc-top, or an index ahead of the chain), before any other thread
    // reads the index
    // Phase 1: collect keys to delete (curr_seq > floor) and keys at floor with prev_seq > floor.
    std::vector<std::vector<uint8_t>> toDelete;
    std::vector<std::vector<uint8_t>> toRewrite;
    try {
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        auto cur = lmdb::cursor::open(txn, dbi);
        lmdb::val k, v;
        if (cur.get(k, v, MDB_FIRST)) {
            do {
                if (k.size() != kTrxIndexKeySize) continue;
                Sequence seq = 0;
                std::memcpy(&seq, reinterpret_cast<const uint8_t*>(k.data()) + 32, sizeof(Sequence));
                if (seq > floor) {
                    toDelete.emplace_back(reinterpret_cast<const uint8_t*>(k.data()),
                                          reinterpret_cast<const uint8_t*>(k.data()) + k.size());
                } else if (seq == floor) {
                    Sequence prev = 0;
                    try { prev = std::stoull(std::string(static_cast<const char*>(v.data()), v.size())); }
                    catch (...) { continue; }
                    if (prev != kWrongSequence && prev > floor) {
                        toRewrite.emplace_back(reinterpret_cast<const uint8_t*>(k.data()),
                                               reinterpret_cast<const uint8_t*>(k.data()) + k.size());
                    }
                }
            } while (cur.get(k, v, MDB_NEXT));
        }
        cur.close();
        txn.abort();
    } catch (const std::exception& e) {
        cserror() << "trxIndex: trimToFloor scan failed: " << e.what();
        return;
    }

    cslog() << "trxIndex: trimToFloor will delete " << toDelete.size()
            << " entries, rewrite " << toRewrite.size() << " prev_seq pointers";

    // Phase 2: batched deletes.
    for (size_t i = 0; i < toDelete.size(); i += kTrimBatchSize) {
        const size_t end = std::min(i + kTrimBatchSize, toDelete.size());
        try {
            auto txn = lmdb::txn::begin(env);
            auto dbi = lmdb::dbi::open(txn, nullptr);
            for (size_t j = i; j < end; ++j) {
                lmdb::val key(toDelete[j].data(), toDelete[j].size());
                dbi.del(txn, key);
                ++deleted;
            }
            txn.commit();
        } catch (const std::exception& e) {
            cserror() << "trxIndex: trimToFloor delete batch failed at " << i << ": " << e.what();
            return;
        }
    }

    // Phase 3: rewrite boundary prev_seq to kWrongSequence (use ASCII via wrapper).
    for (const auto& key : toRewrite) {
        try {
            db_->insert(key, kWrongSequence);
            ++rewrittenAtFloor;
        } catch (const std::exception& e) {
            cswarning() << "trxIndex: trimToFloor rewrite failed: " << e.what();
        }
    }

    lastIndexedPool_ = floor;
    recreate_ = false;
    if (indexComplete_) {
        indexComplete_ = false;
        updateCompleteFlag();
    }
    if (indexIncomplete_) {
        indexIncomplete_ = false;
        updateIncompleteFlag();
    }
    updateLastIndexed();
    db_->flush();

    cslog() << "trxIndex: trimToFloor done, deleted=" << deleted
            << " rewritten=" << rewrittenAtFloor << " lastIndexedPool_=" << lastIndexedPool_;
}

void TransactionsIndex::updateFromNextBlock(const csdb::Pool& _pool) {
    std::set<csdb::Address> indexedAddrs;

    auto lbd = [&indexedAddrs, &_pool, this](const csdb::Address& _addr) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);

        if (indexedAddrs.insert(key).second) {
            Sequence lapoo;
            if (recreate_) {
                auto it = lapoos_.find(key);
                if (it == lapoos_.end()) {
                    lapoo = cs::kWrongSequence;
                }
                else {
                    lapoo = it->second;
                }
                lapoos_[key] = _pool.sequence();
            }
            else {
                lapoo = bc_.getLastTransaction(key).pool_seq();
            }

            if (lapoo != _pool.sequence()) {
                setPrevTransBlock(key.public_key(), _pool.sequence(), lapoo);
            }
            else {
                cserror() << "Attempt to make trx index inconsistent, curr pool num is "
                          << _pool.sequence() << ", prev pool num is " << lapoo
                          << ". For public key: "
                          << EncodeBase58(key.public_key().data(), key.public_key().data() + key.public_key().size())
                          << ", recreate status is " << recreate_;
            }
        }
    };

    for (auto& tr : _pool.transactions()) {
        lbd(tr.source());
        lbd(tr.target());
    }

    lastIndexedPool_ = _pool.sequence();
    updateLastIndexed();
}

void TransactionsIndex::setPrevTransBlock(const PublicKey& _pubKey, cs::Sequence _curr, cs::Sequence _prev) {
    db_->insert(getTrxIndexKey(_pubKey, _curr), _prev);
}

void TransactionsIndex::onPruneBlock(const csdb::Pool& _pool) {
    std::set<csdb::Address> removed;
    auto lbd = [&removed, &_pool, this](const csdb::Address& _addr) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);
        if (removed.insert(key).second) {
            removeLastTransBlock(key.public_key(), _pool.sequence());
        }
    };
    for (auto& tr : _pool.transactions()) {
        lbd(tr.source());
        lbd(tr.target());
    }
}

void TransactionsIndex::removeLastTransBlock(const PublicKey& _pubKey, cs::Sequence _curr) {
    db_->remove(getTrxIndexKey(_pubKey, _curr));
}

std::optional<Sequence> TransactionsIndex::prevTransBlockOf(const PublicKey& _pubKey, Sequence _seq) const {
    if (!db_->isOpen()) {
        return std::nullopt;
    }
    // values are decimal strings, never empty: an empty one is a missing key, in one read
    const auto value = db_->value<std::string>(getTrxIndexKey(_pubKey, _seq));
    if (value.empty()) {
        return std::nullopt;
    }
    return Lmdb::convert<Sequence>(value.data(), value.size());
}

void TransactionsIndex::setChainView(ChainView view) {
    chainView_ = std::move(view);
}

Sequence TransactionsIndex::lastTransactionBlock(const csdb::Address& key) const {
    if (chainView_.lastTransactionBlock) {
        return chainView_.lastTransactionBlock(key);
    }
    return bc_.getLastTransaction(key).pool_seq();
}

Sequence TransactionsIndex::firstStored() const {
    return chainView_.firstStored ? chainView_.firstStored() : bc_.getFirstStoredSequence();
}

Sequence TransactionsIndex::backfillFloor() const {
    if (!db_->isOpen() || !db_->isKeyExists(kBackfillFloorKey)) {
        return kWrongSequence;
    }
    return db_->value<Sequence>(kBackfillFloorKey);
}

// The tail record names the wallet's lowest indexed block if that entry still exists (pruning
// removes entries and leaves records behind). Otherwise the wallet's chain is walked down from its
// last transaction, as TransactionsIterator does but without loading blocks, to the entry whose
// previous block is none or not stored.
Sequence TransactionsIndex::tailOf(const csdb::Address& key) {
    const auto record = db_->value<std::string>(getTailKey(key.public_key()));
    if (!record.empty()) {
        const Sequence tail = Lmdb::convert<Sequence>(record.data(), record.size());
        if (prevTransBlockOf(key.public_key(), tail)) {
            return tail;
        }
    }
    return walkToTail(key);
}

Sequence TransactionsIndex::walkToTail(const csdb::Address& key) {
    const auto& pubKey = key.public_key();
    Sequence cur = lastTransactionBlock(key);
    if (cur == kWrongSequence) {
        return kWrongSequence;
    }
    auto prev = prevTransBlockOf(pubKey, cur);
    if (!prev) {
        // no entry for the last transaction: all of the wallet's blocks lie below the stored ones
        return kWrongSequence;
    }
    const Sequence first = firstStored();
    size_t steps = 0;
    // prev < cur: an entry pointing up or to itself ends the walk instead of looping
    while (*prev != kWrongSequence && *prev < cur && *prev >= first) {
        const auto next = prevTransBlockOf(pubKey, *prev);
        if (!next) {
            break;
        }
        cur = *prev;
        prev = next;
        if (++steps % 100'000 == 0) {
            // a busy wallet after an index rebuild: once, one read per stored block of it
            cslog() << "trxIndex: backfill: walking the history of "
                    << EncodeBase58(pubKey.data(), pubKey.data() + pubKey.size()) << ", " << steps << " blocks so far";
        }
    }
    csdebug() << "trxIndex: backfill: tail of " << EncodeBase58(pubKey.data(), pubKey.data() + pubKey.size())
              << " is block " << cur << ", " << steps << " steps";
    return cur;
}

void TransactionsIndex::indexBackfilledBlock(const csdb::Pool& _pool) {
    if (!db_->isOpen()) {
        return;
    }
    const Sequence seq = _pool.sequence();
    std::set<csdb::Address> indexedAddrs;

    auto lbd = [&indexedAddrs, seq, this](const csdb::Address& _addr) {
        auto key = bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey);
        if (!indexedAddrs.insert(key).second) {
            return;
        }
        const auto& pubKey = key.public_key();
        const Sequence tail = tailOf(key);
        // a tail below this block: the block was indexed before (a crash), and the blocks below it too
        const bool below = tail == kWrongSequence || tail > seq;
        if (tail != kWrongSequence && tail > seq && prevTransBlockOf(pubKey, tail) != seq) {
            // the tail names a pruned block or none: it is this one (seq < tail, never itself)
            setPrevTransBlock(pubKey, tail, seq);
        }
        if (!prevTransBlockOf(pubKey, seq)) {
            // the previous block is not known yet: the next block of this wallet below sets it. An
            // entry left by a crash keeps its value
            setPrevTransBlock(pubKey, seq, kWrongSequence);
        }
        if (below) {
            db_->insert(getTailKey(pubKey), seq);
        }
    };

    for (const auto& tr : _pool.transactions()) {
        lbd(tr.source());
        lbd(tr.target());
    }
    db_->insert(kBackfillFloorKey, seq);
}

size_t TransactionsIndex::repairBackfillFloor(Sequence first, const std::function<csdb::Pool(Sequence)>& load) {
    const Sequence floor = backfillFloor();
    if (!db_->isOpen() || floor == kWrongSequence || first >= floor) {
        return 0;
    }
    size_t indexed = 0;
    size_t missing = 0;
    for (Sequence seq = floor; seq-- > first;) {
        const csdb::Pool pool = load(seq);
        if (!pool.is_valid()) {
            ++missing;
            continue;
        }
        indexBackfilledBlock(pool);
        ++indexed;
    }
    db_->flush();
    cslog() << "trxIndex: re-indexed " << indexed << " backfilled blocks " << floor - 1 << ".." << first
            << " left unindexed by a crash" << (missing > 0 ? ", " + std::to_string(missing) + " could not be read" : std::string{});
    return indexed;
}

Sequence TransactionsIndex::getPrevTransBlock(const csdb::Address& _addr, Sequence _prev) const {
    auto key = getTrxIndexKey(bc_.getAddressByType(_addr, BlockChain::AddressType::PublicKey).public_key(), _prev);
    if (db_->size() == 0 || !db_->isKeyExists(key)) {
        return kWrongSequence;
    }
    return db_->value<Sequence>(key);
}

bool TransactionsIndex::loadLastIndexedFromLegacyFile() {
    // <path>/last_indexed: one cs::Sequence, as the memory-mapped file of earlier builds wrote it
    std::ifstream f(rootPath_ + "/last_indexed", std::ios::binary);
    cs::Sequence v = kWrongSequence;
    if (!f.read(reinterpret_cast<char*>(&v), sizeof(v)) || v == kWrongSequence || v == 0) {
        return false;
    }
    if (!db_->isOpen()) {
        return false;
    }
    // the file stays when caches/indexdb is removed: its watermark holds only for the index the
    // official node left, not for an empty one
    if (!hasIndexEntries()) {
        cslog() << "trxIndex: the legacy last_indexed file names block " << v << ", but the index is empty";
        return false;
    }
    lastIndexedPool_ = v;
    return true;
}

bool TransactionsIndex::hasIndexEntries() const {
    if (!db_->isOpen()) {
        return false;
    }
    // a transaction on env() directly, outside cs::Lmdb's lock against a growth of the map: called
    // from the constructor only, before any other thread reads the index
    try {
        auto& env = db_->env();
        auto txn = lmdb::txn::begin(env, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, nullptr);
        auto cur = lmdb::cursor::open(txn, dbi);
        lmdb::val k, v;
        bool found = false;
        if (cur.get(k, v, MDB_FIRST)) {
            do {
                found = k.size() == kTrxIndexKeySize;
            } while (!found && cur.get(k, v, MDB_NEXT));
        }
        cur.close();
        txn.abort();
        return found;
    } catch (const std::exception& e) {
        cswarning() << "trxIndex: cannot read the index: " << e.what();
        return false;
    }
}

bool TransactionsIndex::loadLastIndexedFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kLastIndexedKey)) {
        return false;
    }
    cs::Sequence v = db_->value<cs::Sequence>(kLastIndexedKey);
    if (v == kWrongSequence) {
        return false;
    }
    lastIndexedPool_ = v;
    return true;
}

inline void TransactionsIndex::updateLastIndexed() {
    if (db_->isOpen()) {
        db_->insert(kLastIndexedKey, lastIndexedPool_);
    }
}

void TransactionsIndex::loadIncompleteFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kIncompleteKey)) {
        indexIncomplete_ = false;
        return;
    }
    indexIncomplete_ = (db_->value<uint8_t>(kIncompleteKey) != 0);
}

void TransactionsIndex::updateIncompleteFlag() {
    if (db_->isOpen()) {
        db_->insert(kIncompleteKey, static_cast<uint8_t>(indexIncomplete_ ? 1 : 0));
        db_->flush();
    }
}

void TransactionsIndex::loadCompleteFlagFromDb() {
    if (!db_->isOpen() || !db_->isKeyExists(kCompleteKey)) {
        indexComplete_ = false;
        return;
    }
    indexComplete_ = (db_->value<uint8_t>(kCompleteKey) != 0);
}

void TransactionsIndex::updateCompleteFlag() {
    if (db_->isOpen()) {
        db_->insert(kCompleteKey, static_cast<uint8_t>(indexComplete_ ? 1 : 0));
        db_->flush();
    }
}

void TransactionsIndex::onDbFailed(const LmdbException& e) {
    cswarning() << csfunc() << ", transactions index database exception " << e.what();
}

inline void TransactionsIndex::init() {
    Connector::connect(&db_->failed, this, &TransactionsIndex::onDbFailed);

    db_->setMapSize(cs::Lmdb::Default1GbMapSize);
    db_->open();
}

inline void TransactionsIndex::reset() {
    Connector::disconnect(&db_->failed, this, &TransactionsIndex::onDbFailed);
    db_->close();
    db_.reset(nullptr);
    csdb::internal::path_remove(rootPath_ + kDbPath);
    db_ = std::make_unique<Lmdb>(rootPath_ + kDbPath);
}
} // namespace cs
