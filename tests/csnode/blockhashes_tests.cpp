#include "gtest/gtest.h"

#include <filesystem>
#include <string>
#include <vector>

#include <csdb/pool.hpp>
#include <csnode/blockhashes.hpp>

namespace fs = std::filesystem;

namespace {
fs::path freshDir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("cs_blockhashes_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// a block at `seq`; `variant` changes its content, hence its hash, as more signatures do
csdb::Pool block(cs::Sequence seq, const std::string& variant = {}) {
    csdb::Pool p(csdb::PoolHash{}, seq);
    p.add_user_field(1, "block " + std::to_string(seq) + variant);
    EXPECT_TRUE(p.compose());
    return p;
}
}  // namespace

// The deferred block is replaced by the same block with more signatures (another hash): the old hash
// must not stay in hashdb, where it would accumulate and look like a damaged database.
TEST(BlockHashes, ReplacingABlockForgetsItsOldHash) {
    auto dir = freshDir("replace");
    cs::BlockHashes hashes(dir.string());
    for (cs::Sequence s = 0; s < 10; ++s) {
        hashes.update(block(s));
    }
    const auto first = block(9);
    const auto second = block(9, " with more signatures");
    ASSERT_NE(first.hash(), second.hash());
    hashes.update(second);

    EXPECT_EQ(hashes.find(9), second.hash());
    EXPECT_EQ(hashes.find(second.hash()), 9u);
    EXPECT_EQ(hashes.find(first.hash()), cs::kWrongSequence) << "the replaced hash stayed in hashdb";
    EXPECT_TRUE(hashes.consistentWith(0, 9, [](cs::Sequence s) { return s == 9 ? block(9, " with more signatures").hash() : block(s).hash(); }));
    hashes.close();
}

// hashdb of an earlier build keeps the old hashes of replaced blocks: that surplus is harmless and must
// not make every start rebuild the databases (hours on a full node).
TEST(BlockHashes, StaleHashesOfReplacedBlocksDoNotRequireARebuild) {
    auto dir = freshDir("stale");
    {
        cs::BlockHashes hashes(dir.string());
        for (cs::Sequence s = 0; s < 1000; ++s) {
            hashes.update(block(s));
        }
        hashes.close();
    }
    {
        // what an earlier build left: 300 hash -> seq entries for blocks that were replaced since
        cs::Lmdb hashDb(dir.string() + "/hashdb");
        hashDb.setMapSize(cs::Lmdb::Default1GbMapSize);
        hashDb.open();
        for (cs::Sequence s = 0; s < 300; ++s) {
            hashDb.insert(block(s, " (replaced)").hash().to_binary(), s);
        }
        hashDb.close();
    }
    cs::BlockHashes hashes(dir.string());
    EXPECT_TRUE(hashes.consistentWith(0, 999, [](cs::Sequence s) { return block(s).hash(); }));
    hashes.close();
}

// Missing entries still count: such databases are rebuilt.
TEST(BlockHashes, MissingHashesAreInconsistent) {
    auto dir = freshDir("missing");
    cs::BlockHashes hashes(dir.string());
    for (cs::Sequence s = 0; s < 1000; ++s) {
        hashes.update(block(s));
    }
    for (cs::Sequence s = 0; s < 300; ++s) {
        hashes.remove(s);
    }
    EXPECT_FALSE(hashes.consistentWith(0, 999, [](cs::Sequence s) { return block(s).hash(); }));
    hashes.close();
}
