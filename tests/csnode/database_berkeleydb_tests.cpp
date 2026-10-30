#include "gtest/gtest.h"

#include <cerrno>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <csdb/database_berkeleydb.hpp>

// A failed open leaves some handles null: destroying the object afterwards must not crash.
TEST(DatabaseBerkeleyDB, DestroyAfterAFailedOpen) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cs_berkeleydb_failed_open";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "blockchain.db", std::ios::binary);
        f << std::string(128 * 1024, 'x');   // not a BerkeleyDB file
    }
    std::ofstream(dir / "sequence.db").close();

    bool opened = true;
    {
        auto db = std::make_unique<csdb::DatabaseBerkeleyDB>();
        try {
            opened = db->open(dir.string());
        }
        catch (const std::exception&) {
            opened = false;
        }
        EXPECT_FALSE(static_cast<csdb::Database&>(*db).is_open());
    }   // the destructor runs here
    EXPECT_FALSE(opened);
    fs::remove_all(dir);
}

// an environment that needs recovery stops the node (the error is rethrown); every other error, a full
// disk included, is a failed call the node retries
TEST(DatabaseBerkeleyDB, OnlyARecoveryErrorIsFatal) {
    EXPECT_TRUE(csdb::needsRecovery(DbRunRecoveryException("panic")));
    EXPECT_TRUE(csdb::needsRecovery(DbException("panic", DB_RUNRECOVERY)));
    EXPECT_FALSE(csdb::needsRecovery(DbException("disk full", ENOSPC)));
    EXPECT_FALSE(csdb::needsRecovery(DbException("not found", DB_NOTFOUND)));
    EXPECT_FALSE(csdb::needsRecovery(std::runtime_error("other")));
}

// Backward sync on a pruned BerkeleyDB node switched to full: the Recno records pruning deleted are
// written again, below the first stored one, and read and iterated as before.
TEST(DatabaseBerkeleyDB, DeletedRecordsAreWrittenAgainDownwards) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cs_berkeleydb_backfill";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto key = [](uint32_t seq) { cs::Bytes k(32, 0); k[0] = static_cast<cs::Byte>(seq); k[1] = 0x42; return k; };
    auto block = [](uint32_t seq) { return cs::Bytes(500, static_cast<cs::Byte>(seq)); };
    {
        csdb::DatabaseBerkeleyDB bdb;
        ASSERT_TRUE(bdb.open(dir.string()));
        csdb::Database& db = bdb;
        for (uint32_t s = 50; s <= 60; ++s) {
            ASSERT_TRUE(db.put(key(s), s, block(s)));
        }
        for (uint32_t s = 50; s <= 54; ++s) {
            ASSERT_TRUE(db.remove(key(s)));
        }
        cs::Bytes value;
        EXPECT_FALSE(db.get(uint32_t(53), &value));
        auto it = db.new_iterator();
        it->seek_to_first();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 55u);
        it.reset();   // an open cursor would hold the locks the writes below need

        ASSERT_TRUE(db.put(key(54), 54, block(54)));
        ASSERT_TRUE(db.put(key(53), 53, block(53)));
        ASSERT_TRUE(db.get(uint32_t(53), &value));
        EXPECT_EQ(value, block(53));
        uint32_t seq = 0;
        ASSERT_TRUE(db.seq_no(key(54), &seq));
        EXPECT_EQ(seq, 54u);
        it = db.new_iterator();
        it->seek_to_first();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 53u);
        it->next();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 54u);
    }
    {
        // and after a reopen
        csdb::DatabaseBerkeleyDB bdb;
        ASSERT_TRUE(bdb.open(dir.string()));
        csdb::Database& db = bdb;
        auto it = db.new_iterator();
        it->seek_to_first();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 53u);
    }
    fs::remove_all(dir);
}
