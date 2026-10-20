#include <csdb/storage_convert.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

#include <csdb/pool.hpp>
#include <csdb/storage_backend.hpp>

namespace fs = std::filesystem;

namespace csdb::convert {
namespace {
const char* const kStateFile = ".convert";
const char* const kNewDir = ".convert-new";

std::string hex(const cs::Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

uint64_t folderBytes(const fs::path& p) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    }
    return n;
}

uint64_t pathBytes(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return folderBytes(p);
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

// makes renames and removals in the folder durable
bool syncDir(const fs::path& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool syncWrite(const fs::path& path, const std::string& text) {
    const auto tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    // a renamed state lost on power failure would point above blocks already removed
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0 && syncDir(path.parent_path());
}

void say(const Options& o, const std::string& m) {
    if (o.log) o.log(m);
}

Result failed(Result r, const std::string& error) {
    r.ok = false;
    r.error = error;
    return r;
}

bool anyDone(const State& s) {
    return (s.ascending ? s.doneUpTo : s.doneDownTo) >= 0;
}

// the next batch to convert after what the state records; false when every block is converted
bool nextBatch(const State& s, uint32_t batch, int64_t& lo, int64_t& hi) {
    if (s.ascending) {
        lo = s.doneUpTo < 0 ? int64_t(s.first) : s.doneUpTo + 1;
        hi = std::min<int64_t>(s.last, lo + int64_t(batch) - 1);
        return lo <= int64_t(s.last);
    }
    hi = s.doneDownTo < 0 ? int64_t(s.last) : s.doneDownTo - 1;
    lo = std::max<int64_t>(s.first, hi - int64_t(batch) + 1);
    return hi >= int64_t(s.first);
}

// The last part: the old files go, the new ones move into db/. Repeatable after a crash at any point:
// the two backends' file names never overlap, and entries already moved are no longer in .convert-new.
Result finish(const Options& opt, const State& st, Result r, std::chrono::steady_clock::time_point started) {
    const fs::path dir(opt.dbPath);
    const fs::path newDir = dir / kNewDir;
    std::error_code ec;
    // the old files go only when the new database is there: still in .convert-new, or already
    // moved into db/ by an interrupted finish
    if (backend::files(st.to, newDir.string()).empty() && backend::files(st.to, opt.dbPath).empty()) {
        return failed(r, "the " + st.to + " database is neither in db/.convert-new nor in db/: the " + opt.from + " files are kept");
    }
    for (const auto& p : backend::files(opt.from, opt.dbPath)) {
        fs::remove_all(p, ec);
        if (ec) return failed(r, "cannot remove " + p.string() + ": " + ec.message());
    }
    if (fs::exists(newDir, ec)) {
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(newDir, ec)) entries.push_back(e.path());
        if (ec) return failed(r, "cannot list " + newDir.string() + ": " + ec.message());
        for (const auto& p : entries) {
            fs::rename(p, dir / p.filename(), ec);
            if (ec) return failed(r, "cannot move " + p.string() + " into " + dir.string() + ": " + ec.message());
        }
        syncDir(dir);
        fs::remove(newDir, ec);
        if (ec) return failed(r, "cannot remove " + newDir.string() + ": " + ec.message());
    }
    if (!backend::writeMarker(opt.dbPath, st.to)) return failed(r, "cannot write db/" + std::string(backend::kMarkerFile));
    fs::remove(dir / kStateFile, ec);
    syncDir(dir);

    r.bytesAfter = folderBytes(dir);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    r.ok = true;
    say(opt, "conversion done: " + std::to_string(r.blocks) + " blocks, " + std::to_string(r.contracts) + " contract records, "
             + std::to_string(r.bytesBefore >> 20) + " MB -> " + std::to_string(r.bytesAfter >> 20) + " MB in "
             + std::to_string(static_cast<int>(r.seconds)) + " s");
    return r;
}
}  // namespace

bool readState(const std::string& dbPath, State& s) {
    std::ifstream f(fs::path(dbPath) / kStateFile);
    if (!f) return false;
    s = State{};
    std::string line;
    int seen = 0;
    bool down = false, up = false;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "from") { s.from = v; ++seen; }
            else if (k == "to") { s.to = v; ++seen; }
            else if (k == "first") { s.first = static_cast<uint32_t>(std::stoul(v)); ++seen; }
            else if (k == "last") { s.last = static_cast<uint32_t>(std::stoul(v)); ++seen; }
            else if (k == "done_down_to") { s.doneDownTo = std::stoll(v); down = true; }
            else if (k == "next_prev_hash") { s.nextPrevHash = v; }
            else if (k == "done_up_to") { s.doneUpTo = std::stoll(v); up = true; }
            else if (k == "done_up_to_hash") { s.doneUpToHash = v; }
            else if (k == "order") {
                if (v != "asc" && v != "desc") return false;
                s.ascending = v == "asc";
            }
            else if (k == "contracts") { s.contractsCopied = v == "copied"; }
            else if (k == "phase") { s.moving = v == "moving"; }
        }
        catch (const std::exception&) {
            return false;
        }
    }
    // no order key: written before the order was recorded, when every conversion went newest first.
    // The boundary must be the one of the order.
    const bool boundary = s.ascending ? up && !down : down && !up;
    const int64_t done = s.ascending ? s.doneUpTo : s.doneDownTo;
    const bool doneInRange = done == -1 || (done >= s.first && done <= s.last);
    return seen == 4 && boundary && backend::isKnown(s.from) && backend::isKnown(s.to) && s.from != s.to && s.first <= s.last && doneInRange;
}

bool writeState(const std::string& dbPath, const State& s) {
    std::ostringstream o;
    o << "from=" << s.from << "\nto=" << s.to << "\nfirst=" << s.first << "\nlast=" << s.last;
    if (s.ascending) o << "\norder=asc\ndone_up_to=" << s.doneUpTo << "\ndone_up_to_hash=" << s.doneUpToHash;
    else o << "\norder=desc\ndone_down_to=" << s.doneDownTo << "\nnext_prev_hash=" << s.nextPrevHash;
    o << "\ncontracts=" << (s.contractsCopied ? "copied" : "pending")
      << "\nphase=" << (s.moving ? "moving" : "blocks") << "\n";
    return syncWrite(fs::path(dbPath) / kStateFile, o.str());
}

uint64_t estimatePeakBytes(const std::string& dbPath, const std::string& from, const std::string& to, uint32_t batch) {
    const auto files = backend::files(from, dbPath);
    if (files.empty()) return 0;    // opening would create an empty database
    uint64_t bytes = 0;
    for (const auto& p : files) bytes += pathBytes(p);
    constexpr uint64_t kMargin = 256ULL << 20;
    // LMDB never shrinks data.mdb and RocksDB deletes are tombstones compacted later: the old files
    // keep their size until they are deleted at the end, so the new database needs room for all of it,
    // less what a resumed conversion already holds in .convert-new
    if (from != "berkeleydb") {
        std::error_code ec;
        const fs::path dir(dbPath);
        const uint64_t done = fs::exists(dir / kStateFile, ec) ? folderBytes(dir / kNewDir) : 0;
        // the new database in its own format: RocksDB's compressed blocks take several times their
        // size in LMDB or BerkeleyDB
        const uint64_t target = (from == "rocksdb" && to != "rocksdb") ? bytes * kExpansionFromRocksDb : bytes;
        return (target > done ? target - done : 0) + kMargin;
    }
    auto db = backend::open(from, dbPath);
    if (!db) return bytes;
    auto it = db->new_iterator();
    if (!it) return bytes;
    it->seek_to_first();
    if (!it->is_valid()) return 0;
    const uint64_t first = it->key();
    it->seek_to_last();
    const uint64_t blocks = it->key() - first + 1;
    const uint64_t perBlock = bytes / std::max<uint64_t>(blocks, 1);
    // BerkeleyDB gives each removed batch back: one batch in the new format, plus the same again for
    // compaction and the engines' logs
    return 2 * perBlock * std::min<uint64_t>(batch, blocks) + kMargin;
}

Result run(const Options& opt) {
    const auto started = std::chrono::steady_clock::now();
    Result r;
    const fs::path dir(opt.dbPath);
    const std::string newDir = (dir / kNewDir).string();
    if (!backend::isKnown(opt.from) || !backend::isKnown(opt.to) || opt.from == opt.to) {
        return failed(r, "cannot convert from " + opt.from + " to " + opt.to);
    }
    if (opt.batch == 0) return failed(r, "the batch size must be at least 1");

    State st;
    const bool resuming = fs::exists(dir / kStateFile);
    if (resuming) {
        if (!readState(opt.dbPath, st)) return failed(r, "db/.convert is unreadable");
        if (st.from != opt.from || st.to != opt.to) {
            return failed(r, "an unfinished conversion from " + st.from + " to " + st.to + " is in db/.convert");
        }
        // blocks recorded as converted are only in .convert-new: without it, stop before removing more
        if (!st.moving && (anyDone(st) || st.contractsCopied) && !fs::exists(newDir)) {
            return failed(r, "db/.convert records converted data but db/.convert-new is missing");
        }
        // files of the target backend that appeared in db/ during a pause (another node started there)
        // would collide with the new ones when they move into db/
        if (!st.moving) {
            const auto existing = backend::files(opt.to, opt.dbPath);
            if (!existing.empty()) {
                return failed(r, "db/ holds " + opt.to + " files (" + existing.front().string() + ") that are not part of the conversion: remove them to resume");
            }
        }
        say(opt, "resuming the conversion from " + st.from + " to " + st.to
                 + (!anyDone(st) ? std::string(" from the start")
                    : st.ascending ? " above block " + std::to_string(st.doneUpTo) : " below block " + std::to_string(st.doneDownTo)));
    }
    else {
        // the new database must start empty, and its files must not collide with others when moved into db/
        if (fs::exists(newDir)) return failed(r, "db/.convert-new exists but db/.convert does not: remove db/.convert-new to convert");
        const auto existing = backend::files(opt.to, opt.dbPath);
        if (!existing.empty()) {
            return failed(r, "db/ already holds " + opt.to + " files (" + existing.front().string() + "): remove them to convert");
        }
    }
    r.bytesBefore = folderBytes(dir);

    if (st.moving) {
        r.blocks = uint64_t(st.last) - st.first + 1;
        say(opt, "finishing the conversion: moving the " + st.to + " files into " + opt.dbPath);
        return finish(opt, st, r, started);
    }

    auto oldDb = backend::open(opt.from, opt.dbPath);
    if (!oldDb) return failed(r, "cannot open the " + opt.from + " database");
    if (!resuming) {
        // the iterator closes before anything is written: a BerkeleyDB cursor holds page locks
        auto it = oldDb->new_iterator();
        if (!it) return failed(r, "cannot read the " + opt.from + " database");
        it->seek_to_first();
        if (!it->is_valid()) return failed(r, "the " + opt.from + " database holds no block");
        st.from = opt.from;
        st.to = opt.to;
        st.first = it->key();
        // BerkeleyDB stores blocks in a Recno table, record = sequence + 1: the first put of a pruned
        // database would create every record below it at once (millions on mainnet), and run out of
        // lock entries and memory
        if (opt.to == "berkeleydb" && st.first != 0) {
            return failed(r, "a pruned block database cannot be converted to berkeleydb; use lmdb or rocksdb (its first block is "
                             + std::to_string(st.first) + ")");
        }
        // Newest first only from berkeleydb, which gives space back only from the end of its file. LMDB
        // and RocksDB do not shrink during the conversion anyway; oldest first, a berkeleydb target grows
        // record by record instead of creating every lower record with its first put
        st.ascending = opt.from != "berkeleydb";
        it->seek_to_last();
        if (!it->is_valid() || it->key() < st.first) return failed(r, "cannot find the last block of the " + opt.from + " database");
        st.last = it->key();
        it.reset();
        if (!writeState(opt.dbPath, st)) return failed(r, "cannot write db/.convert");
        say(opt, "converting blocks " + std::to_string(st.first) + ".." + std::to_string(st.last) + " from " + st.from + " to " + st.to
                 + (st.ascending ? ", oldest first" : ", newest first"));
    }

    auto newDb = backend::open(opt.to, newDir);
    if (!newDb) return failed(r, "cannot create the " + opt.to + " database in db/.convert-new");

    // contract records: small, copied at once; removed from the old database only with its files
    if (!st.contractsCopied) {
        std::vector<std::pair<cs::Bytes, cs::Bytes>> records;
        if (!oldDb->forEachContractData([&](const cs::Bytes& k, const cs::Bytes& v) { records.emplace_back(k, v); return true; })) {
            return failed(r, "cannot read the contract records: " + oldDb->last_error_message());
        }
        for (const auto& [k, v] : records) {
            if (!newDb->updateContractData(k, v)) return failed(r, "cannot write a contract record: " + newDb->last_error_message());
        }
        if (!newDb->flush()) return failed(r, "cannot flush the " + opt.to + " database: " + newDb->last_error_message());
        for (const auto& [k, v] : records) {
            cs::Bytes back;
            if (!newDb->getContractData(k, back) || back != v) return failed(r, "a contract record differs after copying");
        }
        st.contractsCopied = true;
        if (!writeState(opt.dbPath, st)) return failed(r, "cannot write db/.convert");
        say(opt, "copied " + std::to_string(records.size()) + " contract records");
    }

    // A batch recorded before a crash may still be partly in the old database: remove what is left
    // of it, each block only if the new database holds the same bytes under its sequence
    if (anyDone(st)) {
        std::vector<cs::Bytes> leftovers;
        const int64_t bottom = st.ascending ? std::max<int64_t>(st.first, st.doneUpTo - int64_t(opt.batch) + 1) : st.doneDownTo;
        const int64_t top = st.ascending ? st.doneUpTo : std::min<int64_t>(st.last, st.doneDownTo + int64_t(opt.batch) - 1);
        for (int64_t s = bottom; s <= top; ++s) {
            cs::Bytes b, inNew, byKey;
            if (!oldDb->get(static_cast<uint32_t>(s), &b)) continue;
            auto p = csdb::Pool::from_binary(cs::Bytes(b));
            if (!p.is_valid()) continue;
            auto key = p.hash().to_binary();
            if (newDb->get(static_cast<uint32_t>(s), &inNew) && inNew == b && oldDb->get(key, &byKey) && byKey == b) {
                leftovers.push_back(std::move(key));
            }
        }
        if (!leftovers.empty()) {
            if (!oldDb->remove_batch(leftovers)) return failed(r, "cannot remove converted blocks: " + oldDb->last_error_message());
            oldDb->compact();
            say(opt, "removed " + std::to_string(leftovers.size()) + " blocks already converted before the restart");
        }
    }

    int64_t lo = 0, hi = 0;
    while (nextBatch(st, opt.batch, lo, hi)) {
        std::vector<Database::PendingWrite> blocks;
        blocks.reserve(static_cast<size_t>(hi - lo + 1));
        // the hash link to the converted part: newest first, the hash the top block must have; oldest
        // first, the previous_hash the lowest block must have. Blocks are read, and written, in the
        // conversion's order.
        std::string link = st.ascending ? st.doneUpToHash : st.nextPrevHash;
        for (int64_t i = 0; i <= hi - lo; ++i) {
            const int64_t s = st.ascending ? lo + i : hi - i;
            const auto seq = static_cast<uint32_t>(s);
            const auto at = "block " + std::to_string(s);
            cs::Bytes bytes;
            if (!oldDb->get(seq, &bytes)) return failed(r, at + " is missing in the " + opt.from + " database");
            auto pool = csdb::Pool::from_binary(cs::Bytes(bytes));
            if (!pool.is_valid() || pool.sequence() != seq) return failed(r, at + " does not parse");
            auto key = pool.hash().to_binary();
            // the hash index must lead to this very block: removal goes by hash. Compared by bytes,
            // as seq_no() returns sequence + 1 on BerkeleyDB and RocksDB and the sequence on LMDB
            cs::Bytes byKey;
            if (!oldDb->get(key, &byKey) || byKey != bytes) return failed(r, at + " is stored under another hash");
            if (st.ascending) {
                if (!link.empty() && hex(pool.previous_hash().to_binary()) != link) {
                    return failed(r, at + " does not follow block " + std::to_string(s - 1));
                }
                link = hex(key);
            }
            else {
                if (!link.empty() && hex(key) != link) {
                    return failed(r, at + " is not the previous block of block " + std::to_string(s + 1));
                }
                link = hex(pool.previous_hash().to_binary());
            }
            blocks.push_back(Database::PendingWrite{std::move(key), seq, std::move(bytes)});
        }
        if (opt.hook) opt.hook(Step::Read, uint32_t(lo), uint32_t(hi));

        // put overwrites on every backend, so a batch written before a crash is simply written again
        if (!newDb->put_batch(blocks)) return failed(r, "cannot write blocks " + std::to_string(lo) + ".." + std::to_string(hi) + ": " + newDb->last_error_message());
        if (!newDb->flush()) return failed(r, "cannot flush the " + opt.to + " database: " + newDb->last_error_message());
        if (opt.hook) opt.hook(Step::Written, uint32_t(lo), uint32_t(hi));

        for (const auto& b : blocks) {
            cs::Bytes bySeq, byKey;
            if (!newDb->get(b.seq_no, &bySeq) || bySeq != b.payload || !newDb->get(b.hash_key, &byKey) || byKey != b.payload) {
                return failed(r, "block " + std::to_string(b.seq_no) + " differs after copying");
            }
        }
        if (opt.hook) opt.hook(Step::Verified, uint32_t(lo), uint32_t(hi));

        if (st.ascending) {
            st.doneUpTo = hi;
            st.doneUpToHash = link;
        }
        else {
            st.doneDownTo = lo;
            st.nextPrevHash = link;
        }
        if (!writeState(opt.dbPath, st)) return failed(r, "cannot write db/.convert");
        if (opt.hook) opt.hook(Step::Recorded, uint32_t(lo), uint32_t(hi));

        // only now, with the batch synced in the new database and recorded, may it leave the old one
        std::vector<cs::Bytes> keys;
        keys.reserve(blocks.size());
        for (auto& b : blocks) keys.push_back(std::move(b.hash_key));
        blocks.clear();
        if (!oldDb->remove_batch(keys)) return failed(r, "cannot remove converted blocks from the " + opt.from + " database: " + oldDb->last_error_message());
        if (!oldDb->compact()) say(opt, "compaction of the " + opt.from + " database failed: " + oldDb->last_error_message());
        if (opt.hook) opt.hook(Step::Removed, uint32_t(lo), uint32_t(hi));

        say(opt, "converted blocks " + (st.ascending ? std::to_string(st.first) + ".." + std::to_string(hi)
                                                      : std::to_string(lo) + ".." + std::to_string(st.last)));
        int64_t nextLo = 0, nextHi = 0;
        if (nextBatch(st, opt.batch, nextLo, nextHi) && opt.stopRequested && opt.stopRequested()) {
            r.stopped = true;
            return failed(r, "stopped on request, the conversion resumes at the next start");
        }
    }

    // whole new database: the recorded range, every block, an unbroken hash chain
    {
        auto it = newDb->new_iterator();
        if (!it) return failed(r, "cannot read the converted database");
        it->seek_to_first();
        if (!it->is_valid() || it->key() != st.first) return failed(r, "the converted database does not start at block " + std::to_string(st.first));
        std::string prevHash;
        uint64_t count = 0;
        for (uint32_t expectSeq = st.first; it->is_valid(); it->next(), ++expectSeq, ++count) {
            if (it->key() != expectSeq) return failed(r, "block " + std::to_string(expectSeq) + " is missing after the conversion");
            auto pool = csdb::Pool::from_binary(it->value());
            if (!pool.is_valid() || pool.sequence() != expectSeq) return failed(r, "block " + std::to_string(expectSeq) + " does not parse after the conversion");
            if (!prevHash.empty() && hex(pool.previous_hash().to_binary()) != prevHash) {
                return failed(r, "hash chain broken at block " + std::to_string(expectSeq));
            }
            prevHash = hex(pool.hash().to_binary());
        }
        if (count != uint64_t(st.last) - st.first + 1) return failed(r, "the converted database has " + std::to_string(count) + " blocks");
        r.blocks = count;
    }
    {
        uint64_t n = 0;
        newDb->forEachContractData([&](const cs::Bytes&, const cs::Bytes&) { ++n; return true; });
        r.contracts = n;
    }

    // the old files are deleted next, irreversibly: whatever is left in them must be in the new database
    {
        auto it = oldDb->new_iterator();
        if (!it) return failed(r, "cannot read the " + opt.from + " database");
        for (it->seek_to_first(); it->is_valid(); it->next()) {
            const uint32_t seq = it->key();
            const auto at = "block " + std::to_string(seq);
            if (seq < st.first || seq > st.last) {
                return failed(r, at + " is in the " + opt.from + " database but outside the converted range "
                                 + std::to_string(st.first) + ".." + std::to_string(st.last));
            }
            cs::Bytes inNew;
            if (!newDb->get(seq, &inNew) || inNew != it->value()) {
                return failed(r, at + " left in the " + opt.from + " database differs from the converted one");
            }
        }
    }
    {
        std::string bad;
        bool readOk = oldDb->forEachContractData([&](const cs::Bytes& k, const cs::Bytes& v) {
            cs::Bytes inNew;
            if (!newDb->getContractData(k, inNew) || inNew != v) {
                bad = hex(k);
                return false;
            }
            return true;
        });
        if (!readOk) return failed(r, "cannot read the contract records: " + oldDb->last_error_message());
        if (!bad.empty()) return failed(r, "contract record " + bad + " in the " + opt.from + " database is missing or differs in the converted one");
    }

    newDb.reset();
    oldDb.reset();
    st.moving = true;
    if (!writeState(opt.dbPath, st)) return failed(r, "cannot write db/.convert");
    return finish(opt, st, r, started);
}

}  // namespace csdb::convert
