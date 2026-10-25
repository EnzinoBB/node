#ifndef SNAPSHOT_BOOTSTRAP_HPP
#define SNAPSHOT_BOOTSTRAP_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <config.hpp>
#include <csnode/snapshot_agreement.hpp>
#include <csnode/statedigest.hpp>
#include <lib/system/common.hpp>
#include <net/packet.hpp>

// The start of a node on an empty database (docs/specs/2026-10-25-snapshot-p2p-design.md, sections
// 6.2, 6.7-6.9): the parts of Node's bootstrap mode that need no Node, pure over the file system.
namespace cs::snapshot_bootstrap {

inline const char* const kTmpDir = "snapshot-tmp";               // the fetcher's downloads, in the node folder
inline const char* const kImportingFile = ".importing";          // in db/: an import in progress
inline const char* const kManifestFile = "snapshot.json";        // written by the importer
inline const char* const kVerifiedFile = "snapshot.verified";
inline const char* const kMismatchFile = "snapshot.mismatch";
inline const char* const kSourcesFile = "snapshot.sources.json";

// why this start does not fetch a snapshot (design 6.2)
struct Skip {
    enum Why {
        FetchOff,     // [snapshot] fetch=off
        SetBcTop,     // --set-bc-top
        Database,     // db/ holds a database
        Checkpoint,   // qs/ holds a checkpoint: a previous run
        Backend       // a BerkeleyDB node cannot start above block 0
    };
    Why why = FetchOff;
    std::string text;
};

// nullopt: this start fetches a snapshot; snapshot-tmp/ left by an interrupted fetch is then
// removed (*removedTmp set). The checks, in order: fetch=off, --set-bc-top, a database in dbPath
// (csdb::backend::detect without opening an engine), a numbered checkpoint in qs/ (qs/0 too), the
// backend. The backend comes last, so that its reason is given only for an empty folder.
std::optional<Skip> notNeeded(const SnapshotData& cfg, const std::filesystem::path& nodeDir, const std::string& dbPath,
                              const std::string& backend, bool setBcTop, bool* removedTmp = nullptr);

// while bootstrapping only the two snapshot replies are processed (they reach the fetcher), every
// other message, the snapshot requests too, is dropped
bool processWhileBootstrapping(MsgTypes type);

// snapshot.json present and snapshot.verified absent: the digest the quick start computed from the
// loaded caches must be the manifest's (design 6.8)
enum class StartupCheck {
    NoManifest,        // no snapshot.json: nothing to verify
    AlreadyVerified,   // snapshot.verified exists
    Verified,          // the digests are equal: snapshot.verified written
    Skipped,           // the chain is above the snapshot's block: snapshot.verified written, nothing compared
    Mismatch,          // the digests differ, the chain ends below the snapshot's block, or the manifest
                       // does not parse: snapshot.mismatch written
    NotFormat2         // a manifest of another format (the Docker image's format-1 restore): left to its entrypoint
};

struct StartupReport {
    cs::Sequence seq = 0;    // the manifest's block
    std::string snapshot;    // its digest, "<state> wallets <n> settings <hex>"
    std::string node;        // the node's, the same way
    std::string detail;      // why it does not parse
};

StartupCheck startupCheck(const std::filesystem::path& nodeDir, cs::Sequence lastSeq, const CheckpointDigest& computed,
                          StartupReport& report);

// db/.importing holds "<backend> <seq>" while an import into db/ is in progress
bool writeImportMarker(const std::string& dbPath, const std::string& backend, cs::Sequence seq);
void removeImportMarker(const std::string& dbPath);

// what an import interrupted by a kill left, found by db/.importing: the database files of the
// marker's backend and db/.backend, caches/hashdb, caches/seqdb, caches/blockhashes.rebuild,
// qs/<seq> and every qs/<digits>.tmp, snapshot.json, snapshot.sources.json, then the marker. db/.lock
// and caches/indexdb stay. False when there is no marker; what lists what was removed.
bool cleanupInterruptedImport(const std::filesystem::path& nodeDir, const std::string& dbPath, std::string& what);

// snapshot.sources.json: [{"key": base58, "address": "ip"}] of the agreed sources, mode 0600
bool writeSources(const std::filesystem::path& nodeDir, const std::vector<SourceManifest>& used);

}  // namespace cs::snapshot_bootstrap

#endif  // SNAPSHOT_BOOTSTRAP_HPP
