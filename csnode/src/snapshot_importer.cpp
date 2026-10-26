#include <csnode/snapshot_importer.hpp>

#include <array>
#include <fstream>
#include <vector>

#include <sodium.h>

#include <lib/system/platform.hpp>

#include <csdb/database.hpp>
#include <csdb/storage_backend.hpp>
#include <csnode/blockhashes.hpp>
#include <csnode/blockverifier.hpp>
#include <csnode/caches_serialization_manager.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/statedigest.hpp>
#include <lib/system/logger.hpp>

namespace cs {

namespace {
namespace fs = std::filesystem;

constexpr uint64_t kMaxRawFile = 4ull << 30;
constexpr size_t kBatch = 1000;
constexpr uint32_t kMaxContractField = 64u << 20;

std::string hexOf(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// the file as downloaded: size, every chunk and the whole file as the manifest declares
bool fileMatches(const fs::path& path, const SnapshotFile& f, uint64_t chunkSize, std::string& why) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != f.size) {
        why = "missing or of another size";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    crypto_hash_sha256_state whole;
    crypto_hash_sha256_init(&whole);
    std::vector<uint8_t> buf(static_cast<size_t>(chunkSize));
    size_t chunk = 0;
    for (uint64_t done = 0; done < f.size; ++chunk) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(chunkSize, f.size - done));
        in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
        if (static_cast<size_t>(in.gcount()) != n || chunk >= f.chunks.size() || sha256Hex(buf.data(), n) != f.chunks[chunk]) {
            why = "chunk " + std::to_string(chunk) + " does not match";
            return false;
        }
        crypto_hash_sha256_update(&whole, buf.data(), n);
        done += n;
    }
    std::array<uint8_t, crypto_hash_sha256_BYTES> h{};
    crypto_hash_sha256_final(&whole, h.data());
    if (chunk != f.chunks.size() || hexOf(h.data(), h.size()) != f.sha256) {
        why = "the file hash does not match";
        return false;
    }
    return true;
}

// on disk before the next step relies on it: a power cut must not leave a truncated checkpoint
void syncPath(const fs::path& path, bool directory) {
    cs::platform::syncPath(path, directory);
}

bool stopped(const std::atomic<bool>* stop) {
    return stop && stop->load();
}
}  // namespace

SnapshotImportResult importSnapshot(const fs::path& srcDir, const SnapshotManifest& m, const fs::path& nodeDir,
                                    const fs::path& dbPath, const std::string& backend, const std::atomic<bool>* stop) {
    SnapshotImportResult result;
    std::error_code ec;
    const cs::Sequence S = m.seq;
    const fs::path qsFinal = nodeDir / "qs" / std::to_string(S);
    // ".tmp": the node's checkpoint manager removes such a leftover at its start and never takes it
    // for a checkpoint
    const fs::path qsTmp = nodeDir / "qs" / (std::to_string(S) + ".tmp");
    const fs::path caches = nodeDir / "caches";
    bool createdDb = false, createdQs = false, createdCaches = false;

    auto failWith = [&](const std::string& reason, const std::string& file = std::string{}) {
        fs::remove_all(qsTmp, ec);
        if (createdQs) fs::remove_all(qsFinal, ec);
        if (createdDb) {
            // only the database files this import created: db/ may hold the node's lock file
            for (const auto& f : csdb::backend::files(backend, dbPath.string())) {
                fs::remove_all(f, ec);
            }
            fs::remove(dbPath / csdb::backend::kMarkerFile, ec);
        }
        if (createdCaches) {
            fs::remove_all(caches / "hashdb", ec);
            fs::remove_all(caches / "seqdb", ec);
            fs::remove(caches / "blockhashes.rebuild", ec);
        }
        result.ok = false;
        result.error = reason;
        result.fileAtFault = file;
        return result;
    };

    if (backend != "lmdb" && backend != "rocksdb") {
        return failWith("a snapshot starts above block 0, which a " + backend + " database cannot hold: use lmdb or rocksdb");
    }
    if (!csdb::backend::detect(dbPath.string(), true).empty()) {
        return failWith(dbPath.string() + " already holds a database");
    }
    if (fs::exists(qsFinal, ec)) {
        return failWith(qsFinal.string() + " already exists");
    }
    if (!m.verify()) {
        return failWith("the manifest's signature does not verify");
    }

    // 1. every file as the manifest declares it
    for (const auto& f : m.files) {
        std::string why;
        if (!SnapshotManifest::isKnownFileName(f.name)) {
            return failWith("unknown file", f.name);
        }
        if (!fileMatches(srcDir / f.name, f, m.chunkSize, why)) {
            return failWith(f.name + ": " + why, f.name);
        }
        if (f.rawSize > kMaxRawFile) {
            return failWith(f.name + " is larger than 4 GB once decompressed", f.name);
        }
    }

    // 2. the checkpoint, whose head and digests must be the manifest's
    fs::remove_all(qsTmp, ec);
    fs::create_directories(qsTmp, ec);
    if (ec) {
        return failWith("cannot create " + qsTmp.string() + ": " + ec.message());
    }
    for (const auto& name : SnapshotManifest::checkpointFileNames()) {
        const auto* f = m.file("qs-" + name + ".lz4");
        if (!f) {
            continue;
        }
        std::ofstream out(qsTmp / name, std::ios::binary | std::ios::trunc);
        uint64_t written = 0;
        const bool ok = snapshot_lz4::decompressFile(srcDir / f->name, [&](const uint8_t* data, size_t size) {
            written += size;
            if (written > f->rawSize) {
                return false;
            }
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        });
        out.close();
        syncPath(qsTmp / name, false);
        if (!ok || written != f->rawSize || !out) {
            return failWith(f->name + " does not decompress to its declared size", f->name);
        }
    }
    const auto head = readCheckpointHead(qsTmp);
    if (!head || head->sequence != S || hexOf(head->head_hash.data(), head->head_hash.size()) != m.headHash) {
        return failWith("the checkpoint's head is not the manifest's block", "qs-head.bin.lz4");
    }
    const auto digest = CheckpointDigest::read(qsTmp / CheckpointDigest::kFileName);
    if (!digest || hexOf(digest->digest.data(), digest->digest.size()) != m.stateDigest || digest->wallets != m.wallets ||
        hexOf(digest->settings.data(), digest->settings.size()) != m.settingsDigest) {
        return failWith("the checkpoint's state digest is not the manifest's", "qs-state_digest.bin.lz4");
    }

    // 3. the blocks: verified, chained to the manifest's head, written with their hashes
    if (fs::exists(caches / "hashdb", ec) || fs::exists(caches / "seqdb", ec)) {
        return failWith(caches.string() + " already holds block hash databases");
    }
    createdDb = true;
    createdCaches = true;
    // the database and the hash databases are closed when this returns: on failure their files are
    // removed only then (RocksDB writes on close)
    const auto [dbError, dbErrorFile] = [&]() -> std::pair<std::string, std::string> {
        auto db = csdb::backend::open(backend, dbPath.string());
        if (!db || !db->is_open()) {
            return {"cannot create the " + backend + " database in " + dbPath.string(), std::string{}};
        }
        fs::create_directories(caches, ec);
        BlockHashes hashes(caches.string());
        std::vector<csdb::Database::PendingWrite> batch;
        cs::Sequence expected = m.firstBlock;
        csdb::PoolHash previous;
        cs::PublicKeys previousConfidants;
        std::string reason;
        const auto* blocksFile = m.file("blocks.bin.lz4");
        const bool read = forEachSnapshotBlock(srcDir / "blocks.bin.lz4", [&](cs::Sequence seq, cs::Bytes&& bytes) {
            if (stopped(stop)) {
                reason = "stopped";
                return false;
            }
            if (seq != expected || seq > S) {
                reason = "block " + std::to_string(seq) + " where " + std::to_string(expected) + " was expected";
                return false;
            }
            const auto pool = csdb::Pool::from_binary(cs::Bytes(bytes));
            std::string why;
            if (!BlockVerifier::verify(pool, seq, csdb::PoolHash{}, why)) {
                reason = "block " + std::to_string(seq) + " " + why;
                return false;
            }
            // anchored: each block is the one the next block names, and the last is the agreed head
            if (seq > m.firstBlock && pool.previous_hash() != previous) {
                reason = "block " + std::to_string(seq) + " does not follow block " + std::to_string(seq - 1);
                return false;
            }
            // and the confidants of each round confirmed the next round's, as the node checks when
            // it stores a block: a forged range needs the keys of every round's confidants
            if (seq > m.firstBlock && !BlockVerifier::verifyConfirmations(pool, previousConfidants, why)) {
                reason = "block " + std::to_string(seq) + " " + why;
                return false;
            }
            previousConfidants = pool.confidants();
            const auto hash = pool.hash();
            if (seq == S && hexOf(hash.to_binary().data(), hash.to_binary().size()) != m.headHash) {
                reason = "block " + std::to_string(S) + " is not the manifest's head";
                return false;
            }
            if (!hashes.update(pool)) {
                reason = "cannot write the block hash databases";
                return false;
            }
            batch.push_back(csdb::Database::PendingWrite{hash.to_binary(), static_cast<uint32_t>(seq), std::move(bytes)});
            if (batch.size() >= kBatch && !db->put_batch(batch)) {
                reason = "cannot write blocks to the database";
                return false;
            }
            if (batch.size() >= kBatch) {
                batch.clear();
            }
            previous = hash;
            ++expected;
            return true;
        }, blocksFile ? blocksFile->rawSize : 0);
        if (!read || expected != S + 1) {
            hashes.close();
            return {reason.empty() ? "blocks.bin.lz4 is truncated or malformed" : reason, "blocks.bin.lz4"};
        }
        if (!batch.empty() && !db->put_batch(batch)) {
            hashes.close();
            return {"cannot write blocks to the database", std::string{}};
        }

        // 4. contract data
        if (const auto* f = m.file("contracts.bin.lz4")) {
            std::vector<uint8_t> pending;
            bool good = true;
            uint64_t total = 0;
            const bool ok = snapshot_lz4::decompressFile(srcDir / f->name, [&](const uint8_t* data, size_t size) {
                total += size;
                if (total > f->rawSize) {
                    return false;
                }
                pending.insert(pending.end(), data, data + size);
                size_t pos = 0;
                for (;;) {
                    if (pending.size() - pos < 4) break;
                    const uint32_t klen = getU32(pending.data() + pos);
                    if (klen > kMaxContractField) {
                        good = false;
                        return false;
                    }
                    if (pending.size() - pos < 8ull + klen) break;
                    const uint32_t vlen = getU32(pending.data() + pos + 4 + klen);
                    if (vlen > kMaxContractField) {
                        good = false;
                        return false;
                    }
                    if (pending.size() - pos < 8ull + klen + vlen) break;
                    const cs::Bytes key(pending.begin() + static_cast<ptrdiff_t>(pos + 4), pending.begin() + static_cast<ptrdiff_t>(pos + 4 + klen));
                    const cs::Bytes value(pending.begin() + static_cast<ptrdiff_t>(pos + 8 + klen),
                                          pending.begin() + static_cast<ptrdiff_t>(pos + 8 + klen + vlen));
                    if (!db->updateContractData(key, value)) {
                        good = false;
                        return false;
                    }
                    pos += 8ull + klen + vlen;
                }
                pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(pos));
                return true;
            });
            if (!ok || !good || !pending.empty() || total != f->rawSize) {
                hashes.close();
                return {"contracts.bin.lz4 does not restore", f->name};
            }
        }
        if (!db->flush()) {
            hashes.close();
            return {"cannot flush the database", std::string{}};
        }
        hashes.close();
        if (hashes.rebuildRequested()) {
            return {"cannot write the block hash databases", std::string{}};
        }
        return {};
    }();
    if (!dbError.empty()) {
        return failWith(dbError, dbErrorFile);
    }
    if (!csdb::backend::writeMarker(dbPath.string(), backend)) {
        return failWith("cannot write the database marker");
    }

    // 5. publish the checkpoint and the manifest
    fs::rename(qsTmp, qsFinal, ec);
    if (ec) {
        return failWith("cannot move the checkpoint into place: " + ec.message());
    }
    createdQs = true;
    syncPath(qsTmp.parent_path(), true);
    {
        std::ofstream out(nodeDir / "snapshot.json", std::ios::binary | std::ios::trunc);
        out << m.toJson();
        out.flush();
        if (!out) {
            return failWith("cannot write snapshot.json");
        }
    }
    syncPath(nodeDir / "snapshot.json", false);
    syncPath(nodeDir, true);
    result.ok = true;
    return result;
}

}  // namespace cs
