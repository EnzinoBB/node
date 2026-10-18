#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../../client/src/dbcheck.hpp"
#include <csdb/storage_backend.hpp>
#include <csdb/storage_convert.hpp>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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

#ifndef _WIN32
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
    const int fd = ::open((dir / ".lock").c_str(), O_RDWR | O_CREAT, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);
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
    ::close(fd);
    fs::remove_all(dir);
}
#endif
