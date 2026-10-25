#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <base58.h>
#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_fetcher.hpp>
#include <csnode/snapshot_manifest.hpp>
#include <csnode/snapshot_provider.hpp>
#include <csnode/statedigest.hpp>
#include <net/hostdata.hpp>

namespace {
namespace fs = std::filesystem;
using cs::SnapshotFetcher;
using cs::snapshot_p2p::ManifestReply;
using cs::snapshot_p2p::ManifestRequest;
using cs::snapshot_p2p::PieceReply;
using cs::snapshot_p2p::PieceRequest;
using cs::snapshot_p2p::PieceStatus;
using Phase = SnapshotFetcher::Phase;

const std::string kWallets = "qs-walletscache.dat.lz4";
const std::string kBlocks = "blocks.bin.lz4";

std::string fileText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void putU16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

// Five snapshots of block 200 built by buildSnapshot from one generated chain, each signed by its
// own key, with 64 KiB chunks: A1 and A2 from the same checkpoint files (identical bytes), B and
// E with one and two trailing bytes in walletscache.dat (same digest, another sha256 for that
// file), D with another state digest. Built once for all tests.
struct Snapshots {
    enum { A1, A2, B, D, E, Count };

    struct Source {
        cs::PrivateKey priv;
        cs::PublicKey pub{};
        fs::path dir;
        cs::SnapshotManifest manifest;
        std::string json;
    };

    fs::path root;
    std::array<Source, Count> src;

    Snapshots() : root(fs::temp_directory_path() / "snapshot_fetcher_sources") {
        fs::remove_all(root);
        fs::create_directories(root);
        EXPECT_TRUE(cscrypto::cryptoInit());

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
        std::mt19937 rng(11);
        std::string noise(1'000'000, '\0');
        for (auto& c : noise) {
            c = static_cast<char>(rng());
        }

        auto checkpoint = [&](const std::string& name, size_t extra, uint8_t digestFill) {
            const auto qs = root / ("qs-" + name) / "200";
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
            std::ofstream(qs / "walletscache.dat", std::ios::binary) << noise << std::string(extra, 'x');
            std::ofstream(qs / "blockchain.dat", std::ios::binary) << "settings";
            cs::CheckpointDigest d;
            d.digest.fill(digestFill);
            d.settings.fill(9);
            d.wallets = 42;
            EXPECT_TRUE(d.write(qs / cs::CheckpointDigest::kFileName));
            return qs;
        };
        const auto qsA = checkpoint("a", 0, 7);
        const auto qsB = checkpoint("b", 1, 7);
        const auto qsD = checkpoint("d", 0, 8);
        const auto qsE = checkpoint("e", 2, 7);
        const std::array<fs::path, Count> qsOf{qsA, qsA, qsB, qsD, qsE};

        for (size_t i = 0; i < Count; ++i) {
            auto& s = src[i];
            s.priv = cscrypto::generateKeyPair(s.pub);
            cs::SnapshotBuildInput in;
            in.seq = 200;
            in.checkpointDir = qsOf[i];
            in.snapshotBlocks = 150;
            in.nodeVersion = "test";
            in.chunkSize = 64 << 10;
            in.blockBytes = [&blocks](cs::Sequence seq, cs::Bytes& out) {
                auto it = blocks.find(seq);
                if (it == blocks.end()) return false;
                out = it->second;
                return true;
            };
            const auto r = cs::buildSnapshot(in, root / ("out" + std::to_string(i)), s.priv);
            EXPECT_TRUE(r.ok) << r.error;
            s.dir = r.dir;
            s.json = fileText(s.dir / "snapshot.json");
            s.manifest = *cs::SnapshotManifest::fromJson(s.json);
        }
    }

    ~Snapshots() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

const Snapshots& snapshots() {
    static Snapshots s;
    return s;
}

cs::PublicKey otherKey(uint8_t n) {
    cs::PublicKey k{};
    k.fill(n);
    return k;
}

std::string base58(const cs::PublicKey& k) {
    return EncodeBase58(k.data(), k.data() + k.size());
}

// The joining node's network: the fetcher's hooks lead to real SnapshotProvider instances, one per
// source, that share the test's clock. step() delivers what the fetcher sent since the last step,
// advances the clock and ticks.
struct Net {
    struct Source {
        size_t index = 0;
        std::unique_ptr<cs::SnapshotProvider> provider;
        std::string address;
        bool stall = false;               // never answers a piece request
        bool corruptAll = false;          // flips a byte of every piece
        bool busyAll = false;             // answers every piece request busy
        uint32_t busyMs = 2000;
        std::optional<std::string> manifestJson;   // replaces the manifest it serves
        size_t silentManifests = 0;       // manifest requests left without an answer, from the first
        size_t manifestAsks = 0;
        size_t outstanding = 0;           // piece requests not answered yet
    };

    const cs::PublicKey me = otherKey(0xEE);
    uint64_t nowMs = 10'000'000;
    std::map<cs::PublicKey, Source> sources;
    SnapshotFetcher::Peers peers;
    std::vector<std::pair<cs::PublicKey, ManifestRequest>> manifestQueue;
    std::vector<std::pair<cs::PublicKey, PieceRequest>> pieceQueue;
    std::vector<std::set<cs::PublicKey>> addCalls;
    std::vector<std::string> logs;
    std::vector<std::tuple<uint64_t, cs::PublicKey, PieceRequest>> sent;
    std::vector<std::pair<uint64_t, cs::PublicKey>> busySent;   // busy replies: when, from whom
    size_t maxOutstandingTotal = 0;
    size_t maxOutstandingPerSource = 0;
    size_t outstandingTotal = 0;
    size_t manifestRequests = 0;
    std::map<cs::PublicKey, std::string> addresses;   // of peers that are not sources

    SnapshotFetcher::Hooks hooks() {
        SnapshotFetcher::Hooks h;
        h.sendManifestRequest = [this](const cs::PublicKey& k, const ManifestRequest& r) {
            manifestQueue.emplace_back(k, r);
            ++manifestRequests;
        };
        h.sendPieceRequest = [this](const cs::PublicKey& k, const PieceRequest& r) {
            pieceQueue.emplace_back(k, r);
            sent.emplace_back(nowMs, k, r);
            if (auto it = sources.find(k); it != sources.end()) {
                ++it->second.outstanding;
                maxOutstandingPerSource = std::max(maxOutstandingPerSource, it->second.outstanding);
            }
            ++outstandingTotal;
            maxOutstandingTotal = std::max(maxOutstandingTotal, outstandingTotal);
        };
        h.addNeighbours = [this](const std::set<cs::PublicKey>& keys) { addCalls.push_back(keys); };
        h.addressOf = [this](const cs::PublicKey& k) {
            auto it = sources.find(k);
            if (it != sources.end()) {
                return it->second.address;
            }
            auto other = addresses.find(k);
            return other == addresses.end() ? std::string() : other->second;
        };
        h.clock = [this]() { return nowMs; };
        h.onLog = [this](const std::string& line) { logs.push_back(line); };
        return h;
    }

    const cs::PublicKey& key(size_t index) const {
        return snapshots().src[index].pub;
    }

    Source& add(size_t index, bool neighbour = true) {
        const auto& k = key(index);
        auto& s = sources[k];
        s.index = index;
        s.address = "10.0." + std::to_string(index + 1) + ".1";
        s.provider = std::make_unique<cs::SnapshotProvider>(
            [this]() { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(nowMs); });
        EXPECT_TRUE(s.provider->load(snapshots().src[index].dir));
        peers.known.emplace_back(k, cs::hostdata::kSnapshotProtocolV1);
        if (neighbour) {
            peers.neighbours.insert(k);
        }
        return s;
    }

    static cs::SnapshotProvider::Settings serving() {
        cs::SnapshotProvider::Settings s;
        s.serve = true;
        s.rateKBps = 0;
        return s;
    }

    static cs::SnapshotNodeState synced() {
        cs::SnapshotNodeState s;
        s.lastSeq = 1000;
        s.round = 1001;
        s.synced = true;
        return s;
    }

    void deliver(SnapshotFetcher& fetcher) {
        auto manifests = std::move(manifestQueue);
        manifestQueue.clear();
        for (auto& [k, request] : manifests) {
            auto it = sources.find(k);
            if (it == sources.end()) {
                continue;
            }
            ++it->second.manifestAsks;
            if (it->second.silentManifests != 0) {
                --it->second.silentManifests;
                continue;
            }
            auto reply = it->second.provider->onManifestRequest(me, serving(), synced());
            if (!reply) {
                continue;
            }
            if (it->second.manifestJson && reply->status == cs::snapshot_p2p::ManifestStatus::Ready) {
                reply->manifestJson = *it->second.manifestJson;
            }
            fetcher.onManifestReply(k, *reply);
        }
        auto pieces = std::move(pieceQueue);
        pieceQueue.clear();
        for (auto& [k, request] : pieces) {
            auto it = sources.find(k);
            if (it == sources.end()) {
                continue;
            }
            auto& s = it->second;
            if (s.stall) {
                continue;
            }
            --s.outstanding;
            --outstandingTotal;
            PieceReply reply;
            if (s.busyAll) {
                reply.status = PieceStatus::Busy;
                reply.retryMs = s.busyMs;
                reply.seq = request.seq;
                reply.name = request.name;
                reply.chunk = request.chunk;
                busySent.emplace_back(nowMs, k);
            }
            else {
                reply = s.provider->onPieceRequest(me, request, serving(), synced());
                if (reply.status == PieceStatus::Busy) {
                    busySent.emplace_back(nowMs, k);
                }
                if (s.corruptAll && reply.status == PieceStatus::Ok && !reply.data.empty()) {
                    reply.data[0] ^= 0x5a;
                }
            }
            fetcher.onPieceReply(k, std::move(reply));
        }
    }

    void step(SnapshotFetcher& fetcher, uint64_t ms = 100) {
        deliver(fetcher);
        nowMs += ms;
        fetcher.tick(peers);
    }

    bool runUntil(SnapshotFetcher& fetcher, const std::function<bool()>& done, size_t maxSteps = 5000) {
        for (size_t i = 0; i < maxSteps; ++i) {
            if (done()) {
                return true;
            }
            step(fetcher);
        }
        return done();
    }

    bool logged(const std::string& text) const {
        return std::any_of(logs.begin(), logs.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
    }

    size_t countLogged(const std::string& text) const {
        return static_cast<size_t>(
            std::count_if(logs.begin(), logs.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; }));
    }

    size_t sentTo(const cs::PublicKey& k) const {
        return static_cast<size_t>(std::count_if(sent.begin(), sent.end(), [&](const auto& s) { return std::get<1>(s) == k; }));
    }
};

SnapshotData fetchConfig(size_t minSources = 2) {
    SnapshotData c;
    c.minSources = minSources;
    c.sourcesFile.clear();
    return c;
}

struct TmpDir {
    fs::path path;
    explicit TmpDir(const std::string& name) : path(fs::temp_directory_path() / ("snapshot_fetcher_" + name)) {
        fs::remove_all(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// the fetched files are the manifest's, byte for byte
void expectFilesOf(const fs::path& dir, const cs::SnapshotManifest& m) {
    for (const auto& f : m.files) {
        const auto bytes = fileText(dir / f.name);
        EXPECT_EQ(bytes.size(), f.size) << f.name;
        EXPECT_EQ(cs::sha256Hex(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), f.sha256) << f.name;
    }
}

bool isFetching(const SnapshotFetcher& f) {
    return f.phase() == Phase::Fetch;
}

}  // namespace

// (1) discovery: peers with bit 24, [snapshot] sources and sources_file keys, nothing else
TEST(SnapshotFetcher, DiscoveryAddsServingPeersAndConfiguredKeys) {
    TmpDir tmp("discovery");
    fs::create_directories(tmp.path);
    Net net;
    const auto trusted = otherKey(1);
    const auto listed = otherKey(2);
    const auto official = otherKey(3);
    const auto otherBit = otherKey(4);
    const auto sourcesFile = tmp.path / "snapshot_sources.txt";
    std::ofstream(sourcesFile) << "# serving nodes\n1.2.3.4:6000 " << base58(listed) << "\n\n5.6.7.8:6000 not-a-key\n";

    auto cfg = fetchConfig();
    cfg.sources = {trusted};
    cfg.sourcesFile = sourcesFile.string();
    net.add(Snapshots::A1, false);
    net.add(Snapshots::A2, false);
    net.peers.known.emplace_back(official, 0);
    net.peers.known.emplace_back(otherBit, 2);
    SnapshotFetcher fetcher(cfg, tmp.path / "tmp", net.hooks());

    fetcher.tick(net.peers);
    ASSERT_EQ(net.addCalls.size(), 1u);
    const std::set<cs::PublicKey> expected{trusted, listed, net.key(Snapshots::A1), net.key(Snapshots::A2)};
    EXPECT_EQ(net.addCalls[0], expected);
    EXPECT_TRUE(net.manifestQueue.empty());   // none is a neighbour yet
    EXPECT_EQ(fetcher.phase(), Phase::Discover);
    EXPECT_TRUE(net.logged("not a node key"));

    // once per key; a key that became a neighbour is asked for its manifest
    net.peers.neighbours.insert(net.key(Snapshots::A1));
    net.nowMs += 500;
    fetcher.tick(net.peers);
    EXPECT_EQ(net.addCalls.size(), 1u);
    ASSERT_EQ(net.manifestQueue.size(), 1u);
    EXPECT_EQ(net.manifestQueue[0].first, net.key(Snapshots::A1));
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_TRUE(net.logged("asking " + base58(net.key(Snapshots::A1))));

    // the others are brought in again after a minute; the neighbour is not
    net.nowMs += SnapshotFetcher::kNeighbourRetryMs;
    fetcher.tick(net.peers);
    ASSERT_EQ(net.addCalls.size(), 2u);
    EXPECT_EQ(net.addCalls[1].count(net.key(Snapshots::A1)), 0u);
    EXPECT_EQ(net.addCalls[1].size(), 3u);
}

// a flood of discovered keys from one /24 pushes out neither a configured key nor other networks' sources
TEST(SnapshotFetcher, FloodFromOnePrefixDoesNotFillTheTable) {
    TmpDir tmp("flood");
    Net net;
    const auto configured = otherKey(1);
    auto cfg = fetchConfig();
    cfg.sources = {configured};
    std::vector<cs::PublicKey> flood;
    for (int i = 0; i < 100; ++i) {
        cs::PublicKey k{};
        k.fill(0x40);
        k[0] = static_cast<uint8_t>(i);
        flood.push_back(k);
        net.addresses[k] = "10.9.9." + std::to_string(i + 1);
        net.peers.known.emplace_back(k, cs::hostdata::kSnapshotProtocolV1);
        net.peers.neighbours.insert(k);   // all of them neighbours that never answer
    }
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    SnapshotFetcher fetcher(cfg, tmp.path, net.hooks());
    fetcher.tick(net.peers);
    ASSERT_EQ(net.addCalls.size(), 1u);
    const auto& added = net.addCalls[0];
    EXPECT_EQ(added.count(configured), 1u);
    // the sources of other networks (neighbours already) are asked; of the flood at most kMaxPerPrefix
    std::set<cs::PublicKey> asked;
    size_t floodAsked = 0;
    for (const auto& [k, request] : net.manifestQueue) {
        asked.insert(k);
        floodAsked += std::count(flood.begin(), flood.end(), k);
    }
    for (auto i : {Snapshots::A1, Snapshots::A2, Snapshots::B}) {
        EXPECT_EQ(asked.count(net.key(i)), 1u);
    }
    EXPECT_EQ(floodAsked, SnapshotFetcher::kMaxPerPrefix);
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
}

// a full table makes room for a new network by dropping the oldest of the most represented one
TEST(SnapshotFetcher, FullTableMakesRoomForANewPrefix) {
    TmpDir tmp("fulltable");
    Net net;
    for (int p = 0; p < 16; ++p) {
        for (int i = 0; i < 4; ++i) {
            cs::PublicKey k{};
            k.fill(0x50);
            k[0] = static_cast<uint8_t>(p);
            k[1] = static_cast<uint8_t>(i);
            net.addresses[k] = "10." + std::to_string(100 + p) + ".0." + std::to_string(i + 1);
            net.peers.known.emplace_back(k, cs::hostdata::kSnapshotProtocolV1);
        }
    }
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    fetcher.tick(net.peers);
    ASSERT_EQ(net.addCalls.size(), 1u);
    EXPECT_EQ(net.addCalls[0].size(), SnapshotFetcher::kMaxSources);
    // two serving nodes from other networks turn up later
    net.add(Snapshots::A1, false);
    net.add(Snapshots::A2, false);
    net.nowMs += 500;
    fetcher.tick(net.peers);
    ASSERT_EQ(net.addCalls.size(), 2u);
    EXPECT_EQ(net.addCalls[1].count(net.key(Snapshots::A1)), 1u);
    EXPECT_EQ(net.addCalls[1].count(net.key(Snapshots::A2)), 1u);
    net.peers.neighbours.insert(net.key(Snapshots::A1));
    net.peers.neighbours.insert(net.key(Snapshots::A2));
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
}

// 64 keys over 64 networks that never answer do not keep a new honest peer out
TEST(SnapshotFetcher, SilentSpreadKeysDoNotLockNewcomersOut) {
    TmpDir tmp("spreadkeys");
    Net net;
    for (int p = 0; p < 64; ++p) {
        cs::PublicKey k{};
        k.fill(0x60);
        k[0] = static_cast<uint8_t>(p);
        net.addresses[k] = std::to_string(20 + p) + "." + std::to_string(p) + ".0.1";
        net.peers.known.emplace_back(k, cs::hostdata::kSnapshotProtocolV1);
        net.peers.neighbours.insert(k);
    }
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    net.step(fetcher);
    // a full table makes room at once, before anything has had time to answer
    net.add(Snapshots::A1, false);
    net.step(fetcher);
    ASSERT_FALSE(net.addCalls.empty());
    EXPECT_EQ(net.addCalls.back().count(net.key(Snapshots::A1)), 1u);
    net.peers.neighbours.insert(net.key(Snapshots::A1));
    for (int i = 0; i < 610; ++i) {   // the others asked, never answering, for a minute
        net.step(fetcher);
    }
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_EQ(fetcher.manifests(), 1u);
    net.add(Snapshots::A2);
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
}

// an address not known yet: no cap at discovery, no vote until it is known; the cap is applied
// to the address resolved when the manifest arrives
TEST(SnapshotFetcher, UnknownAddresses) {
    TmpDir tmp("unknownaddr");
    Net net;
    for (auto i : {Snapshots::A1, Snapshots::A2, Snapshots::B, Snapshots::D, Snapshots::E}) {
        net.add(i).address.clear();
    }
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    fetcher.tick(net.peers);
    EXPECT_EQ(net.manifestQueue.size(), 5u);   // five unknown addresses, none refused
    net.step(fetcher);
    EXPECT_EQ(fetcher.manifests(), 5u);
    for (int i = 0; i < 10; ++i) {
        net.step(fetcher);
    }
    EXPECT_EQ(fetcher.phase(), Phase::Collect);   // nothing counts without an address
    EXPECT_FALSE(fetcher.groups().empty());
    EXPECT_EQ(fetcher.groups()[0].independent, 0u);

    // all five turn out to be in one /24: four are kept, they count once
    int n = 1;
    for (auto& [key, source] : net.sources) {
        source.address = "10.0.7." + std::to_string(n++);
    }
    net.nowMs += SnapshotFetcher::kManifestIntervalMs;
    fetcher.tick(net.peers);
    net.deliver(fetcher);
    EXPECT_EQ(fetcher.manifests(), 4u);
    EXPECT_TRUE(net.logged("too many sources at 10.0.7"));
    fetcher.tick(net.peers);
    EXPECT_EQ(fetcher.phase(), Phase::Collect);   // one network: one source

    // known in different networks, they agree
    n = 1;
    for (auto& [key, source] : net.sources) {
        source.address = "10.0." + std::to_string(n++) + ".1";
    }
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
}

// (2) bad signature, another node's manifest, an oversized chunk, an unsolicited reply: dropped
TEST(SnapshotFetcher, BadManifestsAreDropped) {
    TmpDir tmp("badmanifests");
    Net net;
    const auto& snaps = snapshots();
    auto& a1 = net.add(Snapshots::A1);
    auto& a2 = net.add(Snapshots::A2);
    auto& b = net.add(Snapshots::B);

    auto forged = snaps.src[Snapshots::A1].manifest;
    forged.wallets += 1;   // signature no longer matches
    a1.manifestJson = forged.toJson();
    a2.manifestJson = snaps.src[Snapshots::A1].json;   // relayed: signed by A1
    auto big = snaps.src[Snapshots::B].manifest;
    big.chunkSize = 2u << 20;
    for (auto& f : big.files) {
        f.chunks.assign(f.size == 0 ? 0 : 1, f.sha256);
    }
    big.sign(snaps.src[Snapshots::B].priv);
    ASSERT_TRUE(cs::SnapshotManifest::fromJson(big.toJson()));
    b.manifestJson = big.toJson();

    SnapshotFetcher fetcher(fetchConfig(1), tmp.path, net.hooks());
    fetcher.onManifestReply(net.key(Snapshots::A1), ManifestReply{});   // not asked yet
    EXPECT_EQ(fetcher.dropped(), 1u);
    fetcher.tick(net.peers);
    net.step(fetcher);
    EXPECT_EQ(fetcher.manifests(), 0u);
    EXPECT_EQ(fetcher.dropped(), 4u);
    EXPECT_TRUE(net.logged("dropped: signature does not verify"));
    EXPECT_TRUE(net.logged("dropped: signed by another node"));
    EXPECT_TRUE(net.logged("dropped: chunk size"));
    EXPECT_EQ(fetcher.phase(), Phase::Collect);

    // the same sources with their own manifests are taken at the next request (30 s later)
    a1.manifestJson.reset();
    net.nowMs += SnapshotFetcher::kManifestIntervalMs;
    fetcher.tick(net.peers);
    net.deliver(fetcher);
    EXPECT_EQ(fetcher.manifests(), 1u);
    fetcher.tick(net.peers);   // agreement is tried at the tick
    EXPECT_NE(fetcher.phase(), Phase::Collect);   // min_sources=1: agreed
}

// (3) three sources of four agree, the fourth (another digest) is reported as minority
TEST(SnapshotFetcher, AgreementOfThreeOfFour) {
    TmpDir tmp("agreement");
    Net net;
    net.add(Snapshots::D);
    SnapshotFetcher fetcher(fetchConfig(3), tmp.path, net.hooks());
    net.step(fetcher);
    net.step(fetcher);
    EXPECT_EQ(fetcher.manifests(), 1u);
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() != Phase::Collect; }, 20));
    EXPECT_EQ(fetcher.phase(), Phase::Fetch);

    const auto groups = fetcher.groups();
    ASSERT_EQ(groups.size(), 2u);
    const auto chosen = std::find_if(groups.begin(), groups.end(), [](const auto& g) { return g.chosen; });
    ASSERT_NE(chosen, groups.end());
    EXPECT_EQ(chosen->independent, 3u);
    const std::set<cs::PublicKey> keys(chosen->keys.begin(), chosen->keys.end());
    EXPECT_EQ(keys, (std::set<cs::PublicKey>{net.key(Snapshots::A1), net.key(Snapshots::A2), net.key(Snapshots::B)}));
    EXPECT_TRUE(net.logged("minority: block 200"));
    EXPECT_EQ(net.sentTo(net.key(Snapshots::D)), 0u);
}

// (4) pieces go round-robin, at most 2 per source and 8 in all; each file only from sources that
// declare its bytes; the result is the files of the largest group
TEST(SnapshotFetcher, PiecesSpreadAndBounded) {
    TmpDir tmp("spread");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    net.add(Snapshots::E);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher); }, 20));

    // the first round already uses every source that can serve
    std::set<cs::PublicKey> first;
    for (const auto& s : net.sent) {
        first.insert(std::get<1>(s));
    }
    EXPECT_EQ(first.size(), 4u);

    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_LE(net.maxOutstandingPerSource, SnapshotFetcher::kPiecesPerSource);
    EXPECT_LE(net.maxOutstandingTotal, SnapshotFetcher::kPiecesTotal);
    EXPECT_EQ(net.maxOutstandingTotal, SnapshotFetcher::kPiecesTotal);

    const auto result = fetcher.result();
    ASSERT_TRUE(result);
    const auto& m = result->first;
    EXPECT_EQ(m.filesHash(), snapshots().src[Snapshots::A1].manifest.filesHash());
    EXPECT_EQ(result->second.size(), 4u);
    expectFilesOf(tmp.path, m);
    for (const auto& s : net.sent) {
        const auto& k = std::get<1>(s);
        if (std::get<2>(s).name == kWallets) {
            EXPECT_TRUE(k == net.key(Snapshots::A1) || k == net.key(Snapshots::A2));
        }
    }
    // B and E, whose walletscache differs, served the files they share with A
    EXPECT_GT(net.sentTo(net.key(Snapshots::B)), 0u);
    EXPECT_GT(net.sentTo(net.key(Snapshots::E)), 0u);
    EXPECT_TRUE(net.logged("SNAPSHOT: fetching block 200: 100 %"));
    EXPECT_TRUE(net.logged("SNAPSHOT: fetched block 200"));
}

// (5) a corrupt piece marks its source for the bootstrap; the piece comes from another source
TEST(SnapshotFetcher, CorruptPieceMarksTheSource) {
    TmpDir tmp("corrupt");
    Net net;
    net.add(Snapshots::A1).corruptAll = true;
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_TRUE(net.logged("rejected piece"));
    EXPECT_TRUE(net.logged(base58(net.key(Snapshots::A1)) + " sent a piece that does not match"));
    EXPECT_LE(net.sentTo(net.key(Snapshots::A1)), SnapshotFetcher::kPiecesPerSource);
    expectFilesOf(tmp.path, fetcher.result()->first);
}

// (6) a stalled source times out after 30 s, its pieces go elsewhere, it is marked
TEST(SnapshotFetcher, StalledSourceTimesOut) {
    TmpDir tmp("stall");
    Net net;
    net.add(Snapshots::A1).stall = true;
    net.add(Snapshots::A2);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher); }, 20));
    const uint64_t start = net.nowMs;
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_GE(net.nowMs - start, SnapshotFetcher::kPieceTimeoutMs);
    EXPECT_TRUE(net.logged(base58(net.key(Snapshots::A1)) + " did not answer within 30 s"));
    EXPECT_GT(net.sentTo(net.key(Snapshots::A1)), 0u);
    expectFilesOf(tmp.path, fetcher.result()->first);
}

// (7) busy delays only that source, by retry_ms
TEST(SnapshotFetcher, BusyDelaysOnlyThatSource) {
    TmpDir tmp("busy");
    Net net;
    auto& a1 = net.add(Snapshots::A1);
    a1.busyAll = true;
    a1.busyMs = 2000;
    net.add(Snapshots::A2);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    const auto a1Key = net.key(Snapshots::A1);
    size_t a1Sends = 0;
    for (const auto& s : net.sent) {
        if (std::get<1>(s) != a1Key) {
            continue;
        }
        ++a1Sends;
        const uint64_t at = std::get<0>(s);
        for (const auto& [when, who] : net.busySent) {
            if (who == a1Key) {
                EXPECT_FALSE(when < at && at < when + 2000) << "asked " << at - when << " ms after a busy reply";
            }
        }
    }
    EXPECT_GE(a1Sends, 2u);
    EXPECT_GT(net.sentTo(net.key(Snapshots::A2)), a1Sends);
    expectFilesOf(tmp.path, fetcher.result()->first);
}

// (8) a source that leaves the neighbour set is not asked
TEST(SnapshotFetcher, SourceThatLeftIsNotAsked) {
    TmpDir tmp("left");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher); }, 20));
    net.peers.neighbours.erase(net.key(Snapshots::A1));
    fetcher.tick(net.peers);
    const size_t before = net.sentTo(net.key(Snapshots::A1));
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_EQ(net.sentTo(net.key(Snapshots::A1)), before);
    expectFilesOf(tmp.path, fetcher.result()->first);
}

// (9) a file whose group runs dry restarts from the other group
TEST(SnapshotFetcher, DryGroupSwitchesToTheOther) {
    TmpDir tmp("dry");
    Net net;
    net.add(Snapshots::A1).corruptAll = true;
    net.add(Snapshots::A2).corruptAll = true;
    net.add(Snapshots::B);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_TRUE(net.logged("switching to the files of another source group"));
    const auto result = fetcher.result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->first.nodeKey, snapshots().src[Snapshots::B].manifest.nodeKey);
    expectFilesOf(tmp.path, result->first);
}

// (10) a file that changes on disk between its last piece and the whole-file check: back to Collect
TEST(SnapshotFetcher, WholeFileMismatchGoesBackToCollect) {
    TmpDir tmp("wholefile");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher); }, 20));
    // up to the last piece; the whole-file check of the last file runs at the next tick
    for (size_t i = 0; i < 5000; ++i) {
        net.deliver(fetcher);
        if (fetcher.bytesTotal() != 0 && fetcher.bytesDone() == fetcher.bytesTotal()) {
            break;
        }
        net.nowMs += 100;
        fetcher.tick(net.peers);
    }
    ASSERT_EQ(fetcher.bytesDone(), fetcher.bytesTotal());
    ASSERT_EQ(fetcher.phase(), Phase::Fetch);
    for (const auto& entry : fs::directory_iterator(tmp.path)) {
        if (fs::file_size(entry.path()) == 0) {
            continue;
        }
        std::fstream f(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
        char c = 0;
        f.read(&c, 1);
        c ^= 0x01;
        f.seekp(0);
        f.write(&c, 1);
    }
    net.nowMs += 100;
    fetcher.tick(net.peers);
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_TRUE(net.logged("does not have the SHA-256 of the manifest after its last piece"));
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    expectFilesOf(tmp.path, fetcher.result()->first);
}

// (11) fetch_wait over without agreement: sync falls back, wait keeps waiting with a line every 5 min
TEST(SnapshotFetcher, FetchWaitSync) {
    TmpDir tmp("waitsync");
    Net net;
    net.add(Snapshots::A1);
    auto cfg = fetchConfig();
    cfg.fetchWait = 60;
    SnapshotFetcher fetcher(cfg, tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Fallback; }, 700));
    EXPECT_GE(net.nowMs - 10'000'000, 60'000u);
    EXPECT_TRUE(net.logged("SNAPSHOT: 1 source declares block 200 (need 2): waiting"));
    EXPECT_TRUE(net.logged("SNAPSHOT: no agreement of 2 community sources after 1 min (seen: 1 source for block 200). Syncing from block 0"));
    EXPECT_TRUE(net.logged("./node --download-snapshot  in this folder (Docker: CREDITS_SNAPSHOT=github)"));
    const auto sends = net.manifestRequests + net.sent.size();
    for (int i = 0; i < 1000; ++i) {
        net.step(fetcher);
    }
    EXPECT_EQ(net.manifestRequests + net.sent.size(), sends);
}

TEST(SnapshotFetcher, FetchWaitWait) {
    TmpDir tmp("waitwait");
    Net net;
    net.add(Snapshots::A1);
    auto cfg = fetchConfig();
    cfg.fetchWait = 60;
    cfg.fetchFallback = "wait";
    SnapshotFetcher fetcher(cfg, tmp.path, net.hooks());
    for (int i = 0; i < 6 * 60 * 10 + 10; ++i) {
        net.step(fetcher);
    }
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_EQ(net.countLogged("Waiting for more sources (fetch_fallback=wait)"), 2u);
    // a second source turns up
    net.add(Snapshots::A2);
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
}

// (12) a refused import marks that file's group; the next attempt takes another group; after
// three refusals the node syncs from block 0
TEST(SnapshotFetcher, ImportFailures) {
    TmpDir tmp("import");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    net.add(Snapshots::E);
    // the sources that declared a refused file are banned: min_sources=1 keeps one for the third attempt
    SnapshotFetcher fetcher(fetchConfig(1), tmp.path, net.hooks());
    std::set<std::string> walletHashes;
    std::set<cs::PublicKey> banned;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const size_t sentBefore = net.sent.size();
        ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; })) << attempt;
        const auto result = fetcher.result();
        ASSERT_TRUE(result);
        expectFilesOf(tmp.path, result->first);
        const auto* wallets = result->first.file(kWallets);
        ASSERT_NE(wallets, nullptr);
        EXPECT_TRUE(walletHashes.insert(wallets->sha256).second) << "the refused file came back";
        for (size_t i = sentBefore; i < net.sent.size(); ++i) {
            if (attempt > 1) {
                // files already verified are not asked again
                EXPECT_EQ(std::get<2>(net.sent[i]).name, kWallets);
            }
            EXPECT_EQ(banned.count(std::get<1>(net.sent[i])), 0u) << "a banned source was asked";
        }
        for (const auto& [key, source] : net.sources) {
            const auto* f = snapshots().src[source.index].manifest.file(kWallets);
            if (f->sha256 == wallets->sha256) {
                banned.insert(key);
            }
        }
        fetcher.onImportFailed(kWallets);
        EXPECT_EQ(fetcher.phase(), attempt < 3 ? Phase::Collect : Phase::Fallback);
        EXPECT_FALSE(fetcher.result());
    }
    EXPECT_TRUE(net.logged("3 imports failed"));
    EXPECT_EQ(net.countLogged("that the import refused: not asked again"), 4u);
}

// a refusal that is no file's fault bans the source that signed the manifest
TEST(SnapshotFetcher, ImportFailureWithoutAFileBansTheSigner) {
    TmpDir tmp("importsigner");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    net.add(Snapshots::B);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    const auto signer = fetcher.result()->first.nodeKey;
    fetcher.onImportFailed("");
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_TRUE(net.logged("source " + signer + " signed a snapshot the import refused: not asked again"));
    const size_t before = net.sent.size();
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    for (size_t i = before; i < net.sent.size(); ++i) {
        EXPECT_NE(base58(std::get<1>(net.sent[i])), signer);
    }
}

// a peer that becomes a neighbour late, and one silent for its first requests, are kept (design 5.3a)
TEST(SnapshotFetcher, LateAndSilentSourcesAreKept) {
    TmpDir tmp("late");
    Net net;
    net.add(Snapshots::A1, false).silentManifests = 1;
    net.add(Snapshots::A2).silentManifests = 2;
    net.add(Snapshots::B, false);
    SnapshotFetcher fetcher(fetchConfig(3), tmp.path, net.hooks());
    for (int i = 0; i < 250; ++i) {
        net.step(fetcher);
    }
    // A2 was silent twice and answered the third request, 10 and 20 s after the first
    EXPECT_EQ(net.sources[net.key(Snapshots::A2)].manifestAsks, 3u);
    EXPECT_EQ(fetcher.manifests(), 1u);
    for (int i = 0; i < 360; ++i) {   // past a minute: A1 and B become neighbours late
        net.step(fetcher);
    }
    EXPECT_EQ(fetcher.manifests(), 1u);
    net.peers.neighbours.insert(net.key(Snapshots::A1));
    net.peers.neighbours.insert(net.key(Snapshots::B));
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    EXPECT_EQ(fetcher.result()->second.size(), 3u);
}

// busy with no delay: not asked again before 500 ms
TEST(SnapshotFetcher, BusyWithoutDelayIsFloored) {
    TmpDir tmp("busyfloor");
    Net net;
    auto& a1 = net.add(Snapshots::A1);
    a1.busyAll = true;
    a1.busyMs = 0;
    net.add(Snapshots::A2);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
    const auto a1Key = net.key(Snapshots::A1);
    for (const auto& s : net.sent) {
        if (std::get<1>(s) != a1Key) {
            continue;
        }
        for (const auto& [when, who] : net.busySent) {
            if (who == a1Key) {
                EXPECT_FALSE(when < std::get<0>(s) && std::get<0>(s) < when + SnapshotFetcher::kMinBusyMs);
            }
        }
    }
}

// sources that are busy forever: each fetch ends after 5 min without data, the third ends the bootstrap
TEST(SnapshotFetcher, BusyForeverEndsTheFetch) {
    TmpDir tmp("busyforever");
    Net net;
    net.add(Snapshots::A1).busyAll = true;
    net.add(Snapshots::A2).busyAll = true;
    auto cfg = fetchConfig();
    cfg.fetchWait = 86'400;
    cfg.fetchFallback = "wait";
    SnapshotFetcher fetcher(cfg, tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher); }, 20));
    const uint64_t start = net.nowMs;
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() != Phase::Fetch; }, 4000));
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_GE(net.nowMs - start, SnapshotFetcher::kNoProgressMs);
    EXPECT_TRUE(net.logged("no data for 5 min (attempt 1 of 3)"));
    EXPECT_TRUE(net.logged("busy for 1 min without a piece"));
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Fallback; }, 10000));
    EXPECT_TRUE(net.logged("(attempt 3 of 3): syncing from block 0"));
}

// two groups as strong as each other: the 30-s line says conflict
TEST(SnapshotFetcher, ConflictLine) {
    TmpDir tmp("conflict");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::D);
    SnapshotFetcher fetcher(fetchConfig(1), tmp.path, net.hooks());
    for (int i = 0; i < 320; ++i) {
        net.step(fetcher);
    }
    EXPECT_EQ(fetcher.phase(), Phase::Collect);
    EXPECT_TRUE(net.logged("SNAPSHOT: conflict: two groups with 1 sources at block 200: waiting"));
    EXPECT_TRUE(net.logged("2 manifests collected, 0 not counted: address unknown, 0 replies dropped"));
}

// (13) a dry run stops after Collect and reports the groups
TEST(SnapshotFetcher, DryRunReportsGroups) {
    TmpDir tmp("dryrun");
    Net net;
    net.add(Snapshots::D);
    SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks(), true);
    net.step(fetcher);
    net.step(fetcher);
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Done; }, 20));
    EXPECT_TRUE(net.sent.empty());
    const auto groups = fetcher.groups();
    EXPECT_EQ(groups.size(), 2u);
    const auto chosen = std::count_if(groups.begin(), groups.end(), [](const cs::GroupReport& g) { return g.chosen; });
    EXPECT_EQ(chosen, 1);
    EXPECT_FALSE(fetcher.result());
    EXPECT_FALSE(fs::exists(tmp.path));

    // a dry run without agreement ends at fetch_wait too
    Net lone;
    lone.add(Snapshots::A1);
    auto cfg = fetchConfig();
    cfg.fetchWait = 10;
    SnapshotFetcher alone(cfg, tmp.path, lone.hooks(), true);
    ASSERT_TRUE(lone.runUntil(alone, [&] { return alone.phase() == Phase::Done; }, 200));
    EXPECT_EQ(alone.groups().size(), 1u);
}

// (14) stop in every phase: nothing outstanding, nothing sent, replies dropped
TEST(SnapshotFetcher, StopInEveryPhase) {
    TmpDir tmp("stop");
    auto quietAfterStop = [](Net& net, SnapshotFetcher& fetcher) {
        fetcher.stop();
        EXPECT_EQ(fetcher.outstanding(), 0u);
        const auto manifests = net.manifestRequests;
        const auto pieces = net.sent.size();
        const auto dropped = fetcher.dropped();
        const auto queued = net.pieceQueue.size() + net.manifestQueue.size();
        net.deliver(fetcher);   // replies to what was asked before the stop
        EXPECT_EQ(fetcher.dropped(), dropped + queued);
        for (int i = 0; i < 1300; ++i) {   // past every timer: piece timeout, marks, manifest interval, stall
            net.nowMs += 100;
            fetcher.tick(net.peers);
        }
        EXPECT_EQ(net.manifestRequests, manifests);
        EXPECT_EQ(net.sent.size(), pieces);
        EXPECT_EQ(fetcher.outstanding(), 0u);
    };
    {
        Net net;
        net.add(Snapshots::A1, false);
        SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
        fetcher.tick(net.peers);
        EXPECT_EQ(fetcher.phase(), Phase::Discover);
        quietAfterStop(net, fetcher);
        EXPECT_EQ(fetcher.phase(), Phase::Discover);
    }
    {
        Net net;
        net.add(Snapshots::A1);
        SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
        fetcher.tick(net.peers);
        EXPECT_EQ(fetcher.phase(), Phase::Collect);
        quietAfterStop(net, fetcher);
        EXPECT_EQ(fetcher.manifests(), 0u);
        EXPECT_EQ(fetcher.dropped(), 1u);   // the manifest reply
    }
    {
        Net net;
        net.add(Snapshots::A1);
        net.add(Snapshots::A2);
        SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
        ASSERT_TRUE(net.runUntil(fetcher, [&] { return isFetching(fetcher) && fetcher.outstanding() != 0; }, 20));
        quietAfterStop(net, fetcher);
    }
    {
        Net net;
        net.add(Snapshots::A1);
        net.add(Snapshots::A2);
        SnapshotFetcher fetcher(fetchConfig(), tmp.path, net.hooks());
        ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));
        quietAfterStop(net, fetcher);
        fetcher.onImportFailed(kWallets);
        EXPECT_EQ(fetcher.phase(), Phase::Import);
    }
}

// (15) fetch_rate_kbps caps the bytes requested
TEST(SnapshotFetcher, RateCap) {
    TmpDir tmp("rate");
    Net net;
    net.add(Snapshots::A1);
    net.add(Snapshots::A2);
    auto cfg = fetchConfig();
    cfg.fetchRateKbps = 200;   // 200,000 bytes/s
    SnapshotFetcher fetcher(cfg, tmp.path, net.hooks());
    ASSERT_TRUE(net.runUntil(fetcher, [&] { return fetcher.phase() == Phase::Import; }));

    const auto& m = snapshots().src[Snapshots::A1].manifest;
    auto length = [&](const PieceRequest& r) {
        const auto* f = m.file(r.name);
        const uint64_t offset = uint64_t(r.chunk) * m.chunkSize;
        return std::min<uint64_t>(m.chunkSize, f->size - offset);
    };
    std::map<uint64_t, uint64_t> perSecond;
    uint64_t total = 0;
    for (const auto& s : net.sent) {
        perSecond[std::get<0>(s) / 1000] += length(std::get<2>(s));
        total += length(std::get<2>(s));
    }
    ASSERT_GE(perSecond.size(), 4u);
    for (const auto& [second, bytes] : perSecond) {
        // a request goes out when nothing is owed: at most one piece above the rate in any second
        EXPECT_LE(bytes, 200'000u + m.chunkSize) << "second " << second;
    }
    const uint64_t span = std::get<0>(net.sent.back()) - std::get<0>(net.sent.front());
    EXPECT_LE(total, 200 * span + m.chunkSize);
}
