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
SnapshotImportResult importSnapshot(const std::filesystem::path& srcDir, const SnapshotManifest& m,
                                    const std::filesystem::path& nodeDir, const std::filesystem::path& dbPath,
                                    const std::string& backend, const std::atomic<bool>* stop = nullptr);

}  // namespace cs

#endif  // SNAPSHOT_IMPORTER_HPP
