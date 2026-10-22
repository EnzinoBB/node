#include "gtest/gtest.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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
Chain makeChain(uint32_t first, uint32_t count, size_t payload = 200) {
    Chain c;
    c.first = first;
    csdb::PoolHash prev;
    for (uint32_t i = 0; i < count; ++i) {
        csdb::Pool p(prev, first + i);
        if (payload > 0) p.add_user_field(1, std::string(payload, static_cast<char>('a' + i % 26)));
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

// the step-crash and refusal tests, run newest first (berkeleydb source) and oldest first (the others)
class ConvertOrder : public ::testing::TestWithParam<std::pair<std::string, std::string>> {
protected:
    std::string from() const { return GetParam().first; }
    std::string to() const { return GetParam().second; }
    std::string name(const std::string& test) const { return test + "_" + from() + "_" + to(); }
};

// the metadata page of BerkeleyDB's block tables overwritten with garbage: opening the environment
// fails, in recovery or in the table's open, as after a crash recovery cannot replay
void damageBerkeleyDb(const fs::path& dir) {
    for (const char* f : {"blockchain.db", "sequence.db"}) {
        std::fstream file(dir / f, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(file) << f;
        const std::string garbage(4096, '\xab');
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
}

bool opens(const std::string& backend, const fs::path& dir) {
    try {
        return csdb::backend::open(backend, dir.string()) != nullptr;
    }
    catch (const std::exception&) {
        return false;
    }
}

// blocks a recorded state has not converted yet
std::pair<int64_t, int64_t> unconverted(const csdb::convert::State& s) {
    if (s.ascending) return {s.doneUpTo < 0 ? int64_t(s.first) : s.doneUpTo + 1, int64_t(s.last)};
    return {int64_t(s.first), s.doneDownTo < 0 ? int64_t(s.last) : s.doneDownTo - 1};
}
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

INSTANTIATE_TEST_CASE_P(BothOrders, ConvertOrder, ::testing::Values(
    std::make_pair(std::string("berkeleydb"), std::string("lmdb")),
    std::make_pair(std::string("lmdb"), std::string("berkeleydb")),
    std::make_pair(std::string("rocksdb"), std::string("lmdb"))));

TEST(Convert, WritesNewestFirstOnlyFromBerkeleyDb) {
    // BerkeleyDB keeps blocks in a Recno table, record = sequence + 1: a first put far above record 1
    // creates every record below it in one operation, which on mainnet (4 M blocks, rocksdb to
    // berkeleydb) ran out of lock entries. Only a berkeleydb source, which shrinks from the end, is
    // converted newest first; any other is converted oldest first, so a berkeleydb target grows record
    // by record.
    using csdb::convert::Step;
    const auto chain = makeChain(0, 200);
    for (std::string from : {"berkeleydb", "lmdb", "rocksdb"}) {
        for (std::string to : {"berkeleydb", "lmdb", "rocksdb"}) {
            if (from == to) continue;
            const auto dir = writeDb(from, "order_" + from + "_" + to, chain);
            auto o = opts(dir, from, to);
            std::vector<std::pair<uint32_t, uint32_t>> read;
            o.hook = [&](Step s, uint32_t lo, uint32_t hi) {
                if (s == Step::Read) read.emplace_back(lo, hi);
            };
            auto r = csdb::convert::run(o);
            ASSERT_TRUE(r.ok) << from << " -> " << to << ": " << r.error;
            ASSERT_EQ(read.size(), 6u) << from << " -> " << to;    // 200 blocks in batches of 37
            const bool newestFirst = from == "berkeleydb";
            EXPECT_EQ(newestFirst ? read.front().second : read.front().first, newestFirst ? 199u : 0u) << from << " -> " << to;
            EXPECT_EQ(newestFirst ? read.back().first : read.back().second, newestFirst ? 0u : 199u) << from << " -> " << to;
            for (size_t i = 1; i < read.size(); ++i) {
                if (newestFirst) EXPECT_EQ(read[i].second + 1, read[i - 1].first) << from << " -> " << to;
                else EXPECT_EQ(read[i].first, read[i - 1].second + 1) << from << " -> " << to;
            }
            expectSame(to, dir, chain);
            fs::remove_all(dir);
        }
    }
}

TEST(Convert, ConvertsPrunedRange) {
    const auto chain = makeChain(1000, 300);
    for (auto [from, to] : {std::make_pair(std::string("lmdb"), std::string("rocksdb")),
                            std::make_pair(std::string("rocksdb"), std::string("lmdb")),
                            std::make_pair(std::string("berkeleydb"), std::string("lmdb"))}) {
        const auto dir = writeDb(from, "pruned_" + from + "_" + to, chain);
        auto r = csdb::convert::run(opts(dir, from, to));
        ASSERT_TRUE(r.ok) << from << " -> " << to << ": " << r.error;
        expectSame(to, dir, chain);
        fs::remove_all(dir);
    }
}

TEST(Convert, RefusesAPrunedDatabaseToBerkeleyDb) {
    // a pruned database starts far above record 1 of BerkeleyDB's Recno table, as on mainnet: whatever
    // the order, its first put would create millions of empty records at once
    const auto chain = makeChain(3999000, 200);
    for (std::string from : {"lmdb", "rocksdb"}) {
        const auto dir = writeDb(from, "pruned_to_bdb_" + from, chain);
        csdb::convert::Result r;
        EXPECT_NO_THROW(r = csdb::convert::run(opts(dir, from, "berkeleydb")));
        EXPECT_FALSE(r.ok);
        EXPECT_FALSE(r.stopped);
        EXPECT_NE(r.error.find("a pruned block database cannot be converted to berkeleydb; use lmdb or rocksdb"), std::string::npos) << r.error;
        // nothing changed
        EXPECT_FALSE(fs::exists(dir / ".convert"));
        EXPECT_FALSE(fs::exists(dir / ".convert-new"));
        EXPECT_TRUE(csdb::backend::files("berkeleydb", dir.string()).empty());
        EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{from});
        auto db = csdb::backend::open(from, dir.string());
        ASSERT_TRUE(db);
        for (size_t i = 0; i < chain.blocks.size(); ++i) {
            cs::Bytes v;
            ASSERT_TRUE(db->get(static_cast<uint32_t>(chain.first + i), &v)) << i;
            EXPECT_EQ(v, chain.blocks[i]);
        }
        db.reset();
        fs::remove_all(dir);
    }
}

TEST_P(ConvertOrder, ResumesAfterEachStep) {
    using csdb::convert::Step;
    for (Step crashAt : {Step::Read, Step::Written, Step::Verified, Step::Recorded, Step::Removed}) {
        const auto chain = makeChain(0, 400);
        const auto dir = writeDb(from(), name("resume"), chain);
        auto o = opts(dir, from(), to());
        int batches = 0;
        o.hook = [&](Step s, uint32_t, uint32_t) {
            if (s == crashAt && ++batches == 3) throw std::runtime_error("simulated crash");
        };
        EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
        ASSERT_TRUE(fs::exists(dir / ".convert"));
        o.hook = nullptr;
        auto r = csdb::convert::run(o);
        ASSERT_TRUE(r.ok) << r.error << " crash at step " << static_cast<int>(crashAt);
        expectSame(to(), dir, chain);
        fs::remove_all(dir);
    }
}

TEST_P(ConvertOrder, StopsBetweenBatchesAndResumes) {
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb(from(), name("stop"), chain);
    auto o = opts(dir, from(), to());
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 2; };
    auto r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.stopped);
    // two batches of 37 converted, from the end that suits the source
    csdb::convert::State st;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), st));
    EXPECT_EQ(st.ascending, from() != "berkeleydb");
    EXPECT_EQ(unconverted(st), st.ascending ? std::make_pair(int64_t(74), int64_t(399)) : std::make_pair(int64_t(0), int64_t(325)));
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    expectSame(to(), dir, chain);
    fs::remove_all(dir);
}

TEST_P(ConvertOrder, RemovesWhatACrashLeftOfARecordedBatch) {
    // a crash in the middle of remove_batch: part of the recorded batch is still in the old database
    using csdb::convert::Step;
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb(from(), name("leftover"), chain);
    auto o = opts(dir, from(), to());
    int batches = 0;
    uint32_t lo = 0, hi = 0;
    o.hook = [&](Step s, uint32_t l, uint32_t h) {
        if (s == Step::Recorded && ++batches == 2) {
            lo = l;
            hi = h;
            throw std::runtime_error("simulated crash");
        }
    };
    EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
    ASSERT_EQ(hi - lo + 1, 37u);
    {
        auto db = csdb::backend::open(from(), dir.string());
        ASSERT_TRUE(db);
        std::vector<cs::Bytes> keys;
        for (uint32_t seq = lo; seq < lo + 10; ++seq) keys.push_back(chain.hashes[seq]);
        ASSERT_TRUE(db->remove_batch(keys));
        db->flush();
    }
    o.hook = nullptr;
    std::vector<std::string> log;
    o.log = [&](const std::string& line) { log.push_back(line); };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NE(std::find(log.begin(), log.end(), "removed 27 blocks already converted before the restart"), log.end());
    expectSame(to(), dir, chain);
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

TEST_P(ConvertOrder, RefusesToResumeOverTargetFilesThatAppearedInAPause) {
    // a node started on db/ during a pause created an empty database of the target backend there:
    // moving the converted one into db/ would collide with it
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb(from(), name("stray_target"), chain);
    auto o = opts(dir, from(), to(), 100);
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 2; };
    ASSERT_TRUE(csdb::convert::run(o).stopped);
    csdb::convert::State before;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), before));
    { auto stray = csdb::backend::open(to(), dir.string()); ASSERT_TRUE(stray); }
    o.stopRequested = nullptr;
    auto r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("not part of the conversion"), std::string::npos) << r.error;
    csdb::convert::State after;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), after));
    EXPECT_EQ(unconverted(after), unconverted(before));
    EXPECT_EQ(unconverted(before).second - unconverted(before).first + 1, 200);
    {
        auto db = csdb::backend::open(from(), dir.string());
        ASSERT_TRUE(db);
        cs::Bytes v;
        for (int64_t seq = unconverted(before).first; seq <= unconverted(before).second; ++seq) {
            ASSERT_TRUE(db->get(uint32_t(seq), &v)) << seq;
        }
    }
    for (const auto& p : csdb::backend::files(to(), dir.string())) fs::remove_all(p);
    r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    expectSame(to(), dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, FinishKeepsTheOldFilesWithoutTheNewDatabase) {
    const auto chain = makeChain(0, 100);
    const auto dir = writeDb("berkeleydb", "finish_without_new", chain);
    csdb::convert::State s;
    s.from = "berkeleydb"; s.to = "lmdb"; s.first = 0; s.last = 99;
    s.doneDownTo = 0; s.contractsCopied = true; s.moving = true;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("files are kept"), std::string::npos) << r.error;
    EXPECT_TRUE(fs::exists(dir / "blockchain.db"));
    EXPECT_TRUE(fs::exists(dir / ".convert"));
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
    EXPECT_FALSE(back.ascending);
    s.doneDownTo = 4;   // below first: malformed
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));

    csdb::convert::State a;
    a.from = "lmdb"; a.to = "berkeleydb"; a.first = 0; a.last = 900; a.ascending = true;
    a.doneUpTo = 99; a.doneUpToHash = "ab01"; a.contractsCopied = true;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), a));
    ASSERT_TRUE(csdb::convert::readState(dir.string(), back));
    EXPECT_TRUE(back.ascending);
    EXPECT_EQ(back.doneUpTo, 99);
    EXPECT_EQ(back.doneUpToHash, "ab01");
    EXPECT_EQ(back.doneDownTo, -1);
    EXPECT_TRUE(back.nextPrevHash.empty());
    a.doneUpTo = -1; a.doneUpToHash.clear();
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), a));
    ASSERT_TRUE(csdb::convert::readState(dir.string(), back));
    EXPECT_TRUE(back.ascending);
    EXPECT_EQ(back.doneUpTo, -1);
    a.doneUpTo = 901;   // above last: malformed
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), a));
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));
    fs::remove_all(dir);
}

TEST(Convert, StateWithoutOrderIsNewestFirst) {
    // db/.convert as written before the order was recorded: those conversions all went newest first
    const auto dir = fs::temp_directory_path() / "cs_convert_state_no_order";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto write = [&](const std::string& text) { std::ofstream(dir / ".convert") << text; };
    const std::string old = "from=rocksdb\nto=lmdb\nfirst=0\nlast=399\ndone_down_to=326\nnext_prev_hash=00ff\ncontracts=copied\nphase=blocks\n";
    write(old);
    csdb::convert::State back;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), back));
    EXPECT_FALSE(back.ascending);
    EXPECT_EQ(back.doneDownTo, 326);
    EXPECT_EQ(back.nextPrevHash, "00ff");
    write(old + "order=desc\n");
    ASSERT_TRUE(csdb::convert::readState(dir.string(), back));
    EXPECT_FALSE(back.ascending);
    write(old + "order=sideways\n");
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));
    write(old + "order=asc\n");     // a newest-first boundary in an oldest-first state
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));
    write("from=rocksdb\nto=lmdb\nfirst=0\nlast=399\ndone_up_to=73\ncontracts=copied\nphase=blocks\n");
    EXPECT_FALSE(csdb::convert::readState(dir.string(), back));   // without order=asc
    fs::remove_all(dir);
}

TEST(Convert, ResumesAStateWithoutOrderNewestFirst) {
    // a rocksdb conversion paused by the code that wrote no order key, after its first batch (blocks
    // 363..399): it goes on newest first, as it started
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb("rocksdb", "resume_no_order", chain);
    {
        auto newDb = csdb::backend::open("lmdb", (dir / ".convert-new").string());
        ASSERT_TRUE(newDb);
        for (uint8_t i = 0; i < 5; ++i) ASSERT_TRUE(newDb->updateContractData(cs::Bytes(32, i), cs::Bytes(50, i)));
        std::vector<cs::Bytes> keys;
        for (uint32_t seq = 363; seq < 400; ++seq) {
            ASSERT_TRUE(newDb->put(chain.hashes[seq], seq, chain.blocks[seq]));
            keys.push_back(chain.hashes[seq]);
        }
        newDb->flush();
        auto oldDb = csdb::backend::open("rocksdb", dir.string());
        ASSERT_TRUE(oldDb);
        ASSERT_TRUE(oldDb->remove_batch(keys));
        oldDb->flush();
    }
    std::string prevHex;
    for (auto c : chain.hashes[362]) { static const char* d = "0123456789abcdef"; prevHex += d[c >> 4]; prevHex += d[c & 15]; }
    std::ofstream(dir / ".convert") << "from=rocksdb\nto=lmdb\nfirst=0\nlast=399\ndone_down_to=363\nnext_prev_hash=" << prevHex
                                    << "\ncontracts=copied\nphase=blocks\n";
    auto o = opts(dir, "rocksdb", "lmdb");
    std::vector<std::pair<uint32_t, uint32_t>> read;
    o.hook = [&](csdb::convert::Step s, uint32_t lo, uint32_t hi) {
        if (s == csdb::convert::Step::Read) read.emplace_back(lo, hi);
    };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_FALSE(read.empty());
    EXPECT_EQ(read.front(), std::make_pair(326u, 362u));
    EXPECT_EQ(read.back(), std::make_pair(0u, 29u));
    expectSame("lmdb", dir, chain);
    fs::remove_all(dir);
}

namespace {
uint64_t sourceBytes(const std::string& backend, const fs::path& dir) {
    uint64_t n = 0;
    for (const auto& p : csdb::backend::files(backend, dir.string())) {
        if (fs::is_directory(p)) {
            for (const auto& e : fs::recursive_directory_iterator(p)) {
                if (e.is_regular_file()) n += e.file_size();
            }
        }
        else {
            n += fs::file_size(p);
        }
    }
    return n;
}
}  // namespace

TEST(Convert, EstimateCoversTheWholeCopyUnlessTheSourceShrinks) {
    const auto chain = makeChain(0, 500);
    const auto lmdbDir = writeDb("lmdb", "estimate_lmdb", chain);
    const auto bdbDir = writeDb("berkeleydb", "estimate_bdb", chain);
    // batch 1: in so small a database BerkeleyDB's environment files (MBs) dominate the size per block
    const auto lmdbEstimate = csdb::convert::estimatePeakBytes(lmdbDir.string(), "lmdb", "rocksdb", 1);
    const auto bdbEstimate = csdb::convert::estimatePeakBytes(bdbDir.string(), "berkeleydb", "lmdb", 1);
    // LMDB keeps its file size while blocks leave it: the new copy needs room for all of it
    EXPECT_GE(lmdbEstimate, sourceBytes("lmdb", lmdbDir));
    EXPECT_LT(bdbEstimate, lmdbEstimate);
    fs::remove_all(lmdbDir);
    fs::remove_all(bdbDir);
}

// RocksDB compresses its blocks: converted to an uncompressed backend they take several times the room
TEST(Convert, EstimateFromRocksDbCountsItsCompression) {
    const auto chain = makeChain(0, 500);
    const auto dir = writeDb("rocksdb", "estimate_rocks", chain);
    constexpr uint64_t margin = 256ULL << 20;
    const uint64_t source = sourceBytes("rocksdb", dir);
    EXPECT_GE(csdb::convert::estimatePeakBytes(dir.string(), "rocksdb", "berkeleydb", 1),
              source * csdb::convert::kExpansionFromRocksDb + margin);
    EXPECT_GE(csdb::convert::estimatePeakBytes(dir.string(), "rocksdb", "lmdb", 1),
              source * csdb::convert::kExpansionFromRocksDb + margin);
    EXPECT_EQ(csdb::convert::estimatePeakBytes(dir.string(), "rocksdb", "rocksdb", 1), source + margin);
    fs::remove_all(dir);
}

TEST(Convert, ResumedEstimateCountsTheConvertedPart) {
    // a node with just enough disk for the start must be able to resume: the old LMDB file keeps its
    // size, but what is already in .convert-new no longer needs room
    const auto chain = makeChain(0, 2000);
    const auto dir = writeDb("lmdb", "estimate_resume", chain);
    auto o = opts(dir, "lmdb", "rocksdb", 100);
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 10; };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.stopped) << r.error;
    ASSERT_TRUE(fs::exists(dir / ".convert"));
    constexpr uint64_t margin = 256ULL << 20;
    const uint64_t old = sourceBytes("lmdb", dir);      // what a fresh start on these files would count
    uint64_t converted = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir / ".convert-new")) {
        if (e.is_regular_file()) converted += e.file_size();
    }
    ASSERT_GT(converted, 0u);
    const auto resumed = csdb::convert::estimatePeakBytes(dir.string(), "lmdb", "rocksdb", 100);
    EXPECT_EQ(resumed, (old > converted ? old - converted : 0) + margin);
    EXPECT_LT(resumed, old + margin);
    EXPECT_GE(resumed, margin);     // never below the margin
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    fs::remove_all(dir);
}

TEST_P(ConvertOrder, RefusesToDeleteAnOldBlockOutsideTheRange) {
    const auto chain = makeChain(0, 401);
    Chain first400 = chain;
    first400.blocks.pop_back();
    first400.hashes.pop_back();
    const auto dir = writeDb(from(), name("late_block"), first400);
    auto o = opts(dir, from(), to());
    o.stopRequested = [] { return true; };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.stopped) << r.error;
    {
        // a block arrives in the old database after the range was recorded
        auto db = csdb::backend::open(from(), dir.string());
        ASSERT_TRUE(db);
        ASSERT_TRUE(db->put(chain.hashes[400], 400, chain.blocks[400]));
        db->flush();
    }
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("block 400"), std::string::npos) << r.error;
    EXPECT_TRUE(fs::exists(dir / ".convert"));
    auto db = csdb::backend::open(from(), dir.string());
    ASSERT_TRUE(db);
    cs::Bytes v;
    ASSERT_TRUE(db->get(400u, &v));
    EXPECT_EQ(v, chain.blocks[400]);
    db.reset();
    fs::remove_all(dir);
}

TEST_P(ConvertOrder, RefusesToDeleteAChangedContractRecord) {
    const auto chain = makeChain(0, 200);
    const auto dir = writeDb(from(), name("changed_contract"), chain);
    auto o = opts(dir, from(), to());
    o.stopRequested = [] { return true; };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.stopped) << r.error;
    {
        auto db = csdb::backend::open(from(), dir.string());
        ASSERT_TRUE(db);
        ASSERT_TRUE(db->updateContractData(cs::Bytes(32, 2), cs::Bytes(50, 9)));
        db->flush();
    }
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("contract record"), std::string::npos) << r.error;
    EXPECT_TRUE(fs::exists(dir / ".convert"));
    auto db = csdb::backend::open(from(), dir.string());
    ASSERT_TRUE(db);
    cs::Bytes v;
    ASSERT_TRUE(db->getContractData(cs::Bytes(32, 2), v));
    EXPECT_EQ(v, cs::Bytes(50, 9));
    db.reset();
    fs::remove_all(dir);
}

TEST_P(ConvertOrder, ResumesAfterTheLastCheck) {
    // every block converted and verified, the old database not yet compared nor deleted
    using csdb::convert::Step;
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb(from(), name("checked"), chain);
    auto o = opts(dir, from(), to());
    o.hook = [](Step s, uint32_t, uint32_t) {
        if (s == Step::Checked) throw std::runtime_error("simulated crash");
    };
    EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
    csdb::convert::State st;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), st));
    EXPECT_EQ(unconverted(st).first, unconverted(st).second + 1);   // nothing left to convert
    EXPECT_FALSE(st.moving);
    o.hook = nullptr;
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    expectSame(to(), dir, chain);
    fs::remove_all(dir);
}

// A crash in BerkeleyDB's last compaction can leave a log recovery cannot replay (DB_RUNRECOVERY on
// every start). Every block is in .convert-new by then: the conversion finishes without the old files.
TEST(Convert, FinishesWithoutABerkeleyDbThatDoesNotOpen) {
    using csdb::convert::Step;
    for (const std::string to : {"lmdb", "rocksdb"}) {
        const auto chain = makeChain(0, 400);
        const auto dir = writeDb("berkeleydb", "unopenable_" + to, chain);
        auto o = opts(dir, "berkeleydb", to);
        o.hook = [](Step s, uint32_t, uint32_t) {
            if (s == Step::Checked) throw std::runtime_error("simulated crash");
        };
        EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
        damageBerkeleyDb(dir);
        ASSERT_FALSE(opens("berkeleydb", dir)) << "the damage must make the source unopenable";
        // the resumed estimate counts from db/.convert and does not open the source
        uint64_t estimate = 1;
        EXPECT_NO_THROW(estimate = csdb::convert::estimatePeakBytes(dir.string(), "berkeleydb", to, 37));
        EXPECT_EQ(estimate, 256ULL << 20);
        o.hook = nullptr;
        std::vector<std::string> log;
        o.log = [&](const std::string& line) { log.push_back(line); };
        auto r = csdb::convert::run(o);
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_TRUE(std::any_of(log.begin(), log.end(), [](const std::string& l) {
            return l.find("cannot be opened") != std::string::npos && l.find("finishing without it") != std::string::npos;
        }));
        EXPECT_TRUE(csdb::backend::files("berkeleydb", dir.string()).empty());
        expectSame(to, dir, chain);
        fs::remove_all(dir);
    }
}

TEST(Convert, AnUnopenableSourceMidwayKeepsTheConversion) {
    // blocks still to convert are only in the old database: stop, keep everything, say which
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb("berkeleydb", "unopenable_midway", chain);
    auto o = opts(dir, "berkeleydb", "rocksdb");
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 2; };
    auto r = csdb::convert::run(o);
    ASSERT_TRUE(r.stopped) << r.error;
    damageBerkeleyDb(dir);
    ASSERT_FALSE(opens("berkeleydb", dir));
    EXPECT_NO_THROW(csdb::convert::estimatePeakBytes(dir.string(), "berkeleydb", "rocksdb", 37));
    o.stopRequested = nullptr;
    EXPECT_NO_THROW(r = csdb::convert::run(o));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("cannot be opened"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("blocks 0..325 are not converted yet"), std::string::npos) << r.error;
    EXPECT_TRUE(fs::exists(dir / ".convert"));
    EXPECT_FALSE(csdb::backend::files("rocksdb", (dir / ".convert-new").string()).empty());
    fs::remove_all(dir);
}

TEST(Convert, CheckpointsTheBerkeleyDbSourceAfterEachCompaction) {
    // after a batch the source's log and files agree: a copy taken without closing the environment
    // (what a kill leaves) opens with nothing to replay and holds exactly the unconverted blocks
    using csdb::convert::Step;
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb("berkeleydb", "checkpointed", chain);
    const auto copy = fs::temp_directory_path() / "cs_convert_checkpointed_copy";
    fs::remove_all(copy);
    auto o = opts(dir, "berkeleydb", "rocksdb");
    int batches = 0;
    o.hook = [&](Step s, uint32_t, uint32_t) {
        if (s == Step::Removed && ++batches == 2) {
            fs::create_directories(copy);
            for (const auto& p : csdb::backend::files("berkeleydb", dir.string())) {
                if (p.filename().string().rfind("__db.", 0) != 0) fs::copy(p, copy / p.filename());
            }
            fs::copy(dir / ".convert", copy / ".convert");
            throw std::runtime_error("simulated crash");
        }
    };
    EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
    {
        auto db = csdb::backend::open("berkeleydb", copy.string());
        ASSERT_TRUE(db);
        auto it = db->new_iterator();
        ASSERT_TRUE(it);
        it->seek_to_first();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 0u);
        it->seek_to_last();
        ASSERT_TRUE(it->is_valid());
        EXPECT_EQ(it->key(), 325u);
    }
    fs::remove_all(copy);
    fs::remove_all(dir);
}

TEST(Convert, AnUnopenableSourceBeforeTheContractsAreCopiedSaysSo) {
    // every block recorded as converted but the contract records not (a hand-edited db/.convert):
    // the old database is still needed, and the message names what is missing, not a block range
    using csdb::convert::Step;
    const auto chain = makeChain(0, 200);
    const auto dir = writeDb("berkeleydb", "unopenable_contracts", chain);
    auto o = opts(dir, "berkeleydb", "rocksdb");
    o.hook = [](Step s, uint32_t, uint32_t) {
        if (s == Step::Checked) throw std::runtime_error("simulated crash");
    };
    EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
    csdb::convert::State st;
    ASSERT_TRUE(csdb::convert::readState(dir.string(), st));
    st.contractsCopied = false;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), st));
    damageBerkeleyDb(dir);
    o.hook = nullptr;
    auto r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("contract records are not copied yet"), std::string::npos) << r.error;
    EXPECT_EQ(r.error.find("blocks "), std::string::npos) << r.error;
    EXPECT_TRUE(fs::exists(dir / ".convert"));
    EXPECT_TRUE(fs::exists(dir / ".convert-new"));
    fs::remove_all(dir);
}
