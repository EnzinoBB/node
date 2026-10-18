#include "gtest/gtest.h"

#include <filesystem>
#include <stdexcept>

#include <csdb/pool.hpp>
#include <csdb/storage_backend.hpp>
#include <csdb/storage_convert.hpp>

namespace fs = std::filesystem;

namespace {
struct Chain {
    std::vector<cs::Bytes> blocks;   // index = sequence - first
    std::vector<cs::Bytes> hashes;
    uint32_t first = 0;
};

// a real hash-linked chain: the converter parses pools and checks previous hashes
Chain makeChain(uint32_t first, uint32_t count) {
    Chain c;
    c.first = first;
    csdb::PoolHash prev;
    for (uint32_t i = 0; i < count; ++i) {
        csdb::Pool p(prev, first + i);
        p.add_user_field(1, std::string(200, static_cast<char>('a' + i % 26)));
        EXPECT_TRUE(p.compose());
        c.blocks.push_back(p.to_binary());
        c.hashes.push_back(p.hash().to_binary());
        prev = p.hash();
    }
    return c;
}

void fillDb(csdb::Database& db, const Chain& c) {
    for (size_t i = 0; i < c.blocks.size(); ++i) {
        EXPECT_TRUE(db.put(c.hashes[i], static_cast<uint32_t>(c.first + i), c.blocks[i]));
    }
    for (uint8_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(db.updateContractData(cs::Bytes(32, i), cs::Bytes(50, i)));
    }
    db.flush();
}

fs::path writeDb(const std::string& backend, const std::string& name, const Chain& c) {
    auto dir = fs::temp_directory_path() / ("cs_convert_" + name);
    fs::remove_all(dir);
    auto db = csdb::backend::open(backend, dir.string());
    EXPECT_TRUE(db);
    if (db) {
        fillDb(*db, c);
    }
    return dir;
}

void expectSame(const std::string& backend, const fs::path& dir, const Chain& c) {
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{backend});
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), backend);
    EXPECT_FALSE(fs::exists(dir / ".convert"));
    EXPECT_FALSE(fs::exists(dir / ".convert-new"));
    auto db = csdb::backend::open(backend, dir.string());
    ASSERT_TRUE(db);
    for (size_t i = 0; i < c.blocks.size(); ++i) {
        cs::Bytes v;
        ASSERT_TRUE(db->get(static_cast<uint32_t>(c.first + i), &v)) << i;
        EXPECT_EQ(v, c.blocks[i]);
        ASSERT_TRUE(db->get(c.hashes[i], &v));
        EXPECT_EQ(v, c.blocks[i]);
    }
    for (uint8_t i = 0; i < 5; ++i) {
        cs::Bytes v;
        ASSERT_TRUE(db->getContractData(cs::Bytes(32, i), v));
        EXPECT_EQ(v, cs::Bytes(50, i));
    }
}

csdb::convert::Options opts(const fs::path& dir, std::string from, std::string to, uint32_t batch = 37) {
    csdb::convert::Options o;
    o.dbPath = dir.string();
    o.from = std::move(from);
    o.to = std::move(to);
    o.batch = batch;
    return o;
}

class ConvertPair : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};
}  // namespace

TEST(Convert, ParsedPoolHasTheStoredHash) {
    // the converter takes each block's key from the parsed pool
    const auto chain = makeChain(7, 3);
    for (size_t i = 0; i < chain.blocks.size(); ++i) {
        auto p = csdb::Pool::from_binary(cs::Bytes(chain.blocks[i]));
        ASSERT_TRUE(p.is_valid());
        EXPECT_EQ(p.sequence(), 7 + i);
        EXPECT_EQ(p.hash().to_binary(), chain.hashes[i]);
        if (i > 0) {
            EXPECT_EQ(p.previous_hash().to_binary(), chain.hashes[i - 1]);
        }
    }
}

TEST_P(ConvertPair, ConvertsEveryBlockAndContract) {
    const auto [from, to] = GetParam();
    const auto chain = makeChain(0, 500);
    const auto dir = writeDb(from, from + "_" + to, chain);
    auto r = csdb::convert::run(opts(dir, from, to));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.blocks, 500u);
    EXPECT_EQ(r.contracts, 5u);
    expectSame(to, dir, chain);
    fs::remove_all(dir);
}

INSTANTIATE_TEST_CASE_P(AllDirections, ConvertPair, ::testing::Values(
    std::make_pair(std::string("berkeleydb"), std::string("lmdb")),
    std::make_pair(std::string("berkeleydb"), std::string("rocksdb")),
    std::make_pair(std::string("lmdb"), std::string("berkeleydb")),
    std::make_pair(std::string("lmdb"), std::string("rocksdb")),
    std::make_pair(std::string("rocksdb"), std::string("berkeleydb")),
    std::make_pair(std::string("rocksdb"), std::string("lmdb"))));

TEST(Convert, ConvertsPrunedRange) {
    const auto chain = makeChain(1000, 300);
    const auto dir = writeDb("lmdb", "pruned", chain);
    auto r = csdb::convert::run(opts(dir, "lmdb", "berkeleydb"));
    ASSERT_TRUE(r.ok) << r.error;
    expectSame("berkeleydb", dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, ResumesAfterEachStep) {
    using csdb::convert::Step;
    for (Step crashAt : {Step::Read, Step::Written, Step::Verified, Step::Recorded, Step::Removed}) {
        const auto chain = makeChain(0, 400);
        const auto dir = writeDb("berkeleydb", "resume", chain);
        auto o = opts(dir, "berkeleydb", "lmdb");
        int batches = 0;
        o.hook = [&](Step s, uint32_t, uint32_t) {
            if (s == crashAt && ++batches == 3) throw std::runtime_error("simulated crash");
        };
        EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
        ASSERT_TRUE(fs::exists(dir / ".convert"));
        o.hook = nullptr;
        auto r = csdb::convert::run(o);
        ASSERT_TRUE(r.ok) << r.error << " crash at step " << static_cast<int>(crashAt);
        expectSame("lmdb", dir, chain);
        fs::remove_all(dir);
    }
}

TEST(Convert, StopsBetweenBatchesAndResumes) {
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb("rocksdb", "stop", chain);
    auto o = opts(dir, "rocksdb", "lmdb");
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 2; };
    auto r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.stopped);
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    expectSame("lmdb", dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, BrokenChainStopsWithNothingRemoved) {
    auto chain = makeChain(0, 200);
    chain.blocks[150] = makeChain(150, 1).blocks[0];        // a block that does not link to 149
    chain.hashes[150] = makeChain(150, 1).hashes[0];
    const auto dir = writeDb("lmdb", "broken", chain);
    auto r = csdb::convert::run(opts(dir, "lmdb", "rocksdb", 1000));  // one batch: nothing may be removed
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("150"), std::string::npos) << r.error;
    auto db = csdb::backend::open("lmdb", dir.string());
    ASSERT_TRUE(db);
    cs::Bytes v;
    for (uint32_t s = 0; s < 200; ++s) ASSERT_TRUE(db->get(s, &v)) << s;
    db.reset();
    fs::remove_all(dir);
}

TEST(Convert, RefusesAStateFileForAnotherConversion) {
    const auto chain = makeChain(0, 50);
    const auto dir = writeDb("berkeleydb", "other_state", chain);
    csdb::convert::State s;
    s.from = "rocksdb"; s.to = "lmdb"; s.first = 0; s.last = 49;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("unfinished conversion from rocksdb to lmdb"), std::string::npos) << r.error;
    fs::remove_all(dir);
}

TEST(Convert, RefusesALeftoverNewFolderWithoutState) {
    // .convert-new without db/.convert is not ours to reuse or delete
    const auto chain = makeChain(0, 50);
    const auto dir = writeDb("berkeleydb", "leftover_new", chain);
    fs::create_directories(dir / ".convert-new");
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find(".convert-new"), std::string::npos) << r.error;
    EXPECT_FALSE(fs::exists(dir / ".convert"));
    fs::remove_all(dir);
}

TEST(Convert, RefusesToResumeWithoutTheNewFolder) {
    const auto chain = makeChain(0, 100);
    const auto dir = writeDb("berkeleydb", "missing_new", chain);
    csdb::convert::State s;
    s.from = "berkeleydb"; s.to = "lmdb"; s.first = 0; s.last = 99;
    s.doneDownTo = 50; s.contractsCopied = true;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find(".convert-new is missing"), std::string::npos) << r.error;
    auto db = csdb::backend::open("berkeleydb", dir.string());
    ASSERT_TRUE(db);
    cs::Bytes v;
    for (uint32_t seq = 0; seq < 100; ++seq) ASSERT_TRUE(db->get(seq, &v)) << seq;
    db.reset();
    fs::remove_all(dir);
}

TEST(Convert, FinishesAnInterruptedMove) {
    // a crash while the new files were moved into db/: only the move is left to do
    const auto chain = makeChain(0, 100);
    const auto dir = writeDb("berkeleydb", "interrupted_move", chain);
    {
        auto db = csdb::backend::open("lmdb", (dir / ".convert-new").string());
        ASSERT_TRUE(db);
        fillDb(*db, chain);
    }
    csdb::convert::State s;
    s.from = "berkeleydb"; s.to = "lmdb"; s.first = 0; s.last = 99;
    s.doneDownTo = 0; s.contractsCopied = true; s.moving = true;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    ASSERT_TRUE(r.ok) << r.error;
    expectSame("lmdb", dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, StateRoundTrips) {
    const auto dir = fs::temp_directory_path() / "cs_convert_state";
    fs::remove_all(dir);
    fs::create_directories(dir);
    csdb::convert::State s, back;
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));
    s.from = "berkeleydb"; s.to = "rocksdb"; s.first = 5; s.last = 900;
    s.doneDownTo = 800; s.nextPrevHash = "00ff"; s.contractsCopied = true;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    ASSERT_TRUE(csdb::convert::readState(dir.string(), back));
    EXPECT_EQ(back.from, s.from);
    EXPECT_EQ(back.to, s.to);
    EXPECT_EQ(back.first, s.first);
    EXPECT_EQ(back.last, s.last);
    EXPECT_EQ(back.doneDownTo, s.doneDownTo);
    EXPECT_EQ(back.nextPrevHash, s.nextPrevHash);
    EXPECT_TRUE(back.contractsCopied);
    EXPECT_FALSE(back.moving);
    s.doneDownTo = 4;   // below first: malformed
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));
    fs::remove_all(dir);
}
