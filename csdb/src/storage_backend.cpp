#include <csdb/storage_backend.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

#if defined(CSDB_USE_ROCKSDB) && defined(CSDB_USE_BERKELEYDB)
#include <csdb/database_berkeleydb.hpp>
#include <csdb/database_rocksdb.hpp>
#elif defined(CSDB_USE_ROCKSDB)
#include <csdb/database_rocksdb.hpp>
#else
#include <csdb/database_berkeleydb.hpp>
#endif
#include <csdb/database_lmdb.hpp>

namespace fs = std::filesystem;

namespace csdb::backend {
namespace {
constexpr std::uintmax_t kEmptyBytes = 64 * 1024;

bool bigger(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > kEmptyBytes;
}

bool anyMatching(const fs::path& dir, const std::string& prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().filename().string().rfind(prefix, 0) == 0) return true;
    }
    return false;
}

bool hasBlocks(const std::string& name, const std::string& dir) {
    // only called when the backend's files exist, so opening creates nothing new
    auto db = open(name, dir);
    if (!db || !db->is_open()) return false;
    auto it = db->new_iterator();
    it->seek_to_first();
    return it->is_valid();
}

bool syncWrite(const fs::path& path, const std::string& text) {
    const auto tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}
}  // namespace

bool isKnown(const std::string& name) {
    return name == "berkeleydb" || name == "lmdb" || name == "rocksdb";
}

std::vector<std::string> detect(const std::string& dbPath) {
    const fs::path dir(dbPath);
    std::vector<std::string> found;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return found;   // first start: no db/ yet
    }
    // fingerprints from the files alone; an engine is opened only where the files cannot tell
    if (bigger(dir / "blockchain.db") && fs::exists(dir / "sequence.db")) found.push_back("berkeleydb");
    if (bigger(dir / "lmdb" / "data.mdb")) found.push_back("lmdb");     // a fresh LMDB file is 12 KB
    const bool rocksdb = fs::exists(dir / "CURRENT") && anyMatching(dir, "MANIFEST-");
    if (found.empty() && rocksdb) {
        // a fresh, empty RocksDB has CURRENT and a MANIFEST too, but no table file yet: only then
        // is the small database opened to look for blocks
        bool tables = false;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() == ".sst") { tables = true; break; }
        }
        if (tables || hasBlocks("rocksdb", dbPath)) found.push_back("rocksdb");
        return found;
    }
    if (rocksdb) found.push_back("rocksdb");
    if (found.size() > 1) {
        // two formats only decide something when both hold blocks: an empty one, e.g. a target
        // created by a node started with the other backend, does not count
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [&](const std::string& b) { return b != "berkeleydb" && !hasBlocks(b, dbPath); }),
                    found.end());
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::string readMarker(const std::string& dbPath) {
    std::ifstream f(fs::path(dbPath) / kMarkerFile);
    std::string name;
    f >> name;
    return isKnown(name) ? name : std::string{};
}

bool writeMarker(const std::string& dbPath, const std::string& name) {
    return isKnown(name) && syncWrite(fs::path(dbPath) / kMarkerFile, name + "\n");
}

std::shared_ptr<Database> open(const std::string& name, const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (name == "lmdb") {
        auto db = std::make_shared<DatabaseLmdb>();
        return db->open(dir + "/lmdb") ? db : nullptr;
    }
#if defined(CSDB_USE_ROCKSDB)
    if (name == "rocksdb") {
        auto db = std::make_shared<DatabaseRocksDB>();
        return db->open(dir) ? db : nullptr;
    }
#endif
#if !defined(CSDB_USE_ROCKSDB) || defined(CSDB_USE_BERKELEYDB)
    if (name == "berkeleydb") {
        auto db = std::make_shared<DatabaseBerkeleyDB>();
        return db->open(dir) ? db : nullptr;
    }
#endif
    return nullptr;
}

std::vector<fs::path> files(const std::string& name, const std::string& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (name == "lmdb") {
        if (fs::exists(fs::path(dir) / "lmdb")) out.push_back(fs::path(dir) / "lmdb");
        return out;
    }
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const auto f = e.path().filename().string();
        const auto ext = e.path().extension().string();
        bool mine = false;
        if (name == "berkeleydb") {
            mine = f == "blockchain.db" || f == "sequence.db" || f == "contracts.db" || f.rfind("__db.", 0) == 0 || f.rfind("log.", 0) == 0;
        }
        else if (name == "rocksdb") {
            mine = f == "CURRENT" || f == "IDENTITY" || f == "LOCK" || f == "LOG" || f.rfind("LOG.old.", 0) == 0
                || f.rfind("MANIFEST-", 0) == 0 || f.rfind("OPTIONS-", 0) == 0 || ext == ".sst" || ext == ".blob"
                || (ext == ".log" && std::all_of(f.begin(), f.end() - 4, [](unsigned char c) { return std::isdigit(c) != 0; }));
        }
        if (mine) out.push_back(e.path());
    }
    return out;
}

}  // namespace csdb::backend
