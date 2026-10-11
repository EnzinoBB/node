#ifndef TRANSACTIONSINDEX_HPP
#define TRANSACTIONSINDEX_HPP

#include <map>
#include <memory>
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

    Sequence getPrevTransBlock(const csdb::Address& _addr, Sequence _curr) const;

    // pruned storage: forget an old block's entries; a walk back through an address's history
    // then ends at the oldest block still stored (getPrevTransBlock answers kWrongSequence)
    void onPruneBlock(const csdb::Pool&);

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

    void setPrevTransBlock(const PublicKey&, cs::Sequence _curr, cs::Sequence _prev);
    void removeLastTransBlock(const PublicKey&, cs::Sequence _curr);

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

    std::map<csdb::Address, cs::Sequence> lapoos_;
};
} // namespace cs
#endif // TRANSACTIONSINDEX_HPP
