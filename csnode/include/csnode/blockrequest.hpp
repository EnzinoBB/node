#ifndef BLOCKREQUEST_HPP
#define BLOCKREQUEST_HPP

#include <lib/system/common.hpp>

namespace cs {

// What a node answers to a sync request whose first block is `first`, holding the blocks
// firstStored..last: it serves the request only when it has that first block. A pruned node must not
// answer AwaitAnswer for blocks it no longer stores and then send nothing: with special order 35 the
// requester keeps such a neighbour busy until its sync ends.
enum class BlockRequestAnswer {
    Serve,
    NoSuchBlocks,
};

inline BlockRequestAnswer answerBlockRequest(Sequence first, Sequence firstStored, Sequence last) {
    if (first > last || first < firstStored) {
        return BlockRequestAnswer::NoSuchBlocks;
    }
    return BlockRequestAnswer::Serve;
}

}  // namespace cs

#endif  // BLOCKREQUEST_HPP
