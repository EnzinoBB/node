#include "snapshot_export.hpp"
#include "dbcheck.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <config.hpp>
#include <version.hpp>

#include <csdb/database.hpp>
#include <csdb/storage_backend.hpp>
#include <csnode/snapshot_bootstrap.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_importer.hpp>
#include <csnode/snapshot_manifest.hpp>
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
        // printed into the manifest's JSON as it is: only plain characters
        for (auto& c : v) {
            if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
                c = '_';
            }
        }
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
    // the export reads the database files directly: never next to a running node (BerkeleyDB would
    // even run recovery on its live environment), never into an empty folder or another backend's
    if (dbcheck::lockedByAnotherProcess(dbPath)) {
        say() << "a node is running on " << dbPath << ": stop it first" << std::endl;
        return 1;
    }
    // from the files alone: the start-up check has already decided the folder holds the configured
    // backend, and opening an engine twice in one process can fail on its own lock
    const auto found = csdb::backend::detect(dbPath, false);
    if (std::find(found.begin(), found.end(), storage.dbBackend) == found.end()) {
        say() << dbPath << " holds no " << storage.dbBackend << " database (the configured db_backend)" << std::endl;
        return 1;
    }
    // the output must not be a folder of the node's data: the builder replaces numbered folders in it
    {
        std::error_code e1, e2;
        const auto out = fs::weakly_canonical(outDir, e1);
        const auto here = fs::weakly_canonical(fs::current_path(), e2);
        if (e1 || out == here) {
            say() << outDir << " is the node's folder, choose another one" << std::endl;
            return 1;
        }
        for (const fs::path data : {fs::path("qs"), fs::path(dbPath), fs::path("caches")}) {
            const auto d = fs::weakly_canonical(data, e2);
            if (!e2 && std::mismatch(d.begin(), d.end(), out.begin(), out.end()).first == d.end()) {
                say() << outDir << " is inside the node's data (" << data.string() << "), choose another folder" << std::endl;
                return 1;
            }
        }
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
    // no contract records: the contracts database holds each contract's latest state, later than the
    // checkpoint; the checkpoint carries the states at its block

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

int runImport(const Config& config, const std::string& manifestFile) {
    auto sayImport = []() -> std::ostream& { return std::cout << "import-snapshot: "; };
    const std::string dbPath = config.getPathToDB();
    // db/ first, so that the import holds db/.lock while it writes: a node started in this folder
    // meanwhile finds it in use
    {
        std::error_code ec;
        fs::create_directories(dbPath, ec);
    }
    if (dbcheck::lockedByAnotherProcess(dbPath)) {
        sayImport() << "a node is running on " << dbPath << ": stop it first" << std::endl;
        return 2;
    }
    std::ifstream in(manifestFile, std::ios::binary);
    if (!in) {
        sayImport() << "cannot read " << manifestFile << std::endl;
        return 2;
    }
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    const auto manifest = cs::SnapshotManifest::fromJson(json, &error);
    if (!manifest) {
        sayImport() << "not restored: the manifest is refused: " << error << std::endl;
        return 2;
    }
    const auto backend = config.getStorageSettings().dbBackend;
    sayImport() << "block " << manifest->seq << " (blocks " << manifest->firstBlock << ".." << manifest->seq << "), state digest "
                << manifest->stateDigest << ", signed by " << manifest->nodeKey << ", into a new " << backend << " database" << std::endl;
    // a kill during the import leaves db/.importing: the next start removes what it wrote (main.cpp).
    // Only into an empty folder: the marker must never name a database or a checkpoint of the node's own
    {
        std::error_code ec;
        if (!csdb::backend::detect(dbPath, true).empty() || fs::exists(fs::path("qs") / std::to_string(manifest->seq), ec)) {
            sayImport() << "not restored: " << dbPath << " already holds a database, or qs/" << manifest->seq << " exists" << std::endl;
            return 2;
        }
    }
    if (!cs::snapshot_bootstrap::writeImportMarker(dbPath, backend, manifest->seq)) {
        sayImport() << "not restored: cannot write " << dbPath << "/" << cs::snapshot_bootstrap::kImportingFile << std::endl;
        return 2;
    }
    const auto r = cs::importSnapshot(fs::path(manifestFile).parent_path(), *manifest, fs::current_path(), dbPath, backend);
    cs::snapshot_bootstrap::removeImportMarker(dbPath);
    if (!r.ok) {
        sayImport() << "not restored: " << r.error << (r.fileAtFault.empty() ? "" : " (" + r.fileAtFault + ")") << std::endl;
        return 2;
    }
    sayImport() << "restored block " << manifest->seq << std::endl;
    return 0;
}

}  // namespace snapshot_export
