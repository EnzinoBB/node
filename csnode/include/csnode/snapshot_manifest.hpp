#ifndef SNAPSHOT_MANIFEST_HPP
#define SNAPSHOT_MANIFEST_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <lib/system/common.hpp>

namespace cs {

// snapshot.json of a format-2 snapshot (docs/specs/2026-10-24-snapshot-serving-design.md, section 2):
// the state of a checkpoint S (its block hash, state and settings digests), the files that carry it,
// each with its SHA-256 and the SHA-256 of every chunk, signed with the key of the node that built it.
struct SnapshotFile {
    std::string name;                 // one of SnapshotManifest::isKnownFileName
    uint64_t size = 0;                // bytes of the file as served (compressed)
    uint64_t rawSize = 0;             // bytes once decompressed
    std::string sha256;               // hex, of the file as served
    std::vector<std::string> chunks;  // hex SHA-256 of each chunkSize piece, the last one shorter
};

struct SnapshotManifest {
    static constexpr uint32_t kFormat = 2;
    static constexpr uint64_t kMaxChunkSize = 16ull << 20;
    static constexpr uint64_t kMaxTotalSize = 8ull << 30;
    static constexpr size_t kMaxFiles = 32;
    static constexpr size_t kMaxJsonSize = 1 << 20;

    uint32_t format = kFormat;
    cs::Sequence seq = 0;
    std::string headHash;        // hex, hash of block seq
    std::string stateDigest;     // hex, wallets digest of the checkpoint (state_digest.bin)
    uint64_t wallets = 0;
    std::string settingsDigest;  // hex, consensus settings hash of the checkpoint (state_digest.bin)
    cs::Sequence firstBlock = 0; // first block of blocks.bin
    std::string created;         // UTC, ISO 8601
    std::string node;            // version of the node that built it
    std::string nodeKey;         // base58 public key of that node
    uint64_t chunkSize = 4ull << 20;
    std::string compression = "lz4";
    std::vector<SnapshotFile> files;
    std::string signature;       // hex Ed25519 signature of signedLine()

    // the snapshot's files: blocks.bin.lz4, contracts.bin.lz4 and qs-<file>.lz4 for every file of a
    // checkpoint directory; anything else in a manifest rejects it
    static bool isKnownFileName(const std::string& name);
    static const std::vector<std::string>& checkpointFileNames();

    // SHA-256 of the lines "name size sha256\n" in manifest order
    std::string filesHash() const;
    std::string signedLine() const;

    void sign(const cs::PrivateKey& key);
    // the signature is node_key's over signedLine(), recomputed from the fields
    bool verify() const;

    std::string toJson() const;
    // nullopt, with the reason in *error, for anything but a well-formed format-2 manifest within the limits
    static std::optional<SnapshotManifest> fromJson(const std::string& json, std::string* error = nullptr);

    const SnapshotFile* file(const std::string& name) const;
};

// lower-case hex SHA-256 (libsodium)
std::string sha256Hex(const uint8_t* data, size_t size);

}  // namespace cs

#endif  // SNAPSHOT_MANIFEST_HPP
