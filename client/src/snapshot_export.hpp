#pragma once

#include <string>

class Config;

namespace snapshot_export {

// --export-snapshot <dir>: builds the snapshot of the latest periodic checkpoint (qs/<seq> with a
// state_digest.bin) of this stopped node into <dir>/<seq>, signed with the node's key. 0 on success.
int run(const Config& config, const std::string& outDir);

// --import-snapshot <snapshot.json>: restores the format-2 snapshot whose manifest and files are in
// that folder into this node's empty database (configured db_backend, lmdb or rocksdb). 0 on
// success, 2 when nothing was restored (the reason is printed), as snapshot-restore.sh.
int runImport(const Config& config, const std::string& manifestFile);

// --download-snapshot [url]: downloads the snapshot.json at url (default kDefaultSnapshotUrl) and
// the files it lists (published next to it as s<seq>-<name>) with curl into snapshot-download/,
// checks every file against the manifest, then restores it as --import-snapshot does. Run by hand
// or by the Docker entrypoint, never by a normal start. 0 on success, 2 otherwise; a failed
// download leaves snapshot-download/ so that the next run resumes it.
int runDownload(const Config& config, const std::string& url);

inline const char* const kDefaultSnapshotUrl = "https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/snapshot.json";

}  // namespace snapshot_export
