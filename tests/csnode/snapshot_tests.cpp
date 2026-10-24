#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_manifest.hpp>
#include <csnode/statedigest.hpp>

namespace {
namespace fs = std::filesystem;

struct Chain {
    std::map<cs::Sequence, cs::Bytes> blocks;
    std::map<cs::Sequence, cs::Bytes> hashes;
};

Chain makeChain(cs::Sequence count) {
    Chain c;
    csdb::PoolHash prev;
    for (cs::Sequence s = 0; s < count; ++s) {
        csdb::Pool p(prev, s);
        p.add_user_field(1, std::string(100 + s % 50, static_cast<char>('a' + s % 26)));
        EXPECT_TRUE(p.compose());
        c.blocks[s] = p.to_binary();
        c.hashes[s] = p.hash().to_binary();
        prev = p.hash();
    }
    return c;
}

void putU16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

// a checkpoint directory as CachesSerializationManager writes it, with the files the builder reads
void makeCheckpoint(const fs::path& dir, cs::Sequence seq, const cs::Bytes& headHash, bool withDigest = true) {
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "head.bin", std::ios::binary);
        const uint32_t magic = 0x44414548u;
        const uint64_t s = seq;
        f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        f.write(reinterpret_cast<const char*>(&s), sizeof(s));
        putU16(f, static_cast<uint16_t>(headHash.size()));
        f.write(reinterpret_cast<const char*>(headHash.data()), static_cast<std::streamsize>(headHash.size()));
        putU16(f, 0);
    }
    std::ofstream(dir / "walletscache.dat", std::ios::binary) << std::string(300000, 'w') << "end";
    std::ofstream(dir / "blockchain.dat", std::ios::binary) << "settings";
    if (withDigest) {
        cs::CheckpointDigest d;
        d.digest.fill(7);
        d.settings.fill(9);
        d.wallets = 42;
        ASSERT_TRUE(d.write(dir / cs::CheckpointDigest::kFileName));
    }
}

struct Fixture {
    fs::path root;
    Chain chain = makeChain(301);
    cs::PrivateKey key;
    cs::PublicKey pub;
    std::map<cs::Bytes, cs::Bytes> contracts = {{{1, 2}, {3}}, {{0, 9}, {4, 5}}};

    explicit Fixture(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root);
        key = cscrypto::generateKeyPair(pub);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    cs::SnapshotBuildInput input(cs::Sequence seq = 200) {
        cs::SnapshotBuildInput in;
        in.seq = seq;
        in.checkpointDir = root / "qs" / std::to_string(seq);
        in.firstStored = 0;
        in.snapshotBlocks = 150;
        in.nodeVersion = "test";
        in.chunkSize = 64 << 10;
        in.blockBytes = [this](cs::Sequence s, cs::Bytes& out) {
            auto it = chain.blocks.find(s);
            if (it == chain.blocks.end()) return false;
            out = it->second;
            return true;
        };
        in.contracts = [this](const std::function<bool(const cs::Bytes&, const cs::Bytes&)>& visit) {
            for (const auto& [k, v] : contracts) {
                if (!visit(k, v)) return false;
            }
            return true;
        };
        return in;
    }
};

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
}  // namespace

TEST(SnapshotBuilder, BuildsAVerifiableSnapshot) {
    Fixture fx("snapshot_build_ok");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    fs::create_directories(fx.root / "out" / "100");   // an older snapshot: removed
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_FALSE(fs::exists(fx.root / "out" / "100"));
    EXPECT_FALSE(fs::exists(fx.root / "out" / "200.tmp"));

    const auto parsed = cs::SnapshotManifest::fromJson(readAll(r.dir / "snapshot.json"));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->seq, 200u);
    EXPECT_EQ(parsed->firstBlock, 50u);
    EXPECT_EQ(parsed->wallets, 42u);
    std::string sevens, nines;
    for (int i = 0; i < 32; ++i) {
        sevens += "07";
        nines += "09";
    }
    EXPECT_EQ(parsed->stateDigest, sevens);
    EXPECT_EQ(parsed->settingsDigest, nines);
    EXPECT_TRUE(parsed->verify());

    // every file and chunk hash matches what was written
    for (const auto& f : parsed->files) {
        const auto data = readAll(r.dir / f.name);
        ASSERT_EQ(data.size(), f.size) << f.name;
        EXPECT_EQ(cs::sha256Hex(reinterpret_cast<const uint8_t*>(data.data()), data.size()), f.sha256) << f.name;
        for (size_t c = 0; c < f.chunks.size(); ++c) {
            const size_t off = c * parsed->chunkSize;
            const size_t len = std::min<size_t>(parsed->chunkSize, data.size() - off);
            EXPECT_EQ(cs::sha256Hex(reinterpret_cast<const uint8_t*>(data.data() + off), len), f.chunks[c]) << f.name << " chunk " << c;
        }
    }

    // blocks 50..200 in order, byte for byte
    cs::Sequence expected = 50;
    ASSERT_TRUE(cs::forEachSnapshotBlock(r.dir / "blocks.bin.lz4", [&](cs::Sequence s, cs::Bytes&& bytes) {
        EXPECT_EQ(s, expected);
        EXPECT_EQ(bytes, fx.chain.blocks[s]);
        ++expected;
        return true;
    }));
    EXPECT_EQ(expected, 201u);

    // a checkpoint file survives compression
    std::string wallets;
    ASSERT_TRUE(cs::snapshot_lz4::decompressFile(r.dir / "qs-walletscache.dat.lz4", [&](const uint8_t* d, size_t n) {
        wallets.append(reinterpret_cast<const char*>(d), n);
        return true;
    }));
    EXPECT_EQ(wallets, readAll(fx.root / "qs" / "200" / "walletscache.dat"));
    EXPECT_NE(parsed->file("contracts.bin.lz4"), nullptr);
}

TEST(SnapshotBuilder, FirstBlockIsBoundedByTheStorage) {
    Fixture fx("snapshot_build_first");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    auto in = fx.input();
    in.firstStored = 120;
    const auto r = cs::buildSnapshot(in, fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.manifest.firstBlock, 120u);
}

TEST(SnapshotBuilder, BrokenChainBuildsNothing) {
    Fixture fx("snapshot_build_chain");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    fx.chain.blocks[150] = fx.chain.blocks[149];   // block 150 replaced: it does not parse as 150
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(fs::exists(fx.root / "out" / "200"));
    EXPECT_FALSE(fs::exists(fx.root / "out" / "200.tmp"));
}

TEST(SnapshotBuilder, AnotherHeadBuildsNothing) {
    Fixture fx("snapshot_build_head");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[199]);
    EXPECT_FALSE(cs::buildSnapshot(fx.input(), fx.root / "out", fx.key).ok);
}

TEST(SnapshotBuilder, CheckpointWithoutDigestBuildsNothing) {
    Fixture fx("snapshot_build_nodigest");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200], false);
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("state_digest"), std::string::npos);
}

TEST(SnapshotBuilder, StopLeavesNothing) {
    Fixture fx("snapshot_build_stop");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    std::atomic<bool> stop{true};
    auto in = fx.input();
    in.stop = &stop;
    EXPECT_FALSE(cs::buildSnapshot(in, fx.root / "out", fx.key).ok);
    EXPECT_FALSE(fs::exists(fx.root / "out" / "200.tmp"));
}

TEST(SnapshotManifest, TamperedFieldsDoNotVerify) {
    Fixture fx("snapshot_manifest_tamper");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    const auto& m = r.manifest;
    ASSERT_TRUE(m.verify());
    auto t = m; t.seq += 1; EXPECT_FALSE(t.verify());
    t = m; t.headHash[0] = t.headHash[0] == 'a' ? 'b' : 'a'; EXPECT_FALSE(t.verify());
    t = m; t.stateDigest[0] = t.stateDigest[0] == 'a' ? 'b' : 'a'; EXPECT_FALSE(t.verify());
    t = m; t.settingsDigest[0] = t.settingsDigest[0] == 'a' ? 'b' : 'a'; EXPECT_FALSE(t.verify());
    t = m; t.wallets += 1; EXPECT_FALSE(t.verify());
    t = m; t.firstBlock += 1; EXPECT_FALSE(t.verify());
    t = m; t.files[0].sha256[0] = t.files[0].sha256[0] == 'a' ? 'b' : 'a'; EXPECT_FALSE(t.verify());
    t = m; t.files[0].size += 1; EXPECT_FALSE(t.verify());
}

TEST(SnapshotManifest, JsonRoundTripAndRejections) {
    Fixture fx("snapshot_manifest_json");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    const auto json = r.manifest.toJson();
    std::string error;
    const auto back = cs::SnapshotManifest::fromJson(json, &error);
    ASSERT_TRUE(back.has_value()) << error;
    EXPECT_EQ(back->toJson(), json);

    auto replaced = [&](const std::string& from, const std::string& to) {
        auto j = json;
        const auto at = j.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        return j.replace(at, from.size(), to);
    };
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(replaced("\"format\": 2", "\"format\": 3")).has_value());
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(replaced("\"blocks.bin.lz4\"", "\"../blocks.bin.lz4\"")).has_value());
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(replaced("\"compression\": \"lz4\"", "\"compression\": \"zip\"")).has_value());
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(replaced("\"chunk_size\": 65536", "\"chunk_size\": 1073741824")).has_value());
    EXPECT_FALSE(cs::SnapshotManifest::fromJson("not json").has_value());
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(std::string(2 << 20, ' ')).has_value());
}

TEST(SnapshotManifest, OnlyKnownNames) {
    EXPECT_TRUE(cs::SnapshotManifest::isKnownFileName("blocks.bin.lz4"));
    EXPECT_TRUE(cs::SnapshotManifest::isKnownFileName("qs-walletscache.dat.lz4"));
    EXPECT_FALSE(cs::SnapshotManifest::isKnownFileName("qs-../../NodePrivate.txt.lz4"));
    EXPECT_FALSE(cs::SnapshotManifest::isKnownFileName("NodePrivate.txt"));
    EXPECT_FALSE(cs::SnapshotManifest::isKnownFileName("qs-config.ini.lz4"));
}

TEST(SnapshotBuilder, RemovesOnlySnapshotFolders) {
    Fixture fx("snapshot_build_only_snapshots");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    fs::create_directories(fx.root / "out" / "100");                  // a numbered folder that is not a snapshot
    std::ofstream(fx.root / "out" / "100" / "walletscache.dat") << "x";
    fs::create_directories(fx.root / "out" / "150");                  // an earlier snapshot
    std::ofstream(fx.root / "out" / "150" / "snapshot.json") << "{}";
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(fs::exists(fx.root / "out" / "100" / "walletscache.dat"));
    EXPECT_FALSE(fs::exists(fx.root / "out" / "150"));
}

TEST(SnapshotBuilder, RefusesToReplaceAFolderThatIsNotASnapshot) {
    Fixture fx("snapshot_build_refuse");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    // the output root is the checkpoints folder itself: qs/200 must survive
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "qs", fx.key);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(fs::exists(fx.root / "qs" / "200" / "walletscache.dat"));
    EXPECT_TRUE(fs::exists(fx.root / "qs" / "200" / "head.bin"));
}

TEST(SnapshotManifest, DeepNestingAndLargeBlockNumbersRejected) {
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(std::string(1000, '[') + std::string(1000, ']')).has_value());
    Fixture fx("snapshot_manifest_seq");
    makeCheckpoint(fx.root / "qs" / "200", 200, fx.chain.hashes[200]);
    const auto r = cs::buildSnapshot(fx.input(), fx.root / "out", fx.key);
    ASSERT_TRUE(r.ok) << r.error;
    auto big = r.manifest;
    big.seq = 1ull << 33;
    big.sign(fx.key);
    EXPECT_FALSE(cs::SnapshotManifest::fromJson(big.toJson()).has_value());
    auto chunks = r.manifest;
    chunks.files[0].chunks[0][0] = chunks.files[0].chunks[0][0] == 'a' ? 'b' : 'a';
    EXPECT_FALSE(chunks.verify());
}
