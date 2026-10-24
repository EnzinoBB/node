#pragma once

#include <string>

class Config;

namespace snapshot_export {

// --export-snapshot <dir>: builds the snapshot of the latest periodic checkpoint (qs/<seq> with a
// state_digest.bin) of this stopped node into <dir>/<seq>, signed with the node's key. 0 on success.
int run(const Config& config, const std::string& outDir);

}  // namespace snapshot_export
