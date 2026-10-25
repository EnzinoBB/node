#include <csnode/snapshot_bootstrap.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>

#include <csdb/storage_backend.hpp>
#include <csnode/snapshot_manifest.hpp>

namespace cs::snapshot_bootstrap {

namespace fs = std::filesystem;

namespace {

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string hexOf(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string digestText(const std::string& state, uint64_t wallets, const std::string& settings) {
    return state + " wallets " + std::to_string(wallets) + " settings " + settings;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        }
        else {
            out.push_back(c);
        }
    }
    return out;
}

// a small file written in full or not at all, readable by the owner only
bool writeFile(const fs::path& path, const std::string& content, mode_t mode = 0644) {
    const fs::path tmp = path.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) {
        return false;
    }
    size_t done = 0;
    while (done < content.size()) {
        const auto n = ::write(fd, content.data() + done, content.size() - done);
        if (n <= 0) {
            ::close(fd);
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    ::fchmod(fd, mode);
    ::fsync(fd);
    ::close(fd);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace

std::optional<Skip> notNeeded(const SnapshotData& cfg, const fs::path& nodeDir, const std::string& dbPath, const std::string& backend,
                              bool setBcTop, bool* removedTmp) {
    if (removedTmp) {
        *removedTmp = false;
    }
    if (cfg.fetch == "off") {
        return Skip{Skip::FetchOff, "fetch=off"};
    }
    if (setBcTop) {
        return Skip{Skip::SetBcTop, "--set-bc-top"};
    }
    // false: no engine is opened (a small file counts as a database); db/.lock is this process's anyway
    const auto found = csdb::backend::detect(dbPath, false);
    if (!found.empty()) {
        return Skip{Skip::Database, dbPath + " holds a " + found.front() + " database"};
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(nodeDir / "qs", ec)) {
        const auto name = e.path().filename().string();
        if (allDigits(name) && e.is_directory(ec)) {
            return Skip{Skip::Checkpoint, "qs/" + name + " holds a checkpoint of a previous run (remove qs/ to fetch a snapshot)"};
        }
    }
    if (backend != "lmdb" && backend != "rocksdb") {
        return Skip{Skip::Backend, "a " + backend + " database cannot start above block 0"};
    }
    const fs::path tmp = nodeDir / kTmpDir;
    if (fs::exists(tmp, ec)) {
        fs::remove_all(tmp, ec);
        if (removedTmp) {
            *removedTmp = true;
        }
    }
    return std::nullopt;
}

bool processWhileBootstrapping(MsgTypes type) {
    return type == MsgTypes::SnapshotManifestReply || type == MsgTypes::SnapshotPieceReply;
}

StartupCheck startupCheck(const fs::path& nodeDir, cs::Sequence lastSeq, const CheckpointDigest& computed, StartupReport& report) {
    report = StartupReport{};
    std::error_code ec;
    const fs::path manifestPath = nodeDir / kManifestFile;
    if (!fs::exists(manifestPath, ec)) {
        return StartupCheck::NoManifest;
    }
    if (fs::exists(nodeDir / kVerifiedFile, ec)) {
        return StartupCheck::AlreadyVerified;
    }
    report.node = digestText(hexOf(computed.digest.data(), computed.digest.size()), computed.wallets,
                             hexOf(computed.settings.data(), computed.settings.size()));
    const auto mismatch = [&](const std::string& why) {
        report.detail = why;
        writeFile(nodeDir / kMismatchFile, "snapshot.json at block " + std::to_string(report.seq) + ": " +
                                               (why.empty() ? "snapshot " + report.snapshot + ", node " + report.node : why) + "\n");
        return StartupCheck::Mismatch;
    };

    std::string json;
    {
        std::ifstream in(manifestPath, std::ios::binary);
        if (!in) {
            return mismatch("cannot read snapshot.json");
        }
        std::ostringstream all;
        all << in.rdbuf();
        json = all.str();
    }
    if (json.size() > SnapshotManifest::kMaxJsonSize) {
        return mismatch("snapshot.json is larger than 1 MB");
    }
    // the Docker image's format-1 restore (packaging/docker/snapshot-restore.sh) writes its own
    // manifest as snapshot.json, without "format": its entrypoint compares that digest
    {
        boost::property_tree::ptree tree;
        try {
            std::istringstream in(json);
            boost::property_tree::read_json(in, tree);
        }
        catch (const std::exception&) {
            return mismatch("snapshot.json is not JSON");
        }
        if (tree.get<std::string>("format", "") != std::to_string(SnapshotManifest::kFormat)) {
            return StartupCheck::NotFormat2;
        }
    }
    std::string error;
    const auto m = SnapshotManifest::fromJson(json, &error);
    if (!m) {
        return mismatch("snapshot.json is refused: " + error);
    }
    report.seq = m->seq;
    report.snapshot = digestText(lower(m->stateDigest), m->wallets, lower(m->settingsDigest));
    if (lastSeq < m->seq) {
        // the quick start of an import always reaches the snapshot's block: a chain below it did not
        // load the restored state
        return mismatch("the chain ends at block " + std::to_string(lastSeq) + ", below the snapshot's block " + std::to_string(m->seq));
    }
    if (lastSeq > m->seq) {
        // the chain moved past the snapshot before a start verified it: the digest at its block is gone
        writeFile(nodeDir / kVerifiedFile, "skipped: chain at " + std::to_string(lastSeq) + ", snapshot at " + std::to_string(m->seq) + "\n");
        return StartupCheck::Skipped;
    }
    if (report.snapshot != report.node) {
        return mismatch(std::string{});
    }
    writeFile(nodeDir / kVerifiedFile, "verified at block " + std::to_string(m->seq) + "\n");
    return StartupCheck::Verified;
}

bool writeImportMarker(const std::string& dbPath, const std::string& backend, cs::Sequence seq) {
    std::error_code ec;
    fs::create_directories(dbPath, ec);
    return writeFile(fs::path(dbPath) / kImportingFile, backend + " " + std::to_string(seq) + "\n");
}

void removeImportMarker(const std::string& dbPath) {
    std::error_code ec;
    fs::remove(fs::path(dbPath) / kImportingFile, ec);
}

bool cleanupInterruptedImport(const fs::path& nodeDir, const std::string& dbPath, std::string& what) {
    what.clear();
    std::error_code ec;
    const fs::path marker = fs::path(dbPath) / kImportingFile;
    if (!fs::exists(marker, ec)) {
        return false;
    }
    std::string backend;
    std::string seq;
    {
        std::ifstream in(marker);
        in >> backend >> seq;
    }
    std::vector<std::string> removed;
    const auto remove = [&](const fs::path& p, const std::string& name) {
        std::error_code e;
        if (fs::exists(p, e) && fs::remove_all(p, e) > 0 && !e) {
            removed.push_back(name);
        }
    };
    if (csdb::backend::isKnown(backend)) {
        for (const auto& f : csdb::backend::files(backend, dbPath)) {
            remove(f, "db/" + f.filename().string());
        }
    }
    remove(fs::path(dbPath) / csdb::backend::kMarkerFile, std::string("db/") + csdb::backend::kMarkerFile);
    const fs::path caches = nodeDir / "caches";
    remove(caches / "hashdb", "caches/hashdb");
    remove(caches / "seqdb", "caches/seqdb");
    remove(caches / "blockhashes.rebuild", "caches/blockhashes.rebuild");
    // the importer refuses an existing qs/<seq> and notNeeded() any numbered checkpoint: this one is the import's
    if (allDigits(seq)) {
        remove(nodeDir / "qs" / seq, "qs/" + seq);
    }
    for (const auto& e : fs::directory_iterator(nodeDir / "qs", ec)) {
        const auto name = e.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0 && allDigits(name.substr(0, name.size() - 4))) {
            remove(e.path(), "qs/" + name);
        }
    }
    remove(nodeDir / kManifestFile, kManifestFile);
    remove(nodeDir / kSourcesFile, kSourcesFile);
    fs::remove(marker, ec);
    for (const auto& r : removed) {
        what += (what.empty() ? "" : ", ") + r;
    }
    if (what.empty()) {
        what = "nothing but the marker";
    }
    return true;
}

bool writeSources(const fs::path& nodeDir, const std::vector<SourceManifest>& used) {
    std::string out = "[";
    for (size_t i = 0; i < used.size(); ++i) {
        const auto& s = used[i];
        out += std::string(i == 0 ? "\n" : ",\n") + " {\"key\": \"" + EncodeBase58(s.key.data(), s.key.data() + s.key.size()) +
               "\", \"address\": \"" + jsonEscape(s.address) + "\"}";
    }
    out += used.empty() ? "]\n" : "\n]\n";
    return writeFile(nodeDir / kSourcesFile, out, 0600);
}

}  // namespace cs::snapshot_bootstrap
