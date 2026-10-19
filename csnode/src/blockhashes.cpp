#include <csnode/blockhashes.hpp>

#include <conveyer.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <csdb/internal/utils.hpp>
#include <lib/system/logger.hpp>

static const char* seqPath = "/seqdb";
static const char* hashPath = "/hashdb";
static const char* rebuildMarker = "/blockhashes.rebuild";

namespace cs {
BlockHashes::BlockHashes(const std::string& path)
: path_(path) {
    initialization();
}

void BlockHashes::close() {
    if (seqDb_->isOpen()) {
        seqDb_->close();
    }

    if (hashDb_->isOpen()) {
        hashDb_->close();
    }
}

size_t BlockHashes::size() const {
    return seqDb_->size();
}

bool BlockHashes::update(const csdb::Pool& block) {
    cs::Sequence seq = block.sequence();

    auto hash = block.hash();
    auto cachedHash = find(seq);

    if (cachedHash == hash) {
        return true;
    }
    if (!cachedHash.is_empty()) {
        // the block at this sequence was replaced (e.g. the deferred block got more signatures, which
        // changes its hash): the old hash no longer names a stored block
        hashDb_->remove(cachedHash.to_binary());
    }

    seqDb_->insert(seq, hash.to_binary());
    hashDb_->insert(hash.to_binary(), seq);

    return true;
}

csdb::PoolHash BlockHashes::find(cs::Sequence seq) const {
    if (seqDb_->size() == 0 || !seqDb_->isKeyExists(seq)) {
        return csdb::PoolHash{};
    }

    auto value = seqDb_->value<cs::Bytes>(seq);
    return csdb::PoolHash::from_binary(std::move(value));
}

cs::Sequence BlockHashes::find(const csdb::PoolHash& hash) const {
    if (hash.is_empty() || hashDb_->size() == 0 || !hashDb_->isKeyExists(hash.to_binary())) {
        return cs::kWrongSequence;
    }

    return hashDb_->value<cs::Sequence>(hash.to_binary());
}

bool BlockHashes::remove(cs::Sequence sequence) {
    auto hash = find(sequence);
    if (hash.is_empty()) {
        return false;
    }
    seqDb_->remove(sequence);
    hashDb_->remove(hash.to_binary());
    return true;
}

bool BlockHashes::remove(const csdb::PoolHash& hash) {
    auto sequence = find(hash);
    if (sequence == kWrongSequence) {
        return false;
    }
    seqDb_->remove(sequence);
    hashDb_->remove(hash.to_binary());
    return true;
}

bool BlockHashes::rebuildRequested() const {
    std::error_code ec;
    return std::filesystem::exists(path_ + rebuildMarker, ec);
}

void BlockHashes::requestRebuild() {
    std::ofstream(path_ + rebuildMarker) << "the block hashes databases failed; they are rebuilt at the next start\n";
}

bool BlockHashes::consistentWith(cs::Sequence first, cs::Sequence last,
                                 const std::function<csdb::PoolHash(cs::Sequence)>& hashOf) const {
    if (first > last) {
        return true;
    }
    // a crash can leave a few blocks written here ahead of the storage, or a pruning step not
    // yet removed: tolerate that much, not more. hashdb may hold more: earlier builds left the old
    // hash of every replaced block in it, harmless since nothing maps to it any more; only missing
    // entries matter (a damaged hashdb is caught by its failures and by the samples below)
    constexpr size_t kSlack = 128;
    const size_t blocks = last - first + 1;
    const size_t seqs = seqDb_->size();
    const size_t hashes = hashDb_->size();
    if (seqs < blocks || seqs > blocks + kSlack || hashes < blocks) {
        cswarning() << "blockHashes: " << seqs << " sequences and " << hashes << " hashes for the "
                    << blocks << " stored blocks " << first << ".." << last;
        return false;
    }
    for (cs::Sequence seq : {first, first + (last - first) / 2, last}) {
        const csdb::PoolHash hash = hashOf(seq);
        if (hash.is_empty()) {
            continue;
        }
        if (find(seq) != hash || find(hash) != seq) {
            cswarning() << "blockHashes: block " << seq << " does not map to its hash both ways";
            return false;
        }
    }
    return true;
}

bool BlockHashes::rebuild(cs::Sequence first, cs::Sequence last, const std::function<csdb::Pool(cs::Sequence)>& load,
                          const std::function<bool()>& stopRequested) {
    cslog() << "blockHashes: rebuilding from the stored blocks " << first << ".." << last;
    // the marker stays until the rebuild ends, so that an interrupted one runs again
    requestRebuild();
    close();
    cs::Connector::disconnect(&seqDb_->failed, this, &BlockHashes::onDbFailed);
    cs::Connector::disconnect(&hashDb_->failed, this, &BlockHashes::onDbFailed);
    seqDb_.reset();
    hashDb_.reset();
    csdb::internal::path_remove(path_ + seqPath);
    csdb::internal::path_remove(path_ + hashPath);
    failures_ = 0;
    initialization();

    constexpr cs::Sequence kProgressEvery = 1'000'000;
    size_t written = 0;
    for (cs::Sequence seq = first; first <= last && seq <= last; ++seq) {
        if (seq % kProgressEvery == 0 && seq != first) {
            if (stopRequested && stopRequested()) {
                cslog() << "blockHashes: rebuild stopped at block " << seq << ", it runs again at the next start";
                return false;
            }
            cslog() << "blockHashes: rebuilt up to block " << seq << " of " << last;
        }
        const csdb::Pool pool = load(seq);
        if (!pool.is_valid()) {
            continue;
        }
        seqDb_->insert(seq, pool.hash().to_binary());
        hashDb_->insert(pool.hash().to_binary(), seq);
        ++written;
    }
    seqDb_->flush();
    hashDb_->flush();
    if (failures_ > 0) {
        cserror() << "blockHashes: the rebuild failed " << failures_ << " time(s), it runs again at the next start";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(path_ + rebuildMarker, ec);
    cslog() << "blockHashes rebuilt from the stored blocks " << first << ".." << last << " (" << written << " blocks)";
    return true;
}

void BlockHashes::onDbFailed(const LmdbException& exception) {
    ++failures_;
    if (failures_ == 1) {
        cserror() << csfunc() << ", block hashes database exception: " << exception.what()
                  << "; the block hashes databases are rebuilt from the stored blocks at the next start";
        requestRebuild();
    }
    else if (failures_ % 100'000 == 0) {
        cswarning() << csfunc() << ", block hashes database exceptions: " << failures_ << " so far, the last: "
                    << exception.what();
    }
}

void BlockHashes::initialization() {
    csinfo() << "blockHashes initialization started";
    seqDb_ = std::make_unique<cs::Lmdb>(path_ + seqPath);
    hashDb_ = std::make_unique<cs::Lmdb>(path_ + hashPath);
    cs::Connector::connect(&seqDb_->failed, this, &BlockHashes::onDbFailed);
    cs::Connector::connect(&hashDb_->failed, this, &BlockHashes::onDbFailed);

    seqDb_->setMapSize(cs::Lmdb::Default1GbMapSize);
    hashDb_->setMapSize(cs::Lmdb::Default1GbMapSize);

    seqDb_->open();
    csinfo() << "seqDb opened successfully";
    hashDb_->open();
    csinfo() << "hashDb opened successfully";

}
}  // namespace cs
