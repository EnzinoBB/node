#ifndef SNAPSHOT_IMPORTER_HPP
#define SNAPSHOT_IMPORTER_HPP

#include <atomic>
#include <filesystem>
#include <string>

#include <csnode/snapshot_manifest.hpp>

namespace cs {

struct SnapshotImportResult {
    bool ok = false;
    std::string error;
    std::string fileAtFault;   // the snapshot file that failed, empty when the fault is not a file's
};

// Restores a format-2 snapshot (manifest m, its files in srcDir, all downloaded) into a node folder
// whose database is empty: the files must match the manifest (sizes, whole-file and chunk hashes);
// qs/<seq> is written from the checkpoint files, whose head.bin and state_digest.bin must carry the
// manifest's block hash and digests; every block is checked with the BlockVerifier and as a hash
// chain ending at the manifest's head, then written to a new database of the given backend (lmdb or
// rocksdb) with caches/hashdb and caches/seqdb; then the contract data and nodeDir/snapshot.json.
// The node quick-starts from qs/<seq> and rebuilds its transaction index. Any failure removes
// everything the import created.
// Trust: this proves that the snapshot is consistent with itself (signed manifest, a hash chain of
// blocks signed by their confidants and confirmed by the previous round's, ending at the declared
// head, a checkpoint whose head and digests are the declared ones). It does not prove that the
// head is the network's: the caller anchors it, by the agreement of independent sources on
// (seq, head_hash, state_digest) before calling, and the node by continuing from that block with
// the network; the node also recomputes the state from the loaded caches at its quick start.
SnapshotImportResult importSnapshot(const std::filesystem::path& srcDir, const SnapshotManifest& m,
                                    const std::filesystem::path& nodeDir, const std::filesystem::path& dbPath,
                                    const std::string& backend, const std::atomic<bool>* stop = nullptr);

}  // namespace cs

#endif  // SNAPSHOT_IMPORTER_HPP
