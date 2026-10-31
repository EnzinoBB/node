#ifndef TRANSACTIONSINDEX_HPP
#define TRANSACTIONSINDEX_HPP

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <csdb/address.hpp>
#include <lib/system/common.hpp>
#include <lmdb.hpp>

class BlockChain;

namespace csdb {
class Pool;
} // namespace csdb

namespace cs {

class TransactionsIndex {
public:
    TransactionsIndex(BlockChain&, const std::string& _path, bool _recreate = false);
    ~TransactionsIndex() = default;

    void update(const csdb::Pool&);
    void invalidate();
    void close();
    void flush();

    void pinFloor(Sequence floor);

    // Delete trxIndex entries with curr_seq > floor; rewrite prev_seq>floor at curr==floor to kWrongSequence.
    void trimToFloor(Sequence floor);

    bool recreate() const;

    // false while pinFloor lifted past unwalked blocks; cleared by a slow-start walk.
    bool isReady() const;

    // true when lastIndexedPool_ claims a populated index but the LMDB is effectively empty.
    bool looksEmpty() const;

    // wipe lastIndexedPool_ and arm recreate_ so the next slow-start fully repopulates.
    void forceRebuild();

    // a rebuild without a slow start: after a quick start the index is rebuilt from the blocks the
    // node stores. deferRebuild() makes the start-up read of the database leave the index alone;
    // rebuildFrom() then walks first..last and marks the index complete. It returns false when
    // stopRequested() stopped it: the index stays marked incomplete and is rebuilt at the next start.
    void deferRebuild();
    bool rebuildDeferred() const;
    bool rebuildFrom(Sequence first, Sequence last, const std::function<csdb::Pool(Sequence)>& load,
                     const std::function<bool()>& stopRequested);

    Sequence getPrevTransBlock(const csdb::Address& _addr, Sequence _curr) const;

    // pruned storage: forget an old block's entries; a walk back through an address's history
    // then ends at the oldest block still stored (getPrevTransBlock answers kWrongSequence)
    void onPruneBlock(const csdb::Pool&);

    // Backward sync (docs/specs/2026-10-17-backward-sync-design.md, section 6): indexes a block below
    // every block indexed so far, before it is stored, so that a wallet's history grows downwards
    // while the node runs. The block is linked under each of its wallets' lowest indexed block (the
    // tail, kept per wallet in a record 't' + public key); a pointer the forward index wrote above
    // the tail is never changed and no entry ever points to itself. Idempotent: a block may be
    // indexed again after a crash. Then the floor (key __backfill_floor__) is the block.
    void indexBackfilledBlock(const csdb::Pool&);
    // the lowest block the backfill indexed, kWrongSequence if this index never took one
    Sequence backfillFloor() const;
    // at start: indexes again the stored blocks floor - 1 down to first, which a crash left stored
    // but not indexed (first < floor); returns how many
    size_t repairBackfillFloor(Sequence first, const std::function<csdb::Pool(Sequence)>& load);

    // what the backward indexing reads from the chain: a wallet's last transaction block (the
    // wallet cache's, kWrongSequence if none) and the first stored block. BlockChain's by default;
    // tests set their own
    struct ChainView {
        std::function<Sequence(const csdb::Address&)> lastTransactionBlock;
        std::function<Sequence()> firstStored;
    };
    void setChainView(ChainView view);

public slots:
    void onStartReadFromDb(Sequence _lastWrittenPoolSeq);
    void onReadFromDb(const csdb::Pool&);
    void onDbReadFinished();
    void onRemoveBlock(const csdb::Pool&);

private slots:
    void onDbFailed(const LmdbException&);

private:
    void init();
    void reset();

    void updateFromNextBlock(const csdb::Pool&);
    void updateLastIndexed();

    // Reads last_indexed value from the index LMDB. Returns true if a valid
    // value was loaded into lastIndexedPool_, false if missing or sentinel.
    bool loadLastIndexedFromDb();
    // the watermark of the old last_indexed file (earlier builds, the official node): true if a
    // valid value was loaded into lastIndexedPool_
    bool loadLastIndexedFromLegacyFile();
    // true if the index holds at least one entry (not only its watermark and flags)
    bool hasIndexEntries() const;

    void setPrevTransBlock(const PublicKey&, cs::Sequence _curr, cs::Sequence _prev);
    void removeLastTransBlock(const PublicKey&, cs::Sequence _curr);

    // the value of the entry (key, seq), nullopt if there is none
    std::optional<Sequence> prevTransBlockOf(const PublicKey&, Sequence) const;
    // backward sync: the lowest indexed block of a wallet, kWrongSequence if it has none
    Sequence tailOf(const csdb::Address& key);
    Sequence walkToTail(const csdb::Address& key);
    Sequence lastTransactionBlock(const csdb::Address& key) const;
    Sequence firstStored() const;

    void loadIncompleteFromDb();
    void updateIncompleteFlag();
    void loadCompleteFlagFromDb();
    void updateCompleteFlag();

    BlockChain& bc_;
    const std::string rootPath_;
    std::unique_ptr<Lmdb> db_;
    Sequence lastIndexedPool_ = 0;
    bool recreate_ = false;
    bool indexIncomplete_ = false;
    bool indexComplete_ = false;
    bool rebuildDeferred_ = false;

    std::map<csdb::Address, cs::Sequence> lapoos_;
    ChainView chainView_;
};
} // namespace cs
#endif // TRANSACTIONSINDEX_HPP
