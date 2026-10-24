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

}  // namespace snapshot_export
