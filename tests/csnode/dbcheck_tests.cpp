#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../../client/src/dbcheck.hpp"
#include <csdb/storage_backend.hpp>
#include <csdb/storage_convert.hpp>

#include <lib/system/platform.hpp>

using dbcheck::Action;
using dbcheck::Facts;

TEST(DbCheck, DecisionTable) {
    EXPECT_EQ(dbcheck::decide(Facts{{}, "lmdb"}), Action::Open);
    EXPECT_EQ(dbcheck::decide(Facts{{"lmdb"}, "lmdb"}), Action::Open);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, false, false}), Action::StopMismatch);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, false, true}), Action::Ask);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, true, false}), Action::Convert);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", true, false, false}), Action::Resume);
}

TEST(DbCheck, BothFormatsStop) {
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb", "lmdb"}, "lmdb", false, true, true}), Action::StopBoth);
}

TEST(DbCheck, NoDatabaseStartsNormally) {
    namespace fs = std::filesystem;
    const auto missing = fs::temp_directory_path() / "cs_dbcheck_missing";
    fs::remove_all(missing);
    for (std::string b : {"berkeleydb", "lmdb", "rocksdb"}) {
        EXPECT_EQ(dbcheck::check(missing.string(), b, false, false, 100000), -1) << b;  // first start
        EXPECT_EQ(dbcheck::check(missing.string(), b, true, false, 100000), -1) << b;   // --convert-db with nothing to convert
    }
    EXPECT_FALSE(fs::exists(missing));
    const auto empty = fs::temp_directory_path() / "cs_dbcheck_empty_dir";
    fs::remove_all(empty);
    fs::create_directories(empty);
    EXPECT_EQ(dbcheck::check(empty.string(), "lmdb", false, false, 100000), -1);
    fs::remove_all(empty);
}

TEST(DbCheck, ConvertOnlyOnMatchingDatabaseExitsZero) {
    const auto dir = std::filesystem::temp_directory_path() / "cs_dbcheck_empty";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", true, true, 100000), 0);
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", false, false, 100000), -1);
    std::filesystem::remove_all(dir);
}

TEST(DbCheck, BatchOutsideTheLimitsStops) {
    namespace fs = std::filesystem;
    const auto missing = fs::temp_directory_path() / "cs_dbcheck_batch";
    fs::remove_all(missing);
    EXPECT_EQ(dbcheck::check(missing.string(), "lmdb", true, false, 0), 3);
    EXPECT_EQ(dbcheck::check(missing.string(), "lmdb", true, false, dbcheck::kMaxBatch + 1), 3);
    EXPECT_EQ(dbcheck::check(missing.string(), "lmdb", true, false, 1), -1);
    EXPECT_EQ(dbcheck::check(missing.string(), "lmdb", true, false, dbcheck::kMaxBatch), -1);
    EXPECT_FALSE(fs::exists(missing));
}

TEST(DbCheck, ResumeRefusals) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cs_dbcheck_resume";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / ".convert") << "garbage";
    }
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", false, false, 100000), 3);  // unreadable state
    csdb::convert::State s;
    s.from = "berkeleydb";
    s.to = "rocksdb";
    s.first = 1;
    s.last = 10;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", false, false, 100000), 3);  // converts to another backend
    fs::remove_all(dir);
}

TEST(DbCheck, NoConversionWhileAnotherProcessHoldsTheDatabase) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cs_dbcheck_locked";
    fs::remove_all(dir);
    {
        auto db = csdb::backend::open("berkeleydb", dir.string());
        ASSERT_TRUE(db);
        for (uint32_t s = 0; s < 100; ++s) {
            ASSERT_TRUE(db->put(cs::Bytes(32, static_cast<cs::Byte>(s)), s, cs::Bytes(4000, 1)));
        }
        db->flush();
    }
    ASSERT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{"berkeleydb"});
    // a running node: the lock taken by its own start-up check
    const auto fd = cs::platform::openFile(dir / ".lock", cs::platform::Open::ReadWrite);
    ASSERT_TRUE(cs::platform::valid(fd));
    ASSERT_EQ(cs::platform::lockExclusive(fd), cs::platform::Lock::Held);
    auto listing = [&] {
        std::vector<std::pair<std::string, uintmax_t>> l;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            l.emplace_back(e.path().string(), e.is_regular_file() ? e.file_size() : 0);
        }
        std::sort(l.begin(), l.end());
        return l;
    };
    const auto before = listing();
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", true, false, 100000), 3);   // --convert-db
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", true, true, 100000), 3);    // --convert-only
    EXPECT_EQ(listing(), before);
    EXPECT_FALSE(fs::exists(dir / ".convert"));
    EXPECT_FALSE(fs::exists(dir / ".convert-new"));
    EXPECT_EQ(dbcheck::check(dir.string(), "berkeleydb", false, false, 100000), -1);  // a normal start goes on
    cs::platform::closeFile(fd);
    fs::remove_all(dir);
}

TEST(DbCheck, PrunedDatabaseToBerkeleyDbStops) {
    // the converter refuses a pruned database to berkeleydb: --convert-db stops with nothing changed
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "cs_dbcheck_pruned_to_bdb";
    fs::remove_all(dir);
    {
        auto db = csdb::backend::open("lmdb", dir.string());
        ASSERT_TRUE(db);
        for (uint32_t s = 1000; s < 1100; ++s) {
            ASSERT_TRUE(db->put(cs::Bytes(32, static_cast<cs::Byte>(s)), s, cs::Bytes(100, 1)));
        }
        db->flush();
    }
    EXPECT_EQ(dbcheck::check(dir.string(), "berkeleydb", true, false, 100000), 3);   // --convert-db
    EXPECT_EQ(dbcheck::check(dir.string(), "berkeleydb", true, true, 100000), 3);    // --convert-only
    EXPECT_FALSE(fs::exists(dir / ".convert"));
    EXPECT_FALSE(fs::exists(dir / ".convert-new"));
    EXPECT_TRUE(csdb::backend::files("berkeleydb", dir.string()).empty());
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{"lmdb"});
    {
        auto db = csdb::backend::open("lmdb", dir.string());
        ASSERT_TRUE(db);
        cs::Bytes v;
        for (uint32_t s = 1000; s < 1100; ++s) ASSERT_TRUE(db->get(s, &v)) << s;
    }
    fs::remove_all(dir);
}
