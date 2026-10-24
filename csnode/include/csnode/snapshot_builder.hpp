#ifndef SNAPSHOT_BUILDER_HPP
#define SNAPSHOT_BUILDER_HPP

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <csnode/snapshot_manifest.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Compressed files of a snapshot: independent lz4 (HC) blocks of at most 4 MiB of raw data, each
// "u32 raw_len, u32 compressed_len, bytes" (little-endian), so that a reader needs no frame library
// and memory stays bounded.
namespace snapshot_lz4 {
constexpr size_t kBlockSize = 4u << 20;
// calls sink with each decompressed block; false for a malformed file or when sink returns false
bool decompressFile(const std::filesystem::path& file, const std::function<bool(const uint8_t*, size_t)>& sink);
}  // namespace snapshot_lz4

// What a snapshot of checkpoint seq is built from: the checkpoint directory (qs/<seq>) and the
// node's storage, read through these functions (raw stored block bytes by sequence, contract data).
struct SnapshotBuildInput {
    cs::Sequence seq = 0;
    std::filesystem::path checkpointDir;
    cs::Sequence firstStored = 0;
    size_t snapshotBlocks = 101'000;
    std::function<bool(cs::Sequence, cs::Bytes&)> blockBytes;
    std::function<bool(const std::function<bool(const cs::Bytes&, const cs::Bytes&)>&)> contracts;
    std::string nodeVersion;
    uint64_t chunkSize = 4ull << 20;
    const std::atomic<bool>* stop = nullptr;   // checked between blocks: true aborts the build
};

struct SnapshotBuildResult {
    bool ok = false;
    std::string error;
    SnapshotManifest manifest;
    std::filesystem::path dir;   // <outRoot>/<seq>
};

// Builds <outRoot>/<seq>/ (snapshot.json and its files) through <outRoot>/<seq>.tmp, then removes
// the other built snapshots in outRoot. Nothing is left behind on failure. The blocks must form a
// hash chain ending at the checkpoint's head, and the checkpoint must have state_digest.bin.
SnapshotBuildResult buildSnapshot(const SnapshotBuildInput& input, const std::filesystem::path& outRoot,
                                  const cs::PrivateKey& key);

// the blocks.bin record stream: "u32 seq, u32 len, bytes" (little-endian)
bool forEachSnapshotBlock(const std::filesystem::path& blocksFile,
                          const std::function<bool(cs::Sequence, cs::Bytes&&)>& visit);

}  // namespace cs

#endif  // SNAPSHOT_BUILDER_HPP
