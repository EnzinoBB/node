#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include "../../client/src/dbcheck.hpp"
#include <csdb/storage_convert.hpp>

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
