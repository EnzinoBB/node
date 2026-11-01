#ifndef BLOCKHASHES_HPP
#define BLOCKHASHES_HPP

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <csdb/pool.hpp>
#include <lmdb.hpp>

namespace cs {
class BlockHashes {
public:
    explicit BlockHashes(const std::string& path);
    ~BlockHashes() = default;

    bool empty() const {
        return size() == 0;
    }

    size_t size() const;

    void close();
    bool onNextBlock(const csdb::Pool& block) {
        return update(block);
    }

    csdb::PoolHash find(cs::Sequence seq) const;
    cs::Sequence find(const csdb::PoolHash& hash) const;

    bool update(const csdb::Pool& block);

    bool remove(cs::Sequence);
    bool remove(const csdb::PoolHash& hash);

    // The first failure of either database is reported once and leaves the file blockhashes.rebuild
    // in the caches folder: the next start rebuilds both databases from the stored blocks.
    bool rebuildRequested() const;

    // true if the databases hold the blocks first..last: their entry counts match (within a few
    // blocks written ahead of the storage), and the first, the middle and the last block map to
    // their hash both ways
    bool consistentWith(cs::Sequence first, cs::Sequence last,
                        const std::function<csdb::PoolHash(cs::Sequence)>& hashOf) const;

    // removes both databases and writes them again from the stored blocks first..last; false when
    // stopRequested() stopped it, and the rebuild is then requested again for the next start
    bool rebuild(cs::Sequence first, cs::Sequence last, const std::function<csdb::Pool(cs::Sequence)>& load,
                 const std::function<bool()>& stopRequested);

    // writes both databases to disk
    void flush();

    // Backward sync stores blocks downwards and writes these databases before the storage: after a
    // crash they may hold entries below the first stored block, or miss the entries of the lowest
    // stored blocks. Removes the former and inserts the latter from the stored blocks first..last;
    // each walk stops at the first sequence that needs nothing. Returns how many entries changed.
    size_t repairBottom(cs::Sequence first, cs::Sequence last, const std::function<csdb::Pool(cs::Sequence)>& load);

private slots:
    void onDbFailed(const cs::LmdbException& exception);

private:
    void initialization();
    void requestRebuild();

    std::string path_;
    std::unique_ptr<cs::Lmdb> seqDb_;
    std::unique_ptr<cs::Lmdb> hashDb_;
    std::atomic<size_t> failures_{0};   // the backfill flush thread reports LMDB failures too
};
}  // namespace cs

#endif  //  BLOCKHASHES_HPP
