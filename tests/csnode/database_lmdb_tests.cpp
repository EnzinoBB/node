#include "gtest/gtest.h"

#include <filesystem>
#include <memory>

#include <csdb/database_lmdb.hpp>
#include <lmdb.h>

namespace {
cs::Bytes hashOf(uint32_t seq) {
    cs::Bytes hash(32, 0);
    hash[0] = static_cast<cs::Byte>(seq);
    hash[1] = static_cast<cs::Byte>(seq >> 8);
    hash[2] = static_cast<cs::Byte>(seq >> 16);
    return hash;
}

cs::Bytes blockOf(uint32_t seq, size_t size = 3000) {
    return cs::Bytes(size, static_cast<cs::Byte>(seq));
}

std::shared_ptr<csdb::Database> openAt(const std::filesystem::path& dir) {
    auto lmdb = std::make_shared<csdb::DatabaseLmdb>();
    lmdb->open(dir.string());
    return lmdb;
}
}  // namespace

// the same contract the storage relies on with BerkeleyDB: blocks by sequence and by hash,
// removal by hash, sequence order when iterating, contract data
TEST(DatabaseLmdb, StoresFindsRemovesAndIterates) {
    const auto dir = std::filesystem::temp_directory_path() / "cs_lmdb_contract_test";
    std::filesystem::remove_all(dir);
    {
        auto db = openAt(dir);
        ASSERT_TRUE(db->is_open());
        for (uint32_t seq = 0; seq < 10; ++seq) {
            ASSERT_TRUE(db->put(hashOf(seq), seq, blockOf(seq)));
        }
        cs::Bytes value;
        ASSERT_TRUE(db->get(uint32_t(7), &value));
        ASSERT_EQ(value, blockOf(7));
        ASSERT_TRUE(db->get(hashOf(3), &value));
        ASSERT_EQ(value, blockOf(3));
        ASSERT_TRUE(db->get(hashOf(3), nullptr));
        uint32_t seq = 0;
        ASSERT_TRUE(db->seq_no(hashOf(5), &seq));
        ASSERT_EQ(seq, 5u);

        ASSERT_TRUE(db->remove(hashOf(0)));
        ASSERT_TRUE(db->remove(hashOf(1)));
        ASSERT_FALSE(db->get(uint32_t(0), &value));
        ASSERT_FALSE(db->get(hashOf(1), nullptr));
        ASSERT_FALSE(db->remove(hashOf(1)));

        auto it = db->new_iterator();
        it->seek_to_first();
        ASSERT_TRUE(it->is_valid());
        ASSERT_EQ(it->key(), 2u);  // a pruned storage starts after block 0
        it->seek_to_last();
        ASSERT_EQ(it->key(), 9u);
        it->seek(uint32_t(4));
        ASSERT_TRUE(it->is_valid());
        it->next();
        ASSERT_EQ(it->key(), 5u);
        ASSERT_EQ(it->value(), blockOf(5));
        it->seek(uint32_t(1));
        ASSERT_FALSE(it->is_valid());

        ASSERT_TRUE(db->updateContractData(hashOf(100), blockOf(1, 10)));
        ASSERT_TRUE(db->getContractData(hashOf(100), value));
        ASSERT_EQ(value, blockOf(1, 10));
    }
    // reopened, the data is still there
    auto db = openAt(dir);
    cs::Bytes value;
    ASSERT_TRUE(db->get(uint32_t(9), &value));
    ASSERT_EQ(value, blockOf(9));
    db.reset();   // closed first: Windows does not remove open files
    std::filesystem::remove_all(dir);
}

// the reason for LMDB: a rolling window of blocks keeps the file at the window's size, where
// BerkeleyDB's Recno never reuses the pages of removed records
TEST(DatabaseLmdb, ReusesTheSpaceOfRemovedBlocks) {
    const auto dir = std::filesystem::temp_directory_path() / "cs_lmdb_reuse_test";
    std::filesystem::remove_all(dir);
    auto db = openAt(dir);
    constexpr uint32_t kWindow = 2000;
    auto fileSize = [&dir] { return std::filesystem::file_size(dir / "data.mdb"); };

    for (uint32_t seq = 0; seq < kWindow; ++seq) {
        ASSERT_TRUE(db->put(hashOf(seq), seq, blockOf(seq)));
    }
    const auto windowSize = fileSize();

    // ten more windows of blocks, each one pruning the blocks a window behind
    for (uint32_t seq = kWindow; seq < 11 * kWindow; ++seq) {
        ASSERT_TRUE(db->put(hashOf(seq), seq, blockOf(seq)));
        ASSERT_TRUE(db->remove(hashOf(seq - kWindow)));
    }
    EXPECT_LT(fileSize(), 2 * windowSize) << "window " << windowSize << " bytes";
    db.reset();
    std::filesystem::remove_all(dir);
}

// pruning removes a whole step of blocks in one transaction; blocks already gone are skipped
TEST(DatabaseLmdb, RemovesABatchSkippingMissingBlocks) {
    const auto dir = std::filesystem::temp_directory_path() / "cs_lmdb_batch_test";
    std::filesystem::remove_all(dir);
    auto db = openAt(dir);
    for (uint32_t seq = 0; seq < 100; ++seq) {
        ASSERT_TRUE(db->put(hashOf(seq), seq, blockOf(seq)));
    }
    ASSERT_TRUE(db->remove(hashOf(10)));

    std::vector<cs::Bytes> step;
    for (uint32_t seq = 0; seq < 64; ++seq) {
        step.push_back(hashOf(seq));
    }
    ASSERT_TRUE(db->remove_batch(step));

    cs::Bytes value;
    ASSERT_FALSE(db->get(uint32_t(0), &value));
    ASSERT_FALSE(db->get(hashOf(63), nullptr));
    ASSERT_TRUE(db->get(uint32_t(64), &value));
    auto it = db->new_iterator();
    it->seek_to_first();
    ASSERT_EQ(it->key(), 64u);
    it.reset();
    db.reset();
    std::filesystem::remove_all(dir);
}

// an environment whose tables cannot be opened (here "blocks" is a plain record, not a table) is of
// no use: open() fails and the database does not report itself open, so the storage refuses to start
TEST(DatabaseLmdb, FailedTableOpenLeavesItClosed) {
    auto dir = std::filesystem::temp_directory_path() / "cs_lmdb_bad_tables";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        MDB_env* env = nullptr;
        ASSERT_EQ(mdb_env_create(&env), 0);
        ASSERT_EQ(mdb_env_set_maxdbs(env, 3), 0);
        ASSERT_EQ(mdb_env_open(env, dir.string().c_str(), 0, 0644), 0);
        MDB_txn* txn = nullptr;
        ASSERT_EQ(mdb_txn_begin(env, nullptr, 0, &txn), 0);
        MDB_dbi main = 0;
        ASSERT_EQ(mdb_dbi_open(txn, nullptr, 0, &main), 0);
        MDB_val key{6, const_cast<char*>("blocks")};
        MDB_val value{1, const_cast<char*>("x")};
        ASSERT_EQ(mdb_put(txn, main, &key, &value, 0), 0);
        ASSERT_EQ(mdb_txn_commit(txn), 0);
        mdb_env_close(env);
    }
    auto lmdb = std::make_shared<csdb::DatabaseLmdb>();
    EXPECT_FALSE(lmdb->open(dir.string()));
    std::shared_ptr<csdb::Database> db = lmdb;    // the interface the storage checks
    EXPECT_FALSE(db->is_open());
    std::filesystem::remove_all(dir);
}
