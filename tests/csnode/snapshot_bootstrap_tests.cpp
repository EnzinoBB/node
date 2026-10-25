#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>
#include <cscrypto/cscrypto.hpp>
#include <csdb/database.hpp>
#include <csdb/storage_backend.hpp>
#include <csnode/snapshot_bootstrap.hpp>
#include <csnode/snapshot_manifest.hpp>

namespace {
namespace fs = std::filesystem;
namespace sb = cs::snapshot_bootstrap;

fs::path freshDir(const std::string& name) {
    const auto dir = fs::temp_directory_path() / ("cs_snapshot_bootstrap_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(const fs::path& p, const std::string& content = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string hex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

cs::CheckpointDigest digestOf(uint8_t fill, uint64_t wallets) {
    cs::CheckpointDigest d;
    d.digest.fill(fill);
    d.settings.fill(static_cast<uint8_t>(fill + 1));
    d.wallets = wallets;
    return d;
}

// a signed format-2 manifest of block seq declaring the digest d (the files are never read here)
cs::SnapshotManifest manifestOf(cs::Sequence seq, const cs::CheckpointDigest& d) {
    EXPECT_TRUE(cscrypto::cryptoInit());
    cs::SnapshotManifest m;
    m.seq = seq;
    m.headHash = std::string(64, 'a');
    m.stateDigest = hex(d.digest.data(), d.digest.size());
    m.wallets = d.wallets;
    m.settingsDigest = hex(d.settings.data(), d.settings.size());
    m.firstBlock = seq;
    m.created = "2026-10-25T00:00:00Z";
    m.node = "test";
    m.chunkSize = 64 << 10;
    for (const char* name : {"blocks.bin.lz4", "qs-head.bin.lz4", "qs-state_digest.bin.lz4"}) {
        cs::SnapshotFile f;
        f.name = name;
        f.size = 10;
        f.rawSize = 10;
        f.sha256 = std::string(64, 'b');
        f.chunks = {std::string(64, 'c')};
        m.files.push_back(f);
    }
    cs::PublicKey pub;
    const auto priv = cscrypto::generateKeyPair(pub);
    m.sign(priv);
    return m;
}

SnapshotData fetching() {
    SnapshotData cfg;
    cfg.fetch = "auto";
    return cfg;
}

}  // namespace

TEST(SnapshotBootstrap, NotNeededReasonsInOrder) {
    const auto dir = freshDir("reasons");
    const std::string db = (dir / "db").string();

    auto off = fetching();
    off.fetch = "off";
    auto r = sb::notNeeded(off, dir, db, "lmdb", true);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::FetchOff);

    r = sb::notNeeded(fetching(), dir, db, "lmdb", true);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::SetBcTop);

    // BerkeleyDB on an empty folder: the backend is the reason
    r = sb::notNeeded(fetching(), dir, db, "berkeleydb", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::Backend);
    EXPECT_NE(r->text.find("berkeleydb"), std::string::npos);

    // a database comes before the backend: a BerkeleyDB node with its blocks gets no backend warning
    touch(dir / "db" / "lmdb" / "data.mdb");
    r = sb::notNeeded(fetching(), dir, db, "berkeleydb", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::Database);
}

TEST(SnapshotBootstrap, NotNeededWithAnLmdbDatabase) {
    const auto dir = freshDir("lmdb_db");
    const std::string db = (dir / "db").string();
    {
        auto opened = csdb::backend::open("lmdb", db);
        ASSERT_TRUE(opened);
        ASSERT_TRUE(opened->is_open());
    }
    const auto r = sb::notNeeded(fetching(), dir, db, "lmdb", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::Database);
}

TEST(SnapshotBootstrap, NotNeededWithACheckpoint) {
    for (const char* qs : {"0", "2000"}) {
        const auto dir = freshDir(std::string("qs_") + qs);
        fs::create_directories(dir / "qs" / qs);
        const auto r = sb::notNeeded(fetching(), dir, (dir / "db").string(), "rocksdb", false);
        ASSERT_TRUE(r) << qs;
        EXPECT_EQ(r->why, sb::Skip::Checkpoint);
        EXPECT_NE(r->text.find(std::string("qs/") + qs), std::string::npos);
    }
    // a leftover qs/<S>.tmp is no checkpoint
    const auto dir = freshDir("qs_tmp");
    fs::create_directories(dir / "qs" / "2000.tmp");
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, (dir / "db").string(), "lmdb", false));
}

TEST(SnapshotBootstrap, FetchesOnAnEmptyFolderAndRemovesSnapshotTmp) {
    const auto dir = freshDir("fetch");
    bool removed = true;
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, (dir / "db").string(), "lmdb", false, &removed));
    EXPECT_FALSE(removed);

    touch(dir / sb::kTmpDir / "blocks.bin.lz4");
    fs::create_directories(dir / "db");   // created by main.cpp before dbcheck: empty, no database
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, (dir / "db").string(), "rocksdb", false, &removed));
    EXPECT_TRUE(removed);
    EXPECT_FALSE(fs::exists(dir / sb::kTmpDir));
}

TEST(SnapshotBootstrap, MessageGate) {
    EXPECT_TRUE(sb::processWhileBootstrapping(MsgTypes::SnapshotManifestReply));
    EXPECT_TRUE(sb::processWhileBootstrapping(MsgTypes::SnapshotPieceReply));
    EXPECT_FALSE(sb::processWhileBootstrapping(MsgTypes::SnapshotManifestRequest));
    EXPECT_FALSE(sb::processWhileBootstrapping(MsgTypes::SnapshotPieceRequest));
    EXPECT_FALSE(sb::processWhileBootstrapping(MsgTypes::RoundTable));
    EXPECT_FALSE(sb::processWhileBootstrapping(MsgTypes::BlockRequest));
    EXPECT_FALSE(sb::processWhileBootstrapping(MsgTypes::RoundPackRequest));
}

TEST(SnapshotBootstrap, StartupCheckOutcomes) {
    const auto digest = digestOf(0x11, 42);
    sb::StartupReport report;

    // no manifest
    auto dir = freshDir("check_none");
    EXPECT_EQ(sb::startupCheck(dir, 2000, digest, report), sb::StartupCheck::NoManifest);

    // verified
    dir = freshDir("check_verified");
    touch(dir / sb::kManifestFile, manifestOf(2000, digest).toJson());
    EXPECT_EQ(sb::startupCheck(dir, 2000, digest, report), sb::StartupCheck::Verified);
    EXPECT_EQ(report.seq, 2000u);
    EXPECT_EQ(readAll(dir / sb::kVerifiedFile), "verified at block 2000\n");
    EXPECT_FALSE(fs::exists(dir / sb::kMismatchFile));

    // then already verified, whatever the digest
    EXPECT_EQ(sb::startupCheck(dir, 2500, digestOf(0x22, 1), report), sb::StartupCheck::AlreadyVerified);

    // the chain is past the snapshot: not compared
    dir = freshDir("check_skipped");
    touch(dir / sb::kManifestFile, manifestOf(2000, digest).toJson());
    EXPECT_EQ(sb::startupCheck(dir, 2001, digestOf(0x22, 1), report), sb::StartupCheck::Skipped);
    EXPECT_EQ(readAll(dir / sb::kVerifiedFile), "skipped: chain at 2001, snapshot at 2000\n");

    // the chain below the snapshot's block: the restored state was not loaded, a mismatch whatever the digest
    dir = freshDir("check_below");
    touch(dir / sb::kManifestFile, manifestOf(2000, digest).toJson());
    EXPECT_EQ(sb::startupCheck(dir, 1999, digest, report), sb::StartupCheck::Mismatch);
    EXPECT_FALSE(fs::exists(dir / sb::kVerifiedFile));
    EXPECT_TRUE(fs::exists(dir / sb::kMismatchFile));
    EXPECT_NE(report.detail.find("below"), std::string::npos);
    dir = freshDir("check_zero");
    touch(dir / sb::kManifestFile, manifestOf(2000, digest).toJson());
    EXPECT_EQ(sb::startupCheck(dir, 0, digest, report), sb::StartupCheck::Mismatch);
    EXPECT_FALSE(fs::exists(dir / sb::kVerifiedFile));

    // another digest, other wallets, other settings: each a mismatch
    int n = 0;
    for (auto other : {digestOf(0x22, 42), digestOf(0x11, 43), digest}) {
        if (n == 2) {
            other.settings[0] ^= 1;
        }
        dir = freshDir("check_mismatch_" + std::to_string(n++));
        touch(dir / sb::kManifestFile, manifestOf(2000, digest).toJson());
        EXPECT_EQ(sb::startupCheck(dir, 2000, other, report), sb::StartupCheck::Mismatch);
        EXPECT_FALSE(fs::exists(dir / sb::kVerifiedFile));
        const auto text = readAll(dir / sb::kMismatchFile);
        EXPECT_NE(text.find(report.snapshot), std::string::npos);
        EXPECT_NE(text.find(report.node), std::string::npos);
        EXPECT_NE(report.snapshot, report.node);
    }

    // a format-2 manifest that does not parse, and no JSON at all: mismatches
    dir = freshDir("check_bad");
    auto json = manifestOf(2000, digest).toJson();
    json.replace(json.find("\"wallets\""), 9, "\"wallet\"");
    touch(dir / sb::kManifestFile, json);
    EXPECT_EQ(sb::startupCheck(dir, 2000, digest, report), sb::StartupCheck::Mismatch);
    EXPECT_FALSE(report.detail.empty());
    EXPECT_TRUE(fs::exists(dir / sb::kMismatchFile));
    dir = freshDir("check_garbage");
    touch(dir / sb::kManifestFile, "{not json");
    EXPECT_EQ(sb::startupCheck(dir, 2000, digest, report), sb::StartupCheck::Mismatch);

    // the Docker image's format-1 restore: left to its entrypoint, nothing written
    dir = freshDir("check_format1");
    touch(dir / sb::kManifestFile, "{\"file\": \"credits-pruned-2000.tar.zst\", \"sha256\": \"" + std::string(64, 'a') +
                                       "\", \"seq\": 2000, \"state_digest\": \"" + std::string(64, 'b') + "\"}");
    EXPECT_EQ(sb::startupCheck(dir, 2000, digest, report), sb::StartupCheck::NotFormat2);
    EXPECT_FALSE(fs::exists(dir / sb::kVerifiedFile));
    EXPECT_FALSE(fs::exists(dir / sb::kMismatchFile));
}

TEST(SnapshotBootstrap, CleanupOfAKilledImport) {
    const auto dir = freshDir("killed");
    const std::string db = (dir / "db").string();
    fs::create_directories(db);
    touch(fs::path(db) / ".lock", "");
    {
        auto opened = csdb::backend::open("lmdb", db);
        ASSERT_TRUE(opened);
    }
    ASSERT_TRUE(csdb::backend::writeMarker(db, "lmdb"));
    touch(dir / "caches" / "hashdb" / "data.mdb");
    touch(dir / "caches" / "seqdb" / "data.mdb");
    touch(dir / "caches" / "indexdb" / "data.mdb");
    touch(dir / "qs" / "2000.tmp" / "head.bin");
    touch(dir / "qs" / "2000" / "head.bin");
    touch(dir / sb::kManifestFile, "{}");
    std::string what;
    EXPECT_FALSE(sb::cleanupInterruptedImport(dir, db, what));   // no marker: nothing touched
    EXPECT_TRUE(fs::exists(dir / "caches" / "hashdb"));

    ASSERT_TRUE(sb::writeImportMarker(db, "lmdb", 2000));
    EXPECT_EQ(readAll(fs::path(db) / sb::kImportingFile), "lmdb 2000\n");
    ASSERT_TRUE(sb::cleanupInterruptedImport(dir, db, what));
    EXPECT_NE(what.find("caches/hashdb"), std::string::npos);
    EXPECT_NE(what.find("qs/2000.tmp"), std::string::npos);
    EXPECT_TRUE(csdb::backend::detect(db, true).empty());
    EXPECT_FALSE(fs::exists(fs::path(db) / csdb::backend::kMarkerFile));
    EXPECT_FALSE(fs::exists(fs::path(db) / sb::kImportingFile));
    EXPECT_FALSE(fs::exists(dir / "caches" / "hashdb"));
    EXPECT_FALSE(fs::exists(dir / "caches" / "seqdb"));
    EXPECT_FALSE(fs::exists(dir / "qs" / "2000.tmp"));
    EXPECT_FALSE(fs::exists(dir / "qs" / "2000"));
    EXPECT_FALSE(fs::exists(dir / sb::kManifestFile));
    // kept: the lock, the transaction index
    EXPECT_TRUE(fs::exists(fs::path(db) / ".lock"));
    EXPECT_TRUE(fs::exists(dir / "caches" / "indexdb" / "data.mdb"));
    // and the next start fetches again
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, db, "lmdb", false));
}

TEST(SnapshotBootstrap, SourcesRoundTrip) {
    const auto dir = freshDir("sources");
    std::vector<cs::SourceManifest> used(2);
    used[0].key.fill(1);
    used[0].address = "127.0.3.1";
    used[1].key.fill(2);
    used[1].address = "2001:db8::1";
    ASSERT_TRUE(sb::writeSources(dir, used));
    struct stat st {};
    ASSERT_EQ(::stat((dir / sb::kSourcesFile).c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);

    boost::property_tree::ptree tree;
    std::istringstream in(readAll(dir / sb::kSourcesFile));
    boost::property_tree::read_json(in, tree);
    std::vector<std::pair<std::string, std::string>> read;
    for (const auto& item : tree) {
        read.emplace_back(item.second.get<std::string>("key"), item.second.get<std::string>("address"));
    }
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0].first, EncodeBase58(used[0].key.data(), used[0].key.data() + used[0].key.size()));
    EXPECT_EQ(read[0].second, "127.0.3.1");
    EXPECT_EQ(read[1].second, "2001:db8::1");

    ASSERT_TRUE(sb::writeSources(dir, {}));
    EXPECT_EQ(readAll(dir / sb::kSourcesFile), "[]\n");
}

TEST(SnapshotBootstrap, StaleImportFilesAreRemovedOnAnEmptyDatabase) {
    const auto dir = freshDir("stale");
    touch(dir / sb::kManifestFile, "{}");
    touch(dir / sb::kVerifiedFile, "verified");
    touch(dir / sb::kSourcesFile, "[]");
    touch(dir / sb::kMismatchFile, "kept: it stops the node until an operator looks");
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, (dir / "db").string(), "lmdb", false));
    EXPECT_FALSE(fs::exists(dir / sb::kManifestFile));
    EXPECT_FALSE(fs::exists(dir / sb::kVerifiedFile));
    EXPECT_FALSE(fs::exists(dir / sb::kSourcesFile));
    EXPECT_TRUE(fs::exists(dir / sb::kMismatchFile));
}

TEST(SnapshotBootstrap, NotNeededWithARocksdbDatabase) {
    const auto dir = freshDir("rocksdb_db");
    const std::string db = (dir / "db").string();
    fs::create_directories(db);
    {
        auto opened = csdb::backend::open("rocksdb", db);
        ASSERT_TRUE(opened);
    }
    const auto r = sb::notNeeded(fetching(), dir, db, "rocksdb", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->why, sb::Skip::Database);
}

TEST(SnapshotBootstrap, CleanupOfAKilledRocksdbImport) {
    const auto dir = freshDir("killed_rocksdb");
    const std::string db = (dir / "db").string();
    fs::create_directories(db);
    touch(fs::path(db) / ".lock", "");
    {
        auto opened = csdb::backend::open("rocksdb", db);
        ASSERT_TRUE(opened);
    }
    touch(dir / "caches" / "hashdb" / "data.mdb");
    ASSERT_TRUE(sb::writeImportMarker(db, "rocksdb", 2000));
    std::string what;
    ASSERT_TRUE(sb::cleanupInterruptedImport(dir, db, what));
    EXPECT_TRUE(csdb::backend::detect(db, true).empty());
    EXPECT_TRUE(fs::exists(fs::path(db) / ".lock"));
    EXPECT_FALSE(fs::exists(dir / "caches" / "hashdb"));
    EXPECT_FALSE(sb::notNeeded(fetching(), dir, db, "rocksdb", false));
}

TEST(SnapshotBootstrap, CleanupWithADamagedMarker) {
    const auto dir = freshDir("killed_garbage");
    const std::string db = (dir / "db").string();
    fs::create_directories(db);
    touch(fs::path(db) / sb::kImportingFile, "\x01garbage");
    touch(dir / "caches" / "hashdb" / "data.mdb");
    std::string what;
    // the backend is unknown: the marker and the caches go, nothing else is guessed
    EXPECT_TRUE(sb::cleanupInterruptedImport(dir, db, what));
    EXPECT_FALSE(fs::exists(fs::path(db) / sb::kImportingFile));
    EXPECT_FALSE(fs::exists(dir / "caches" / "hashdb"));
}
