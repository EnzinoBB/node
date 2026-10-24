#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <cscrypto/cscrypto.hpp>
#include <csdb/database.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage_backend.hpp>
#include <csnode/blockhashes.hpp>
#include <csnode/odatastream.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_importer.hpp>
#include <csnode/snapshot_manifest.hpp>
#include <csnode/statedigest.hpp>

namespace {
namespace fs = std::filesystem;

struct SignedChain {
    std::map<cs::Sequence, cs::Bytes> blocks;
    std::map<cs::Sequence, cs::Bytes> hashes;
};

cs::Hash toHash(const csdb::PoolHash& h) {
    cs::Hash out{};
    const auto b = h.to_binary();
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

// blocks 0..count-1 linked by hash, each (but 0) signed by its three confidants
SignedChain makeSignedChain(cs::Sequence count) {
    EXPECT_TRUE(cscrypto::cryptoInit());
    std::vector<cs::PublicKey> pub(3);
    std::vector<cs::PrivateKey> priv;
    for (auto& p : pub) {
        priv.push_back(cscrypto::generateKeyPair(p));
    }
    SignedChain c;
    csdb::PoolHash prev;
    for (cs::Sequence s = 0; s < count; ++s) {
        csdb::Pool p(prev, s);
        p.add_user_field(1, std::string(64 + s % 30, static_cast<char>('a' + s % 26)));
        if (s > 0) {
            p.set_confidants(pub);
            p.add_number_trusted(3);
            p.add_real_trusted(0b111);
            if (s > 1) {
                // the previous round's confidants (the same keys here) confirm this round's
                cs::Bytes trustedToHash;
                cs::ODataStream stream(trustedToHash);
                stream << s;
                stream << pub;
                const cs::Hash h = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());
                std::vector<cs::Signature> confirmations;
                for (const auto& k : priv) {
                    confirmations.push_back(cscrypto::generateSignature(k, h.data(), h.size()));
                }
                p.add_number_confirmations(3);
                p.add_confirmation_mask(0b111);
                p.add_round_confirmations(confirmations);
            }
            auto probe = p.clone();
            EXPECT_TRUE(probe.compose());
            const auto hash = toHash(probe.hash());
            std::vector<cs::Signature> sigs;
            for (const auto& k : priv) {
                sigs.push_back(cscrypto::generateSignature(k, hash.data(), hash.size()));
            }
            p.set_signatures(sigs);
        }
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

void makeCheckpoint(const fs::path& dir, cs::Sequence seq, const cs::Bytes& headHash) {
    fs::create_directories(dir);
    std::ofstream f(dir / "head.bin", std::ios::binary);
    const uint32_t magic = 0x44414548u;
    const uint64_t s = seq;
    f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    f.write(reinterpret_cast<const char*>(&s), sizeof(s));
    putU16(f, static_cast<uint16_t>(headHash.size()));
    f.write(reinterpret_cast<const char*>(headHash.data()), static_cast<std::streamsize>(headHash.size()));
    putU16(f, 0);
    f.close();
    std::ofstream(dir / "walletscache.dat", std::ios::binary) << std::string(100000, 'w');
    cs::CheckpointDigest d;
    d.digest.fill(3);
    d.settings.fill(4);
    d.wallets = 11;
    EXPECT_TRUE(d.write(dir / cs::CheckpointDigest::kFileName));
}

struct Fixture {
    fs::path root;
    SignedChain chain = makeSignedChain(161);
    cs::PrivateKey key;
    cs::PublicKey pub;
    std::map<cs::Bytes, cs::Bytes> contracts = {{{1, 2, 3}, {7, 7}}, {{9}, {}}};

    explicit Fixture(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        fs::create_directories(root / "source");
        fs::create_directories(root / "target");
        key = cscrypto::generateKeyPair(pub);
        makeCheckpoint(root / "source" / "qs" / "150", 150, chain.hashes[150]);
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    cs::SnapshotBuildResult build() {
        cs::SnapshotBuildInput in;
        in.seq = 150;
        in.checkpointDir = root / "source" / "qs" / "150";
        in.snapshotBlocks = 100;
        in.nodeVersion = "test";
        in.chunkSize = 64 << 10;
        in.blockBytes = [this](cs::Sequence s, cs::Bytes& out) {
            out = chain.blocks.at(s);
            return true;
        };
        in.contracts = [this](const std::function<bool(const cs::Bytes&, const cs::Bytes&)>& visit) {
            for (const auto& [k, v] : contracts) {
                if (!visit(k, v)) return false;
            }
            return true;
        };
        return cs::buildSnapshot(in, root / "out", key);
    }

    cs::SnapshotImportResult import(const cs::SnapshotBuildResult& r, const std::string& backend = "lmdb") {
        return cs::importSnapshot(r.dir, r.manifest, root / "target", root / "target" / "db", backend);
    }

    // nothing the import creates is left after a failure
    void expectNothingLeft() {
        const auto t = root / "target";
        EXPECT_TRUE(csdb::backend::detect((t / "db").string(), false).empty());
        EXPECT_FALSE(fs::exists(t / "qs" / "150"));
        EXPECT_FALSE(fs::exists(t / "qs" / "150.tmp"));
        EXPECT_FALSE(fs::exists(t / "caches" / "hashdb"));
        EXPECT_FALSE(fs::exists(t / "caches" / "seqdb"));
        EXPECT_FALSE(fs::exists(t / "snapshot.json"));
    }
};

void roundTrip(const std::string& backend) {
    Fixture fx(("snapshot_import_" + backend).c_str());
    const auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    const auto r = fx.import(built, backend);
    ASSERT_TRUE(r.ok) << r.error << " " << r.fileAtFault;

    const auto t = fx.root / "target";
    EXPECT_EQ(csdb::backend::readMarker((t / "db").string()), backend);
    auto db = csdb::backend::open(backend, (t / "db").string());
    ASSERT_TRUE(db && db->is_open());
    for (cs::Sequence s = 50; s <= 150; ++s) {
        cs::Bytes bytes;
        ASSERT_TRUE(db->get(static_cast<uint32_t>(s), &bytes)) << s;
        EXPECT_EQ(bytes, fx.chain.blocks[s]) << s;
    }
    EXPECT_FALSE(db->get(static_cast<uint32_t>(49)));
    for (const auto& [k, v] : fx.contracts) {
        cs::Bytes value;
        EXPECT_TRUE(db->getContractData(k, value));
        EXPECT_EQ(value, v);
    }
    db.reset();

    cs::BlockHashes hashes((t / "caches").string());
    EXPECT_EQ(hashes.find(150).to_binary(), fx.chain.hashes[150]);
    EXPECT_EQ(hashes.find(csdb::PoolHash::from_binary(cs::Bytes(fx.chain.hashes[50]))), 50u);
    hashes.close();

    EXPECT_TRUE(fs::exists(t / "qs" / "150" / "head.bin"));
    std::ifstream a(fx.root / "source" / "qs" / "150" / "walletscache.dat", std::ios::binary);
    std::ifstream b(t / "qs" / "150" / "walletscache.dat", std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(a), {}), std::string(std::istreambuf_iterator<char>(b), {}));
    EXPECT_TRUE(fs::exists(t / "snapshot.json"));
}
}  // namespace

TEST(SnapshotImport, RoundTripLmdb) {
    roundTrip("lmdb");
}

TEST(SnapshotImport, RoundTripRocksDb) {
    roundTrip("rocksdb");
}

TEST(SnapshotImport, BerkeleyDbRefused) {
    Fixture fx("snapshot_import_bdb");
    const auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    EXPECT_FALSE(fx.import(built, "berkeleydb").ok);
    fx.expectNothingLeft();
}

TEST(SnapshotImport, AlteredSignatureRejectedNothingLeft) {
    Fixture fx("snapshot_import_sig");
    auto& bytes = fx.chain.blocks[120];
    const auto pool = csdb::Pool::from_binary(cs::Bytes(bytes));
    bytes[pool.hashingLength() + sizeof(size_t)] ^= 1;   // first byte of the first signature
    const auto built = fx.build();                      // the builder checks the chain, not signatures
    ASSERT_TRUE(built.ok) << built.error;
    const auto r = fx.import(built);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.fileAtFault, "blocks.bin.lz4");
    EXPECT_NE(r.error.find("120"), std::string::npos) << r.error;
    fx.expectNothingLeft();
}

TEST(SnapshotImport, ManifestDigestOtherThanTheCheckpointsRejected) {
    Fixture fx("snapshot_import_digest");
    auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    built.manifest.stateDigest = std::string(64, 'f');
    built.manifest.sign(fx.key);   // a validly signed manifest that lies about the state
    const auto r = fx.import(built);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.fileAtFault, "qs-state_digest.bin.lz4");
    fx.expectNothingLeft();
}

TEST(SnapshotImport, ManifestHeadOtherThanTheChainsRejected) {
    Fixture fx("snapshot_import_head");
    auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    built.manifest.headHash = std::string(64, 'e');
    built.manifest.sign(fx.key);
    EXPECT_FALSE(fx.import(built).ok);
    fx.expectNothingLeft();
}

TEST(SnapshotImport, TruncatedOrAlteredFileRejected) {
    Fixture fx("snapshot_import_trunc");
    const auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    const auto blocks = built.dir / "blocks.bin.lz4";
    const auto size = fs::file_size(blocks);
    fs::resize_file(blocks, size - 10);
    auto r = fx.import(built);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.fileAtFault, "blocks.bin.lz4");
    fx.expectNothingLeft();

    fs::resize_file(blocks, size);   // right size, wrong bytes
    r = fx.import(built);
    EXPECT_FALSE(r.ok);
    fx.expectNothingLeft();
}

TEST(SnapshotImport, OversizedRawFileRejected) {
    Fixture fx("snapshot_import_raw");
    auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    for (auto& f : built.manifest.files) {
        if (f.name == "qs-walletscache.dat.lz4") {
            f.rawSize = 5ull << 30;
        }
    }
    built.manifest.sign(fx.key);
    EXPECT_FALSE(fx.import(built).ok);
    fx.expectNothingLeft();
}

TEST(SnapshotImport, ExistingDatabaseRefused) {
    Fixture fx("snapshot_import_existing");
    const auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    ASSERT_TRUE(fx.import(built).ok);
    fs::remove_all(fx.root / "target" / "qs");
    const auto again = fx.import(built);
    EXPECT_FALSE(again.ok);
    // the first import is untouched
    auto db = csdb::backend::open("lmdb", (fx.root / "target" / "db").string());
    cs::Bytes bytes;
    EXPECT_TRUE(db && db->get(150u, &bytes));
}

TEST(SnapshotImport, RebuiltBlockWithoutSignaturesOrConfirmationsRejected) {
    Fixture fx("snapshot_import_confirmations");
    // block 100 rebuilt by someone without the confidants' keys: no signatures, no confirmations
    const auto original = csdb::Pool::from_binary(cs::Bytes(fx.chain.blocks[100]));
    csdb::Pool p(original.previous_hash(), 100);
    p.add_user_field(1, original.user_field(1).value<std::string>());
    p.set_confidants(original.confidants());
    p.add_number_trusted(3);
    p.add_real_trusted(0b111);
    ASSERT_TRUE(p.compose());
    fx.chain.blocks[100] = p.to_binary();   // its hash changes too: block 101 no longer follows it
    const auto built = fx.build();
    if (built.ok) {
        EXPECT_FALSE(fx.import(built).ok);
        fx.expectNothingLeft();
    }
}

TEST(SnapshotImport, FailedRocksDbImportLeavesNoFiles) {
    Fixture fx("snapshot_import_rocksdb_fail");
    auto& bytes = fx.chain.blocks[120];
    const auto pool = csdb::Pool::from_binary(cs::Bytes(bytes));
    bytes[pool.hashingLength() + sizeof(size_t)] ^= 1;
    const auto built = fx.build();
    ASSERT_TRUE(built.ok) << built.error;
    EXPECT_FALSE(fx.import(built, "rocksdb").ok);
    fx.expectNothingLeft();
    // RocksDB writes on close: nothing may appear after the cleanup
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fx.root / "target" / "db", ec)) {
        EXPECT_NE(e.path().extension(), ".sst") << e.path();
        EXPECT_NE(e.path().filename(), "CURRENT");
    }
}

