#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>

#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_manifest.hpp>
#include <csnode/snapshot_provider.hpp>
#include <csnode/statedigest.hpp>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using cs::snapshot_p2p::ManifestStatus;
using cs::snapshot_p2p::PieceStatus;

void putU16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

// a snapshot of block 200 built by buildSnapshot, with 64 KiB chunks and a checkpoint file that
// does not compress, so that it spans several chunks
struct Fixture {
    fs::path root;
    cs::PrivateKey key;
    cs::PublicKey pub;
    fs::path dir;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + 1000h;
    cs::SnapshotProvider provider{[this]() { return now; }};

    explicit Fixture(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root);
        EXPECT_TRUE(cscrypto::cryptoInit());
        key = cscrypto::generateKeyPair(pub);

        std::map<cs::Sequence, cs::Bytes> blocks;
        cs::Bytes head;
        csdb::PoolHash prev;
        for (cs::Sequence s = 0; s <= 200; ++s) {
            csdb::Pool p(prev, s);
            p.add_user_field(1, std::string(100 + s % 50, static_cast<char>('a' + s % 26)));
            EXPECT_TRUE(p.compose());
            blocks[s] = p.to_binary();
            head = p.hash().to_binary();
            prev = p.hash();
        }
        const auto qs = root / "qs" / "200";
        fs::create_directories(qs);
        {
            std::ofstream f(qs / "head.bin", std::ios::binary);
            const uint32_t magic = 0x44414548u;
            const uint64_t s = 200;
            f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            f.write(reinterpret_cast<const char*>(&s), sizeof(s));
            putU16(f, static_cast<uint16_t>(head.size()));
            f.write(reinterpret_cast<const char*>(head.data()), static_cast<std::streamsize>(head.size()));
            putU16(f, 0);
        }
        {
            std::mt19937 rng(7);
            std::string noise(300000, '\0');
            for (auto& c : noise) {
                c = static_cast<char>(rng());
            }
            std::ofstream(qs / "walletscache.dat", std::ios::binary) << noise;
        }
        std::ofstream(qs / "blockchain.dat", std::ios::binary) << "settings";
        cs::CheckpointDigest d;
        d.digest.fill(7);
        d.settings.fill(9);
        d.wallets = 42;
        EXPECT_TRUE(d.write(qs / cs::CheckpointDigest::kFileName));

        cs::SnapshotBuildInput in;
        in.seq = 200;
        in.checkpointDir = qs;
        in.snapshotBlocks = 150;
        in.nodeVersion = "test";
        in.chunkSize = 64 << 10;
        in.blockBytes = [&blocks](cs::Sequence s, cs::Bytes& out) {
            auto it = blocks.find(s);
            if (it == blocks.end()) return false;
            out = it->second;
            return true;
        };
        const auto r = cs::buildSnapshot(in, root / "snapshot", key);
        EXPECT_TRUE(r.ok) << r.error;
        dir = r.dir;
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    cs::SnapshotManifest manifest() const {
        std::ifstream f(dir / "snapshot.json", std::ios::binary);
        return *cs::SnapshotManifest::fromJson(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
    }
};

cs::PublicKey peerKey(uint8_t n) {
    cs::PublicKey k{};
    k.fill(n);
    return k;
}

cs::SnapshotProvider::Settings serving(size_t rateKBps = 0, size_t maxPeers = 8) {
    cs::SnapshotProvider::Settings s;
    s.serve = true;
    s.rateKBps = rateKBps;
    s.maxPeers = maxPeers;
    return s;
}

cs::SnapshotNodeState synced(bool trusted = false) {
    cs::SnapshotNodeState s;
    s.lastSeq = 1000;
    s.round = 1001;
    s.synced = true;
    s.trusted = trusted;
    return s;
}

cs::snapshot_p2p::PieceRequest piece(const std::string& name, uint32_t chunk, cs::Sequence seq = 200) {
    cs::snapshot_p2p::PieceRequest r;
    r.seq = seq;
    r.name = name;
    r.chunk = chunk;
    return r;
}
}  // namespace

TEST(SnapshotProvider, ManifestStatuses) {
    Fixture fx("snapshot_provider_status");
    const auto a = peerKey(1);

    auto off = fx.provider.onManifestRequest(a, cs::SnapshotProvider::Settings{}, synced());
    ASSERT_TRUE(off);
    EXPECT_EQ(off->status, ManifestStatus::NotServing);
    EXPECT_EQ(off->lastSeq, 1000u);

    auto building = fx.provider.onManifestRequest(a, serving(), synced());
    ASSERT_TRUE(building);
    EXPECT_EQ(building->status, ManifestStatus::Building);

    ASSERT_TRUE(fx.provider.load(fx.dir));
    EXPECT_EQ(fx.provider.loadedSeq(), 200u);

    cs::SnapshotNodeState behind = synced();
    behind.synced = false;
    auto notSynced = fx.provider.onManifestRequest(a, serving(), behind);
    ASSERT_TRUE(notSynced);
    EXPECT_EQ(notSynced->status, ManifestStatus::NotSynced);

    auto ready = fx.provider.onManifestRequest(a, serving(), synced(true));
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->status, ManifestStatus::Ready);
    EXPECT_TRUE(ready->servingTrusted);
    const auto parsed = cs::SnapshotManifest::fromJson(ready->manifestJson);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->verify());
    EXPECT_EQ(parsed->seq, 200u);

    // once per peer every 5 s
    EXPECT_FALSE(fx.provider.onManifestRequest(a, serving(), synced()));
    EXPECT_TRUE(fx.provider.onManifestRequest(peerKey(2), serving(), synced()));
    fx.now += 5s;
    EXPECT_TRUE(fx.provider.onManifestRequest(a, serving(), synced()));

    fx.provider.unload();
    EXPECT_FALSE(fx.provider.loadedSeq());
}

TEST(SnapshotProvider, EveryChunkMatchesTheManifest) {
    Fixture fx("snapshot_provider_chunks");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    const auto m = fx.manifest();
    bool sawShortLast = false;
    bool sawSeveralChunks = false;
    uint8_t n = 1;
    for (const auto& f : m.files) {
        sawSeveralChunks = sawSeveralChunks || f.chunks.size() > 2;
        for (uint32_t c = 0; c < f.chunks.size(); ++c) {
            fx.now += 1s;   // well inside every limit
            const auto r = fx.provider.onPieceRequest(peerKey(n), piece(f.name, c), serving(), synced());
            ASSERT_EQ(r.status, PieceStatus::Ok) << f.name << " " << c;
            EXPECT_EQ(cs::sha256Hex(r.data.data(), r.data.size()), f.chunks[c]) << f.name << " " << c;
            if (c + 1 == f.chunks.size() && r.data.size() < m.chunkSize) {
                sawShortLast = true;
            }
            EXPECT_EQ(r.name, f.name);
            EXPECT_EQ(r.chunk, c);
        }
    }
    EXPECT_TRUE(sawSeveralChunks);
    EXPECT_TRUE(sawShortLast);
}

TEST(SnapshotProvider, UnknownRequests) {
    Fixture fx("snapshot_provider_unknown");
    const auto a = peerKey(1);
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("blocks.bin.lz4", 0), serving(), synced()).status, PieceStatus::NotServing);
    ASSERT_TRUE(fx.provider.load(fx.dir));
    const auto m = fx.manifest();
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("blocks.bin.lz4", 0, 199), serving(), synced()).status, PieceStatus::Unknown);
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("contracts.bin.lz4", 0), serving(), synced()).status, PieceStatus::Unknown);
    const auto chunks = static_cast<uint32_t>(m.file("blocks.bin.lz4")->chunks.size());
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("blocks.bin.lz4", chunks), serving(), synced()).status, PieceStatus::Unknown);
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("blocks.bin.lz4", 0), cs::SnapshotProvider::Settings{}, synced()).status,
              PieceStatus::NotServing);
    cs::SnapshotNodeState behind = synced();
    behind.synced = false;
    const auto r = fx.provider.onPieceRequest(a, piece("blocks.bin.lz4", 0), serving(), behind);
    EXPECT_EQ(r.status, PieceStatus::Busy);
    EXPECT_EQ(r.retryMs, cs::SnapshotProvider::kMaxRetryMs);
}

TEST(SnapshotProvider, TwoPiecesPerPeerEvery100ms) {
    Fixture fx("snapshot_provider_window");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    const auto a = peerKey(1);
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("qs-walletscache.dat.lz4", 0), serving(), synced()).status, PieceStatus::Ok);
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("qs-walletscache.dat.lz4", 1), serving(), synced()).status, PieceStatus::Ok);
    fx.now += 40ms;
    const auto third = fx.provider.onPieceRequest(a, piece("qs-walletscache.dat.lz4", 2), serving(), synced());
    EXPECT_EQ(third.status, PieceStatus::Busy);
    EXPECT_GT(third.retryMs, 0u);
    EXPECT_LE(third.retryMs, 60u);
    // another peer is not held up
    EXPECT_EQ(fx.provider.onPieceRequest(peerKey(2), piece("qs-walletscache.dat.lz4", 2), serving(), synced()).status, PieceStatus::Ok);
    fx.now += 60ms;
    EXPECT_EQ(fx.provider.onPieceRequest(a, piece("qs-walletscache.dat.lz4", 2), serving(), synced()).status, PieceStatus::Ok);
}

namespace {
// pieces of 64 KiB a peer gets, two every 100 ms, before the first busy answer
size_t piecesUntilBusy(Fixture& fx, const cs::PublicKey& peer, size_t rateKBps, bool trusted, uint32_t* retryMs = nullptr) {
    for (size_t n = 0; n < 1000; ++n) {
        if (n % 2 == 0) {
            fx.now += 100ms;
        }
        const auto r = fx.provider.onPieceRequest(peer, piece("qs-walletscache.dat.lz4", 0), serving(rateKBps), synced(trusted));
        if (r.status == PieceStatus::Busy) {
            if (retryMs) {
                *retryMs = r.retryMs;
            }
            return n;
        }
        EXPECT_EQ(r.status, PieceStatus::Ok);
    }
    return 1000;
}
}  // namespace

TEST(SnapshotProvider, PerPeerAndGlobalBudget) {
    Fixture fx("snapshot_provider_budget");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    // 100 KB/s: a peer may take half (50 KB/s) after a burst of 2 MiB, 32 pieces of 64 KiB
    uint32_t retry = 0;
    const auto first = piecesUntilBusy(fx, peerKey(1), 100, false, &retry);
    EXPECT_GE(first, 32u);
    EXPECT_LE(first, 36u);
    EXPECT_GT(retry, 0u);
    EXPECT_LE(retry, cs::SnapshotProvider::kMaxRetryMs);
    // a second peer still gets its share of what the global burst has left (4 MiB - 2 MiB)
    const auto second = piecesUntilBusy(fx, peerKey(2), 100, false);
    EXPECT_GE(second, 28u);
    // and then the global bucket is empty for everyone
    EXPECT_LE(piecesUntilBusy(fx, peerKey(3), 100, false), 3u);
}

TEST(SnapshotProvider, TrustedHalvesTheBudget) {
    Fixture fx("snapshot_provider_trusted");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    const auto n = piecesUntilBusy(fx, peerKey(1), 100, true);
    EXPECT_GE(n, 16u);
    EXPECT_LE(n, 18u);
}

TEST(SnapshotProvider, MaxPeers) {
    Fixture fx("snapshot_provider_maxpeers");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    EXPECT_EQ(fx.provider.onPieceRequest(peerKey(1), piece("blocks.bin.lz4", 0), serving(0, 1), synced()).status, PieceStatus::Ok);
    const auto r = fx.provider.onPieceRequest(peerKey(2), piece("blocks.bin.lz4", 0), serving(0, 1), synced());
    EXPECT_EQ(r.status, PieceStatus::Busy);
    EXPECT_EQ(r.retryMs, cs::SnapshotProvider::kMaxRetryMs);
    fx.now += 31s;
    EXPECT_EQ(fx.provider.onPieceRequest(peerKey(2), piece("blocks.bin.lz4", 0), serving(0, 1), synced()).status, PieceStatus::Ok);
}

TEST(SnapshotProvider, PeerTableEvictsTheOldest) {
    Fixture fx("snapshot_provider_table");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    EXPECT_TRUE(fx.provider.onManifestRequest(peerKey(0), serving(), synced()));
    EXPECT_FALSE(fx.provider.onManifestRequest(peerKey(0), serving(), synced()));
    for (uint8_t i = 1; i <= cs::SnapshotProvider::kMaxPeers; ++i) {
        fx.now += 1ms;
        EXPECT_TRUE(fx.provider.onManifestRequest(peerKey(i), serving(), synced()));
    }
    // peer 0 was the oldest of 65: forgotten, so answered again within its 5 s
    EXPECT_TRUE(fx.provider.onManifestRequest(peerKey(0), serving(), synced()));
}

TEST(SnapshotProvider, CompletedPeerAndSummary) {
    Fixture fx("snapshot_provider_complete");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    int completed = 0;
    fx.provider.onPeerCompleted = [&](const cs::PublicKey&, cs::Sequence seq, uint64_t, std::chrono::seconds) {
        EXPECT_EQ(seq, 200u);
        ++completed;
    };
    for (const auto& f : fx.manifest().files) {
        for (uint32_t c = 0; c < f.chunks.size(); ++c) {
            fx.now += 1s;
            ASSERT_EQ(fx.provider.onPieceRequest(peerKey(1), piece(f.name, c), serving(), synced()).status, PieceStatus::Ok);
        }
    }
    EXPECT_EQ(completed, 1);
    EXPECT_NE(fx.provider.takeSummary().find("1 peers"), std::string::npos);
    EXPECT_TRUE(fx.provider.takeSummary().empty());
}

TEST(SnapshotProvider, ATamperedFileIsNotLoaded) {
    Fixture fx("snapshot_provider_tampered");
    ASSERT_TRUE(fx.provider.load(fx.dir));
    std::ofstream(fx.dir / "blocks.bin.lz4", std::ios::binary | std::ios::app) << "x";
    std::string error;
    EXPECT_FALSE(fx.provider.load(fx.dir, &error));
    EXPECT_NE(error.find("size"), std::string::npos) << error;
    EXPECT_EQ(fx.provider.loadedSeq(), 200u);   // the one loaded before stays
    EXPECT_FALSE(fx.provider.load(fx.root / "snapshot" / "not-a-number"));
}
