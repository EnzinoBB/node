#ifndef BLOCKVERIFIER_HPP
#define BLOCKVERIFIER_HPP

#include <string>

#include <csdb/pool.hpp>
#include <csnode/nodecore.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Checks a block that comes from outside the consensus (a snapshot, or a block below the lowest
// stored one) before it is stored, with the rules the node applies to blocks it stores itself.
// Used by the snapshot importer and by backward sync.
class BlockVerifier {
public:
    // the block parses, has the expected sequence and, when expectedHash is not empty, the hash
    // computed from its bytes is expectedHash; for sequence >= 1 its signatures verify against its
    // own confidants with the real trusted mask (as tryFlushDeferredBlock does). Block 0 has none.
    // An empty expectedHash proves only that the block is consistent with itself (anyone can sign a
    // block with confidants of their own): the caller must anchor it, e.g. check that it links to
    // a block it already trusts (previous_hash), as storeBlock does.
    static bool verify(const csdb::Pool& block, cs::Sequence expectedSequence, const csdb::PoolHash& expectedHash, std::string& reason);

    // the pool signatures alone: outside the hashed part, so the only thing a sender could alter
    // without changing the block's hash
    static bool verifySignatures(const csdb::Pool& block, std::string& reason);

    // the round confirmations of a block (sequence >= 2) against the confidants of the block before
    // it, with finalizeBlock's exceptions: bootstrap blocks and one-entry masks are not checked.
    // They are inside the hashed part: a failure is not the sender's doing.
    static bool verifyConfirmations(const csdb::Pool& block, const cs::PublicKeys& previousConfidants, std::string& reason);
};

}  // namespace cs

#endif  // BLOCKVERIFIER_HPP
