#include "gtest/gtest.h"

#include <csnode/blockrequest.hpp>

using cs::BlockRequestAnswer;
using cs::answerBlockRequest;

TEST(BlockRequest, AFullNodeServesEveryStoredBlock) {
    EXPECT_EQ(answerBlockRequest(0, 0, 5000), BlockRequestAnswer::Serve);
    EXPECT_EQ(answerBlockRequest(5000, 0, 5000), BlockRequestAnswer::Serve);
}

TEST(BlockRequest, NobodyServesBlocksAfterItsLastOne) {
    EXPECT_EQ(answerBlockRequest(5001, 0, 5000), BlockRequestAnswer::NoSuchBlocks);
    EXPECT_EQ(answerBlockRequest(5001, 4000, 5000), BlockRequestAnswer::NoSuchBlocks);
}

// a pruned node holding 4000..5000 has nothing for a node syncing from block 0
TEST(BlockRequest, APrunedNodeRefusesBlocksItPruned) {
    EXPECT_EQ(answerBlockRequest(0, 4000, 5000), BlockRequestAnswer::NoSuchBlocks);
    EXPECT_EQ(answerBlockRequest(3999, 4000, 5000), BlockRequestAnswer::NoSuchBlocks);
    EXPECT_EQ(answerBlockRequest(4000, 4000, 5000), BlockRequestAnswer::Serve);
}
