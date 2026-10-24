#include "snapshot_export.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <config.hpp>
#include <version.hpp>

#include <csdb/database.hpp>
#include <csdb/storage_backend.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/statedigest.hpp>

namespace snapshot_export {

namespace {
namespace fs = std::filesystem;

std::ostream& say() {
    return std::cout << "export-snapshot: ";
}

// the highest periodic checkpoint that records its state (qs/0, the shutdown checkpoint, is not one)
cs::Sequence latestCheckpoint() {
    cs::Sequence best = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("qs", ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        const cs::Sequence seq = std::stoull(name);
        if (seq > best && fs::exists(entry.path() / cs::CheckpointDigest::kFileName, ec)) {
            best = seq;
        }
    }
    return best;
}

std::string nodeVersion() {
    std::ifstream f("VERSION");
    std::string v;
    if (f && std::getline(f, v) && !v.empty() && v.size() <= 64) {
        return v;
    }
    return std::to_string(Config::getNodeVersion()) + "." + std::to_string(Config::getMinorNodeVersion()) + " " +
           std::string(client::Version::GIT_SHA1).substr(0, 8);
}
}  // namespace

int run(const Config& config, const std::string& outDir) {
    const auto& storage = config.getStorageSettings();
    const std::string dbPath = config.getPathToDB();
    const auto S = latestCheckpoint();
    if (S == 0) {
        say() << "no periodic checkpoint with a state digest in qs/ (checkpoints of earlier builds have none)" << std::endl;
        return 1;
    }
    auto db = csdb::backend::open(storage.dbBackend, dbPath);
    if (!db || !db->is_open()) {
        say() << "cannot open the " << storage.dbBackend << " database in " << dbPath << std::endl;
        return 1;
    }
    cs::Sequence first = 0;
    {
        auto it = db->new_iterator();
        it->seek_to_first();
        if (!it->is_valid()) {
            say() << "the database holds no block" << std::endl;
            return 1;
        }
        first = it->key();
    }

    cs::SnapshotBuildInput in;
    in.seq = S;
    in.checkpointDir = fs::path("qs") / std::to_string(S);
    in.firstStored = first;
    in.snapshotBlocks = config.getSnapshotSettings().snapshotBlocks;
    in.nodeVersion = nodeVersion();
    in.blockBytes = [&db](cs::Sequence seq, cs::Bytes& out) {
        return db->get(static_cast<uint32_t>(seq), &out);
    };
    in.contracts = [&db](const std::function<bool(const cs::Bytes&, const cs::Bytes&)>& visit) {
        return db->forEachContractData(visit);
    };

    say() << "checkpoint " << S << ", blocks from " << std::max<cs::Sequence>(first, S > in.snapshotBlocks ? S - in.snapshotBlocks : 0)
          << ", into " << outDir << std::endl;
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const auto r = cs::buildSnapshot(in, outDir, config.getMyPrivateKey());
    if (!r.ok) {
        say() << "failed: " << r.error << std::endl;
        return 1;
    }
    uint64_t bytes = 0;
    for (const auto& f : r.manifest.files) {
        bytes += f.size;
    }
    say() << "built " << r.dir.string() << ": block " << r.manifest.seq << ", state digest " << r.manifest.stateDigest
          << ", " << r.manifest.wallets << " wallets, " << r.manifest.files.size() << " files, " << bytes / (1 << 20) << " MB" << std::endl;
    return 0;
}

}  // namespace snapshot_export
