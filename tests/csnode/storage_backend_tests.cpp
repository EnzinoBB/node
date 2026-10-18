#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <csdb/storage_backend.hpp>

namespace fs = std::filesystem;

namespace {
fs::path freshDir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("cs_backend_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void fill(const std::string& backend, const fs::path& dir, uint32_t blocks) {
    auto db = csdb::backend::open(backend, dir.string());
    ASSERT_TRUE(db && db->is_open());
    for (uint32_t s = 0; s < blocks; ++s) {
        cs::Bytes key(32, 0);
        key[0] = static_cast<cs::Byte>(s);
        key[1] = static_cast<cs::Byte>(s >> 8);
        ASSERT_TRUE(db->put(key, s, cs::Bytes(4000, static_cast<cs::Byte>(s))));
    }
    db->flush();
}
}  // namespace

TEST(StorageBackend, DetectsEachFormat) {
    for (std::string b : {"berkeleydb", "lmdb", "rocksdb"}) {
        auto dir = freshDir("each_" + b);
        fill(b, dir, 100);
        EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{b}) << b;
        fs::remove_all(dir);
    }
}

TEST(StorageBackend, DetectsBothFormats) {
    auto dir = freshDir("both");
    fill("berkeleydb", dir, 100);
    fill("lmdb", dir, 100);
    EXPECT_EQ(csdb::backend::detect(dir.string()), (std::vector<std::string>{"berkeleydb", "lmdb"}));
    fs::remove_all(dir);
}

TEST(StorageBackend, EmptyTargetDoesNotCount) {
    auto dir = freshDir("empty_target");
    fill("berkeleydb", dir, 100);
    { auto lmdb = csdb::backend::open("lmdb", dir.string()); ASSERT_TRUE(lmdb); }  // opened, no blocks
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{"berkeleydb"});
    fs::remove_all(dir);
}

TEST(StorageBackend, EmptyRocksDbHasNoFormat) {
    auto dir = freshDir("empty_rocksdb");
    { auto db = csdb::backend::open("rocksdb", dir.string()); ASSERT_TRUE(db); }   // a fresh node's database
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());
    fs::remove_all(dir);
}

TEST(StorageBackend, SingleFormatIsReadFromTheFiles) {
    // opening RocksDB writes a new MANIFEST and OPTIONS file: a detect that did not open leaves the
    // folder as it was
    auto dir = freshDir("single_no_open");
    fill("rocksdb", dir, 100);
    { auto db = csdb::backend::open("rocksdb", dir.string()); ASSERT_TRUE(db); }   // moves the WAL into a table file
    auto names = [&] {
        std::vector<std::string> n;
        for (const auto& e : fs::directory_iterator(dir)) n.push_back(e.path().filename().string());
        std::sort(n.begin(), n.end());
        return n;
    };
    const auto before = names();
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{"rocksdb"});
    EXPECT_EQ(names(), before);
    fs::remove_all(dir);
}

TEST(StorageBackend, EmptyFolderHasNoFormat) {
    auto dir = freshDir("empty");
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "");
    fs::remove_all(dir);
}

TEST(StorageBackend, MissingFolderHasNoFormat) {
    const auto dir = fs::temp_directory_path() / "cs_backend_missing";
    fs::remove_all(dir);
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());   // must not throw or create the folder
    EXPECT_FALSE(fs::exists(dir));
}

TEST(StorageBackend, MarkerWithoutDataIsNoFormat) {
    auto dir = freshDir("marker_only");
    ASSERT_TRUE(csdb::backend::writeMarker(dir.string(), "berkeleydb"));
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());
    fs::remove_all(dir);
}

TEST(StorageBackend, MarkerRoundTripAndRejectsGarbage) {
    auto dir = freshDir("marker");
    ASSERT_TRUE(csdb::backend::writeMarker(dir.string(), "rocksdb"));
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "rocksdb");
    std::ofstream(dir / ".backend") << "leveldb\n";
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "");
    fs::remove_all(dir);
}
