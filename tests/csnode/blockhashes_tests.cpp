#include "gtest/gtest.h"

#include <filesystem>
#include <functional>
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

namespace {
// the stored blocks from..to, as a load callback answers them
std::function<csdb::Pool(cs::Sequence)> storedBlocks(cs::Sequence from, cs::Sequence to) {
    return [from, to](cs::Sequence s) { return s >= from && s <= to ? block(s) : csdb::Pool{}; };
}
}  // namespace

// Backward sync wrote the hashes of blocks 10 and 11 but stopped before storing them: the first
// stored block is 12, and the entries below it go.
TEST(BlockHashes, RepairBottomRemovesEntriesBelowTheFirstStoredBlock) {
    auto dir = freshDir("bottom_below");
    cs::BlockHashes hashes(dir.string());
    for (cs::Sequence s = 10; s <= 20; ++s) {
        hashes.update(block(s));
    }
    EXPECT_EQ(hashes.repairBottom(12, 20, storedBlocks(12, 20)), 2u);
    EXPECT_TRUE(hashes.find(10).is_empty());
    EXPECT_TRUE(hashes.find(11).is_empty());
    EXPECT_EQ(hashes.find(block(11).hash()), cs::kWrongSequence);
    EXPECT_EQ(hashes.find(12), block(12).hash());
    EXPECT_EQ(hashes.size(), 9u);
    EXPECT_TRUE(hashes.consistentWith(12, 20, [](cs::Sequence s) { return block(s).hash(); }));
    hashes.close();
}

// The storage kept blocks 12 and 13, the hash databases lost them (a power cut before a flush):
// they are inserted from the storage.
TEST(BlockHashes, RepairBottomInsertsTheLowestStoredBlocks) {
    auto dir = freshDir("bottom_missing");
    cs::BlockHashes hashes(dir.string());
    for (cs::Sequence s = 14; s <= 20; ++s) {
        hashes.update(block(s));
    }
    EXPECT_EQ(hashes.repairBottom(12, 20, storedBlocks(12, 20)), 2u);
    for (cs::Sequence s = 12; s <= 13; ++s) {
        EXPECT_EQ(hashes.find(s), block(s).hash()) << s;
        EXPECT_EQ(hashes.find(block(s).hash()), s) << s;
    }
    EXPECT_TRUE(hashes.consistentWith(12, 20, [](cs::Sequence s) { return block(s).hash(); }));
    hashes.close();
}

// Nothing to do: nothing changes, and the walks stop at once.
TEST(BlockHashes, RepairBottomOfConsistentDatabasesChangesNothing) {
    auto dir = freshDir("bottom_none");
    cs::BlockHashes hashes(dir.string());
    for (cs::Sequence s = 12; s <= 20; ++s) {
        hashes.update(block(s));
    }
    size_t loads = 0;
    const auto load = [&loads](cs::Sequence s) { ++loads; return block(s); };
    EXPECT_EQ(hashes.repairBottom(12, 20, load), 0u);
    EXPECT_EQ(loads, 1u);
    EXPECT_EQ(hashes.size(), 9u);
    const auto nothingStored = [](cs::Sequence) { return csdb::Pool{}; };
    EXPECT_EQ(hashes.repairBottom(0, 0, nothingStored), 0u);
    hashes.close();
}

// Both at once, and a stale hash at the first stored block (another block of that sequence) and a
// lost hash -> sequence entry are replaced.
TEST(BlockHashes, RepairBottomMixed) {
    auto dir = freshDir("bottom_mixed");
    {
        cs::BlockHashes hashes(dir.string());
        for (cs::Sequence s : {8u, 9u, 10u, 11u}) {
            hashes.update(block(s));
        }
        hashes.update(block(12, " (another)"));
        for (cs::Sequence s = 14; s <= 20; ++s) {
            hashes.update(block(s));
        }
        hashes.close();
    }
    {
        // the hash -> sequence entry of block 14 is lost
        cs::Lmdb hashDb(dir.string() + "/hashdb");
        hashDb.setMapSize(cs::Lmdb::Default1GbMapSize);
        hashDb.open();
        hashDb.remove(block(14).hash().to_binary());
        hashDb.close();
    }
    cs::BlockHashes hashes(dir.string());
    EXPECT_EQ(hashes.repairBottom(12, 20, storedBlocks(12, 20)), 4u + 3u);
    EXPECT_TRUE(hashes.find(11).is_empty());
    EXPECT_TRUE(hashes.find(8).is_empty());
    EXPECT_EQ(hashes.find(block(12, " (another)").hash()), cs::kWrongSequence);
    for (cs::Sequence s = 12; s <= 20; ++s) {
        EXPECT_EQ(hashes.find(s), block(s).hash()) << s;
        EXPECT_EQ(hashes.find(block(s).hash()), s) << s;
    }
    EXPECT_TRUE(hashes.consistentWith(12, 20, [](cs::Sequence s) { return block(s).hash(); }));
    hashes.flush();
    hashes.close();
}
