#include <gtest/gtest.h>

#include <csnode/caches_serialization_manager.hpp>

// quick start must not trust caches saved for a block the chain no longer has at that sequence
namespace {
const cs::Bytes kHashA{ 1, 2, 3 };
const cs::Bytes kHashB{ 4, 5, 6 };

cs::CheckpointHead head(cs::Sequence seq, const cs::Bytes& hash) {
    cs::CheckpointHead h;
    h.sequence = seq;
    h.head_hash = hash;
    return h;
}
}  // namespace

TEST(Checkpoint, AcceptedWhenHeadMatchesChain) {
    ASSERT_TRUE(cs::isCheckpointOnChain(head(100, kHashA), [](cs::Sequence) { return kHashA; }));
}

TEST(Checkpoint, RejectedWhenChainHasAnotherBlockAtHead) {
    ASSERT_FALSE(cs::isCheckpointOnChain(head(100, kHashA), [](cs::Sequence) { return kHashB; }));
}

TEST(Checkpoint, AcceptedWhenChainHashUnknown) {
    ASSERT_TRUE(cs::isCheckpointOnChain(head(100, kHashA), [](cs::Sequence) { return cs::Bytes{}; }));
}

TEST(Checkpoint, AcceptedWhenCheckpointHasNoHead) {
    ASSERT_TRUE(cs::isCheckpointOnChain(head(0, cs::Bytes{}), [](cs::Sequence) { return kHashB; }));
}
