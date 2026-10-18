# Block Database Conversion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A node finds an existing block database of another format than `[storage] db_backend`, never overwrites it, and converts it in place (batch by batch, newest block first from berkeleydb and oldest first from the others, verified, resumable) on confirmation.

**Architecture:** The conversion lives in csdb (`storage_convert.{hpp,cpp}`) and works on the `csdb::Database` interface the three backends already implement, plus one new method to enumerate contract data. Format detection and the `db/.backend` marker live in csdb too (`storage_backend.{hpp,cpp}`). The start-up decision (prompt, flags, exit codes) runs in `client/src/main.cpp` right after `Config::read`, before `cs::Peer` is built, through a small `client/src/dbcheck.{hpp,cpp}`; `csnode/` is not touched. Docker, the private-network runner and the upgrade test call the same node flags.

**Tech Stack:** C++17, BerkeleyDB 5 C++ API, LMDB, RocksDB, Boost.ProgramOptions, GoogleTest (`tests/csnode` → `nodetests`, run by `ctest` in `build.yml`), Bash, GitHub Actions.

**Spec:** `docs/specs/2026-10-18-block-db-conversion-design.md`

## Global Constraints

- Blocks, block hashes, consensus rules and the wire protocol do not change; a converted node stays compatible with the official 4.2.532 network.
- Backends: `berkeleydb`, `lmdb`, `rocksdb`; every direction supported.
- Marker: `db/.backend`, one line, one of the three names. The official node ignores it.
- Fingerprints: berkeleydb = `db/blockchain.db` and `db/sequence.db`; lmdb = `db/lmdb/data.mdb`; rocksdb = `db/CURRENT` and `db/MANIFEST-*`.
- Flags: `--convert-db` (convert without asking), `--convert-only` (convert, exit 0), `--convert-batch N` (default 100000).
- Exit code 3 when the node stops because of a format mismatch, a refused conversion, two formats in `db/`, or too little free space; nothing changed on disk in those cases.
- Prompt (stdin is a terminal only): `Convert the block database from X to Y now? [y/N]`.
- No-terminal message: `the block database is X, config.ini asks for Y: run the node with --convert-db to convert it, or set db_backend=X`.
- Order: newest block first from berkeleydb (its compaction gives space back from the end of the file), oldest first from lmdb and rocksdb (a high first put into an empty berkeleydb target creates every lower Recno record at once); a pruned database is not converted to berkeleydb. See the spec, section 2. Per batch: read → write new → flush → verify → record in `db/.convert` (temp file, fsync, rename) → remove from old → compact old. Removal only after the record.
- Contract data: copied and verified first, removed from the old database only at the end.
- Peak disk: old database + one batch + compaction work space; refuse (exit 3) before starting when free space is lower than the estimate.
- Docker: `CREDITS_CONVERT_DB=yes` makes the entrypoint pass `--convert-db`.
- Ship decision: the upgrade test reports the conversion time extrapolated to ~186 M blocks next to the network sync time; the conversion ships only if clearly faster.
- Repository rules: commits and comments in English; every node change a separate cherry-pickable commit; files keep their line endings (several `.cpp` are CRLF: edit with a CRLF-preserving tool); local builds with `nice -n 19` and `-j2`; heavy runs on GitHub Actions with `max-parallel` ≤ 5.

## Review Focus

1. **No block database at all** (first start: `db/` missing, or `db/` empty, or only a stale `db/.backend` left): the node must start exactly as today, with no prompt, no message and no new exit path, and create the configured database. Tests in Task 2 (`MissingFolderHasNoFormat`, `MarkerWithoutDataIsNoFormat`), Task 4 (`NoDatabaseStartsNormally`) and Task 6 (every scenario starts its nodes from an empty `db/`; the new check asserts they never printed a `block database:` line).
2. **Folder with both a BerkeleyDB and an LMDB database** (an operator switched `db_backend` once already and the node opened an empty LMDB): two formats with data must stop with the "found both" message, never pick one. Test in Task 2 (`DetectsBothFormats`) and Task 4 (`BothFormatsStop`).
3. **An empty database of the configured format next to a full one of another format** (same situation, the new one has zero blocks): the empty one must not count as "data", so the node offers to convert the full one. Test in Task 2 (`EmptyTargetDoesNotCount`).
4. **Pruned database whose first block is not 0**: the conversion covers `first..last` only and the hash-chain check does not ask for block `first-1`. Test in Task 3 (`ConvertsPrunedRange`).
5. **Ctrl-C / SIGTERM during a conversion**: the node must stop between batches with `db/.convert` consistent and resume next time. Test in Task 3 (`ResumesAfterEachStep`, the stop flag variant).
6. **Conversion back to berkeleydb so the official node can run again**: the official node must open the result. Test in Task 7 (official phase 3 runs on the folder converted back).

---

## File Structure

| File | Responsibility |
|---|---|
| `csdb/include/csdb/database.hpp` (modify) | new virtual `forEachContractData` |
| `csdb/src/database_berkeleydb.cpp`, `database_lmdb.cpp`, `database_rocksdb.cpp` + headers (modify) | implement it |
| `csdb/include/csdb/storage_backend.hpp`, `csdb/src/storage_backend.cpp` (create) | detect formats in a folder, read/write `db/.backend`, open a backend by name, list/remove a backend's files |
| `csdb/src/storage.cpp` (modify) | write the marker after a successful open |
| `csdb/include/csdb/storage_convert.hpp`, `csdb/src/storage_convert.cpp` (create) | the batched conversion, the state file, verification, resume, finalisation |
| `client/src/dbcheck.hpp`, `client/src/dbcheck.cpp` (create) | start-up decision table, prompt, free-space check, calls the converter |
| `client/src/main.cpp` (modify) | three new options, call `dbcheck` after `Config::read` |
| `client/CMakeLists.txt` (modify) | add `dbcheck.cpp` |
| `csdb/CMakeLists.txt` (modify) | add the two new sources |
| `tests/csnode/database_contracts_iter_tests.cpp`, `storage_backend_tests.cpp`, `storage_convert_tests.cpp`, `dbcheck_tests.cpp` (create) | unit tests (picked up by the `file(GLOB SRCS *.cpp)` of `tests/csnode/CMakeLists.txt`) |
| `tests/csnode/CMakeLists.txt` (modify) | add `client/src/dbcheck.cpp` to `SRCS` |
| `packaging/docker/entrypoint.sh`, `packaging/docker/test/test-entrypoint.sh`, `packaging/docker/.env.example`, `packaging/docker/README.md` (modify) | `CREDITS_CONVERT_DB` |
| `ci/testnet/run.sh`, `.github/workflows/robustness.yml` (modify) | `CONVERT_AT`, scenario `db-convert` |
| `.github/workflows/upgrade-compat.yml` (modify) | conversion phases and timing report |
| `packaging/README` of the release package (modify, `packaging/make-package.sh` heredoc) | operator notes |

---

### Task 1: Enumerate contract data in every backend

**Files:**
- Modify: `csdb/include/csdb/database.hpp` (after `getContractData`, ~line 57)
- Modify: `csdb/include/csdb/database_berkeleydb.hpp`, `csdb/src/database_berkeleydb.cpp`
- Modify: `csdb/include/csdb/database_lmdb.hpp`, `csdb/src/database_lmdb.cpp`
- Modify: `csdb/include/csdb/database_rocksdb.hpp`, `csdb/src/database_rocksdb.cpp`
- Test: `tests/csnode/database_contracts_iter_tests.cpp`

**Interfaces:**
- Produces: `virtual bool csdb::Database::forEachContractData(const std::function<bool(const cs::Bytes& key, const cs::Bytes& value)>& fn);` — calls `fn` for every contract record in key order of the engine; `fn` returning `false` stops early (still returns `true`); returns `false` on an engine error. Base implementation sets `NotSupported` and returns `false`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/csnode/database_contracts_iter_tests.cpp
#include "gtest/gtest.h"

#include <filesystem>
#include <map>
#include <memory>

#include <csdb/database_berkeleydb.hpp>
#include <csdb/database_lmdb.hpp>
#include <csdb/database_rocksdb.hpp>

namespace {
std::shared_ptr<csdb::Database> openBackend(const std::string& name, const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    if (name == "lmdb") {
        auto db = std::make_shared<csdb::DatabaseLmdb>();
        db->open((dir / "lmdb").string());
        return db;
    }
    if (name == "rocksdb") {
        auto db = std::make_shared<csdb::DatabaseRocksDB>();
        db->open(dir.string());
        return db;
    }
    auto db = std::make_shared<csdb::DatabaseBerkeleyDB>();
    db->open(dir.string());
    return db;
}

class ContractIter : public ::testing::TestWithParam<std::string> {};
}  // namespace

TEST_P(ContractIter, VisitsEveryRecordAndStopsEarly) {
    const auto dir = std::filesystem::temp_directory_path() / ("cs_contract_iter_" + GetParam());
    std::filesystem::remove_all(dir);
    {
        auto db = openBackend(GetParam(), dir);
        ASSERT_TRUE(db->is_open());
        std::map<cs::Bytes, cs::Bytes> written;
        for (uint8_t i = 0; i < 20; ++i) {
            cs::Bytes key(32, i), value(100 + i, static_cast<cs::Byte>(i * 3));
            ASSERT_TRUE(db->updateContractData(key, value));
            written[key] = value;
        }
        std::map<cs::Bytes, cs::Bytes> seen;
        ASSERT_TRUE(db->forEachContractData([&](const cs::Bytes& k, const cs::Bytes& v) {
            seen[k] = v;
            return true;
        }));
        EXPECT_EQ(seen, written);

        int calls = 0;
        ASSERT_TRUE(db->forEachContractData([&](const cs::Bytes&, const cs::Bytes&) { return ++calls < 5; }));
        EXPECT_EQ(calls, 5);
    }
    std::filesystem::remove_all(dir);
}

INSTANTIATE_TEST_SUITE_P(AllBackends, ContractIter, ::testing::Values("berkeleydb", "lmdb", "rocksdb"));
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd build && nice -n 19 ninja -j2 nodetests && ./tests/csnode/nodetests --gtest_filter='AllBackends/ContractIter.*'`
Expected: compile error, `forEachContractData` is not a member of `csdb::Database`.

- [ ] **Step 3: Implement**

`database.hpp`, after `getContractData` (add `#include <functional>` at the top):

```cpp
    // every contract record, in the engine's key order; fn returns false to stop early.
    // Used to convert a database to another backend.
    using ContractVisitor = std::function<bool(const cs::Bytes& key, const cs::Bytes& value)>;
    virtual bool forEachContractData(const ContractVisitor& fn) {
        (void)fn;
        set_last_error(NotSupported);
        return false;
    }
```

Each backend header: `bool forEachContractData(const ContractVisitor& fn) override;`

`database_lmdb.cpp` (uses the file's existing `Txn`, `toVal`, `assign`, `fail` helpers):

```cpp
bool DatabaseLmdb::forEachContractData(const ContractVisitor& fn) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, true);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_cursor* cursor = nullptr;
    int status = mdb_cursor_open(txn.get(), contracts_, &cursor);
    if (status != 0) {
        return fail(status, "mdb_cursor_open");
    }
    MDB_val k, v;
    cs::Bytes key, value;
    for (status = mdb_cursor_get(cursor, &k, &v, MDB_FIRST); status == 0; status = mdb_cursor_get(cursor, &k, &v, MDB_NEXT)) {
        assign(&key, k);
        assign(&value, v);
        if (!fn(key, value)) {
            status = MDB_NOTFOUND;
            break;
        }
    }
    mdb_cursor_close(cursor);
    if (status != MDB_NOTFOUND) {
        return fail(status, "forEachContractData");
    }
    set_last_error();
    return true;
}
```

`database_rocksdb.cpp`:

```cpp
bool DatabaseRocksDB::forEachContractData(const ContractVisitor& fn) {
    if (!db_ || !cf_contracts_) {
        set_last_error(NotOpen);
        return false;
    }
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), cf_contracts_));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const auto k = it->key();
        const auto v = it->value();
        if (!fn(cs::Bytes(k.data(), k.data() + k.size()), cs::Bytes(v.data(), v.data() + v.size()))) {
            break;
        }
    }
    if (!it->status().ok()) {
        set_last_error(UnknownError, "forEachContractData: %s", it->status().ToString().c_str());
        return false;
    }
    set_last_error();
    return true;
}
```

`database_berkeleydb.cpp` (cursor on `db_contracts_`, no transaction, as `getContractData` reads; `DB_DBT_MALLOC`-free `Dbt_safe` as the file already uses):

```cpp
bool DatabaseBerkeleyDB::forEachContractData(const ContractVisitor& fn) {
    if (!db_contracts_) {
        set_last_error(NotOpen);
        return false;
    }
    Dbc* cursor = nullptr;
    try {
        int status = db_contracts_->cursor(nullptr, &cursor, 0);
        if (status != 0) {
            set_last_error_from_berkeleydb(status);
            return false;
        }
        auto g = cs::scopeGuard([&]() { cursor->close(); });
        Dbt_safe k, v;
        while ((status = cursor->get(&k, &v, DB_NEXT)) == 0) {
            auto kb = static_cast<uint8_t*>(k.get_data());
            auto vb = static_cast<uint8_t*>(v.get_data());
            if (!fn(cs::Bytes(kb, kb + k.get_size()), cs::Bytes(vb, vb + v.get_size()))) {
                status = DB_NOTFOUND;
                break;
            }
        }
        if (status != DB_NOTFOUND) {
            set_last_error_from_berkeleydb(status);
            return false;
        }
    }
    catch (const DbException& e) {
        set_last_error_from_berkeleydb(e.get_errno());
        return false;
    }
    set_last_error();
    return true;
}
```

If `Dbt_safe` is declared only inside `database_berkeleydb.cpp` after this point, place the function below its definition.

- [ ] **Step 4: Run the test**

Run: `cd build && nice -n 19 ninja -j2 nodetests && ./tests/csnode/nodetests --gtest_filter='AllBackends/ContractIter.*'`
Expected: 3 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add csdb/include/csdb/database*.hpp csdb/src/database_*.cpp tests/csnode/database_contracts_iter_tests.cpp
git commit -m "csdb: enumerate the contract records of every block database backend"
```

---

### Task 2: Detect the format of a block database and keep a marker

**Files:**
- Create: `csdb/include/csdb/storage_backend.hpp`, `csdb/src/storage_backend.cpp`
- Modify: `csdb/CMakeLists.txt` (add `src/storage_backend.cpp` to the sources list next to `src/storage.cpp`)
- Modify: `csdb/src/storage.cpp` (in `Storage::open(path, callback, newBlockchainTop, startReadFrom, ..., dbBackend)` ~line 576: after a successful open, write the marker)
- Test: `tests/csnode/storage_backend_tests.cpp`

**Interfaces:**
- Produces (namespace `csdb::backend`):
  - `std::vector<std::string> detect(const std::string& dbPath);` — names of the formats whose files hold data in `dbPath`, sorted; empty = none.
  - `std::string readMarker(const std::string& dbPath);` — content of `dbPath/.backend` trimmed, `""` if absent or not one of the three names.
  - `bool writeMarker(const std::string& dbPath, const std::string& name);` — temp file + fsync + rename.
  - `std::shared_ptr<csdb::Database> open(const std::string& name, const std::string& dir);` — opens the backend whose files live in `dir` (lmdb in `dir + "/lmdb"`), `nullptr` if it fails.
  - `std::vector<std::filesystem::path> files(const std::string& name, const std::string& dir);` — the backend's own files in `dir` (used to move and delete them).
  - `bool isKnown(const std::string& name);`

Rules for `detect` ("holds data" avoids counting the empty database a node opened after a config change):
- berkeleydb: `blockchain.db` exists and `sequence.db` exists, and `blockchain.db` is larger than 64 KiB;
- lmdb: `lmdb/data.mdb` exists and is larger than 64 KiB and opening it finds at least one block (`new_iterator()->seek_to_first(); is_valid()`);
- rocksdb: `CURRENT` exists and some `MANIFEST-*` exists, and opening it finds at least one block.

Files per backend:
- berkeleydb: `blockchain.db`, `sequence.db`, `contracts.db`, `__db.*`, `log.*`;
- lmdb: the `lmdb` directory;
- rocksdb: `CURRENT`, `IDENTITY`, `LOCK`, `LOG`, `LOG.old.*`, `MANIFEST-*`, `OPTIONS-*`, `*.sst`, `*.log` (WAL, numeric names), `*.blob`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/csnode/storage_backend_tests.cpp
#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>

#include <csdb/storage_backend.hpp>

namespace fs = std::filesystem;

namespace {
fs::path freshDir(const std::string& name) {
    auto dir = fs::temp_directory_path() / ("cs_backend_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void fill(const std::string& backend, const fs::path& dir, uint32_t blocks) {
    auto db = csdb::backend::open(backend, dir.string());
    ASSERT_TRUE(db && db->is_open());
    for (uint32_t s = 0; s < blocks; ++s) {
        cs::Bytes key(32, 0);
        key[0] = static_cast<cs::Byte>(s);
        key[1] = static_cast<cs::Byte>(s >> 8);
        ASSERT_TRUE(db->put(key, s, cs::Bytes(4000, static_cast<cs::Byte>(s))));
    }
    db->flush();
}
}  // namespace

TEST(StorageBackend, DetectsEachFormat) {
    for (std::string b : {"berkeleydb", "lmdb", "rocksdb"}) {
        auto dir = freshDir("each_" + b);
        fill(b, dir, 100);
        EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{b}) << b;
        fs::remove_all(dir);
    }
}

TEST(StorageBackend, DetectsBothFormats) {
    auto dir = freshDir("both");
    fill("berkeleydb", dir, 100);
    fill("lmdb", dir, 100);
    EXPECT_EQ(csdb::backend::detect(dir.string()), (std::vector<std::string>{"berkeleydb", "lmdb"}));
    fs::remove_all(dir);
}

TEST(StorageBackend, EmptyTargetDoesNotCount) {
    auto dir = freshDir("empty_target");
    fill("berkeleydb", dir, 100);
    { auto lmdb = csdb::backend::open("lmdb", dir.string()); ASSERT_TRUE(lmdb); }  // opened, no blocks
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{"berkeleydb"});
    fs::remove_all(dir);
}

TEST(StorageBackend, EmptyFolderHasNoFormat) {
    auto dir = freshDir("empty");
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "");
    fs::remove_all(dir);
}

TEST(StorageBackend, MissingFolderHasNoFormat) {
    const auto dir = fs::temp_directory_path() / "cs_backend_missing";
    fs::remove_all(dir);
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());   // must not throw or create the folder
    EXPECT_FALSE(fs::exists(dir));
}

TEST(StorageBackend, MarkerWithoutDataIsNoFormat) {
    auto dir = freshDir("marker_only");
    ASSERT_TRUE(csdb::backend::writeMarker(dir.string(), "berkeleydb"));
    EXPECT_TRUE(csdb::backend::detect(dir.string()).empty());
    fs::remove_all(dir);
}

TEST(StorageBackend, MarkerRoundTripAndRejectsGarbage) {
    auto dir = freshDir("marker");
    ASSERT_TRUE(csdb::backend::writeMarker(dir.string(), "rocksdb"));
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "rocksdb");
    std::ofstream(dir / ".backend") << "leveldb\n";
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), "");
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd build && nice -n 19 ninja -j2 nodetests`
Expected: compile error, `csdb/storage_backend.hpp` not found.

- [ ] **Step 3: Implement**

```cpp
// csdb/include/csdb/storage_backend.hpp
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <csdb/database.hpp>

namespace csdb::backend {

inline const char* const kMarkerFile = ".backend";

bool isKnown(const std::string& name);
std::vector<std::string> detect(const std::string& dbPath);
std::string readMarker(const std::string& dbPath);
bool writeMarker(const std::string& dbPath, const std::string& name);
std::shared_ptr<Database> open(const std::string& name, const std::string& dir);
std::vector<std::filesystem::path> files(const std::string& name, const std::string& dir);

}  // namespace csdb::backend
```

```cpp
// csdb/src/storage_backend.cpp
#include <csdb/storage_backend.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

#include <csdb/database_berkeleydb.hpp>
#include <csdb/database_lmdb.hpp>
#include <csdb/database_rocksdb.hpp>

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
    if (bigger(dir / "blockchain.db") && fs::exists(dir / "sequence.db")) found.push_back("berkeleydb");
    if (bigger(dir / "lmdb" / "data.mdb") && hasBlocks("lmdb", dbPath)) found.push_back("lmdb");
    if (fs::exists(dir / "CURRENT") && anyMatching(dir, "MANIFEST-") && hasBlocks("rocksdb", dbPath)) found.push_back("rocksdb");
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
    if (name == "rocksdb") {
        auto db = std::make_shared<DatabaseRocksDB>();
        return db->open(dir) ? db : nullptr;
    }
    if (name == "berkeleydb") {
        auto db = std::make_shared<DatabaseBerkeleyDB>();
        return db->open(dir) ? db : nullptr;
    }
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
                || (ext == ".log" && std::all_of(f.begin(), f.end() - 4, ::isdigit));
        }
        if (mine) out.push_back(e.path());
    }
    return out;
}

}  // namespace csdb::backend
```

Check before writing: the return type of each backend's `open()` is `bool` (it is for LMDB, RocksDB and BerkeleyDB in the current tree). Under `#if defined(CSDB_USE_ROCKSDB)` / `CSDB_USE_BERKELEYDB` guard the includes and branches the same way `storage.cpp` does.

In `storage.cpp`, in the `Storage::open(... dbBackend)` overload, after each successful `open(opt, callback)` (the LMDB early return and the final return), write the marker. Restructure the LMDB branch so both paths share it:

```cpp
    // after the backend-specific open succeeded:
    if (ok) {
        backend::writeMarker(path, dbBackend == "rocks" ? "rocksdb" : dbBackend);
    }
    return ok;
```

with `#include <csdb/storage_backend.hpp>` at the top.

- [ ] **Step 4: Run**

Run: `cd build && nice -n 19 ninja -j2 nodetests && ./tests/csnode/nodetests --gtest_filter='StorageBackend.*'`
Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add csdb/include/csdb/storage_backend.hpp csdb/src/storage_backend.cpp csdb/src/storage.cpp csdb/CMakeLists.txt tests/csnode/storage_backend_tests.cpp
git commit -m "csdb: detect the format of a block database and record it in db/.backend"
```

---

### Task 3: Convert a block database, batch by batch, resumable

**Files:**
- Create: `csdb/include/csdb/storage_convert.hpp`, `csdb/src/storage_convert.cpp`
- Modify: `csdb/CMakeLists.txt`
- Test: `tests/csnode/storage_convert_tests.cpp`

**Interfaces:**
- Consumes: `csdb::backend::{open, files, writeMarker, readMarker}` (Task 2), `Database::forEachContractData` (Task 1), `csdb::Pool::from_binary`, `Pool::hash()`, `Pool::previous_hash()`, `Pool::sequence()`.
- Produces (namespace `csdb::convert`):

```cpp
enum class Step { Read, Written, Verified, Recorded, Removed };

struct Options {
    std::string dbPath;                 // the node's db/ folder
    std::string from;                   // backend now on disk
    std::string to;                     // configured backend
    uint32_t batch = 100000;
    std::function<void(Step, uint32_t lo, uint32_t hi)> hook;   // tests: throw to simulate a crash
    std::function<bool()> stopRequested;                        // checked between batches
    std::function<void(const std::string&)> log;
};

struct Result {
    bool ok = false;
    bool stopped = false;               // stopped between batches on request, resumable
    std::string error;
    uint64_t blocks = 0;
    uint64_t contracts = 0;
    double seconds = 0;
    uint64_t bytesBefore = 0, bytesAfter = 0;
};

struct State {                          // db/.convert
    std::string from, to;
    uint32_t first = 0, last = 0;
    int64_t doneDownTo = -1;            // lowest sequence already converted and removed; -1 = none yet
    std::string nextPrevHash;           // hex: previous_hash of block doneDownTo, to check the next batch's top block
    bool contractsCopied = false;
};

bool readState(const std::string& dbPath, State& out);    // false if absent or malformed
bool writeState(const std::string& dbPath, const State& s);
uint64_t estimatePeakBytes(const std::string& dbPath, const std::string& from, uint32_t batch);
Result run(const Options& opt);         // starts a new conversion or resumes db/.convert
```

Layout during the conversion: the new database is written under `db/.convert-new/` (for lmdb: `db/.convert-new/lmdb`), so files of two backends never share a folder name. At the end the old backend's files are deleted and the new files are moved (renamed) into `db/`.

`run` algorithm:

1. If `db/.convert` exists, `readState`; it must name the same `from`/`to` as `opt` (else error "an unfinished conversion from A to B is in db/.convert"). Otherwise open the old database, `first` = `seek_to_first().key()`, `last` = `seek_to_last().key()`, write a fresh `State`.
2. Open old (`backend::open(from, dbPath)`) and new (`backend::open(to, dbPath + "/.convert-new")`).
3. If `!contractsCopied`: copy every contract record with `updateContractData`, `flush()`, read each back with `getContractData` and compare; set `contractsCopied = true`, `writeState`.
4. `hi = doneDownTo < 0 ? last : doneDownTo - 1`. While `hi >= first` (as int64): `lo = max(first, hi - batch + 1)`;
   - read `seq` from `hi` down to `lo`: `old->get(seq, &bytes)` must succeed; `Pool::from_binary(cs::Bytes(bytes))` must be valid with `sequence() == seq`; `key = pool.hash().to_binary()`; `old->seq_no(key, &s) && s == seq` (proves the key is the one stored); hook(Read);
   - if `nextPrevHash` is set, the block `hi` hash must equal it; then for each block below, its hash must equal the `previous_hash()` of the block above; keep the lowest block's `previous_hash()` as the new `nextPrevHash`; a mismatch is an error;
   - `new->put(key, seq, bytes)` for each (use `put_batch` when available); `new->flush()`; hook(Written);
   - verify: `new->get(seq, &b2) && b2 == bytes` and `new->get(key, &b3) && b3 == bytes` for each; hook(Verified);
   - `doneDownTo = lo`, `nextPrevHash = hex`, `writeState`; hook(Recorded);
   - `old->remove_batch(keys)`; `old->compact()`; hook(Removed);
   - `hi = lo - 1`; if `stopRequested && stopRequested()` → close both, return `{stopped = true}`.
   - On resume when a batch was written but not recorded, the same batch is written again: `put` must overwrite (LMDB `mdb_put` without `MDB_NOOVERWRITE`, RocksDB `Put`, BerkeleyDB `put` without `DB_NOOVERWRITE`; check each, and if one refuses duplicates, `remove` the key in the new database first).
   - On resume when a batch was recorded but removal did not finish, `hi` starts below it, so step 4 first calls `old->remove_batch` for the keys of the recorded batch still present: before the loop, if `doneDownTo >= 0`, read `seq` in `[doneDownTo, min(last, doneDownTo + batch - 1)]` from the old database where present and remove them.
5. Final check on the new database: iterator `seek_to_first().key() == first`, `seek_to_last().key() == last`, walk forward with `next()` checking keys are consecutive and that each block's `previous_hash()` equals the hash of the one before.
6. Close both. Delete `backend::files(from, dbPath)`. Move every entry of `.convert-new/` into `dbPath` (`fs::rename`). Remove `.convert-new/`. `writeMarker(dbPath, to)`. Remove `db/.convert`. Fill `Result` (`bytesBefore` measured at step 1, `bytesAfter` at the end, `seconds` from a steady clock).

Every error returns `Result{ok=false, error=...}` with both databases closed and `db/.convert` describing the last good state.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/csnode/storage_convert_tests.cpp
#include "gtest/gtest.h"

#include <filesystem>
#include <stdexcept>

#include <csdb/pool.hpp>
#include <csdb/storage_backend.hpp>
#include <csdb/storage_convert.hpp>

namespace fs = std::filesystem;

namespace {
struct Chain {
    std::vector<cs::Bytes> blocks;   // index = sequence - first
    std::vector<cs::Bytes> hashes;
    uint32_t first = 0;
};

// a real hash-linked chain: the converter parses pools and checks previous hashes
Chain makeChain(uint32_t first, uint32_t count) {
    Chain c;
    c.first = first;
    csdb::PoolHash prev;
    for (uint32_t i = 0; i < count; ++i) {
        csdb::Pool p;
        p.set_sequence(first + i);
        p.set_previous_hash(prev);
        p.add_user_field(1, std::string(200, static_cast<char>('a' + i % 26)));
        p.compose();
        c.blocks.push_back(p.to_binary());
        c.hashes.push_back(p.hash().to_binary());
        prev = p.hash();
    }
    return c;
}

fs::path writeDb(const std::string& backend, const std::string& name, const Chain& c) {
    auto dir = fs::temp_directory_path() / ("cs_convert_" + name);
    fs::remove_all(dir);
    auto db = csdb::backend::open(backend, dir.string());
    EXPECT_TRUE(db);
    for (size_t i = 0; i < c.blocks.size(); ++i) {
        EXPECT_TRUE(db->put(c.hashes[i], static_cast<uint32_t>(c.first + i), c.blocks[i]));
    }
    for (uint8_t i = 0; i < 5; ++i) {
        EXPECT_TRUE(db->updateContractData(cs::Bytes(32, i), cs::Bytes(50, i)));
    }
    db->flush();
    return dir;
}

void expectSame(const std::string& backend, const fs::path& dir, const Chain& c) {
    EXPECT_EQ(csdb::backend::detect(dir.string()), std::vector<std::string>{backend});
    EXPECT_EQ(csdb::backend::readMarker(dir.string()), backend);
    EXPECT_FALSE(fs::exists(dir / ".convert"));
    EXPECT_FALSE(fs::exists(dir / ".convert-new"));
    auto db = csdb::backend::open(backend, dir.string());
    ASSERT_TRUE(db);
    for (size_t i = 0; i < c.blocks.size(); ++i) {
        cs::Bytes v;
        ASSERT_TRUE(db->get(static_cast<uint32_t>(c.first + i), &v)) << i;
        EXPECT_EQ(v, c.blocks[i]);
        ASSERT_TRUE(db->get(c.hashes[i], &v));
        EXPECT_EQ(v, c.blocks[i]);
    }
    for (uint8_t i = 0; i < 5; ++i) {
        cs::Bytes v;
        ASSERT_TRUE(db->getContractData(cs::Bytes(32, i), v));
        EXPECT_EQ(v, cs::Bytes(50, i));
    }
}

csdb::convert::Options opts(const fs::path& dir, std::string from, std::string to, uint32_t batch = 37) {
    csdb::convert::Options o;
    o.dbPath = dir.string();
    o.from = std::move(from);
    o.to = std::move(to);
    o.batch = batch;
    return o;
}

class ConvertPair : public ::testing::TestWithParam<std::pair<std::string, std::string>> {};
}  // namespace

TEST_P(ConvertPair, ConvertsEveryBlockAndContract) {
    const auto [from, to] = GetParam();
    const auto chain = makeChain(0, 500);
    const auto dir = writeDb(from, from + "_" + to, chain);
    auto r = csdb::convert::run(opts(dir, from, to));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.blocks, 500u);
    EXPECT_EQ(r.contracts, 5u);
    expectSame(to, dir, chain);
    fs::remove_all(dir);
}

INSTANTIATE_TEST_SUITE_P(AllDirections, ConvertPair, ::testing::Values(
    std::make_pair(std::string("berkeleydb"), std::string("lmdb")),
    std::make_pair(std::string("berkeleydb"), std::string("rocksdb")),
    std::make_pair(std::string("lmdb"), std::string("berkeleydb")),
    std::make_pair(std::string("lmdb"), std::string("rocksdb")),
    std::make_pair(std::string("rocksdb"), std::string("berkeleydb")),
    std::make_pair(std::string("rocksdb"), std::string("lmdb"))));

TEST(Convert, ConvertsPrunedRange) {
    const auto chain = makeChain(1000, 300);
    const auto dir = writeDb("lmdb", "pruned", chain);
    auto r = csdb::convert::run(opts(dir, "lmdb", "berkeleydb"));
    ASSERT_TRUE(r.ok) << r.error;
    expectSame("berkeleydb", dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, ResumesAfterEachStep) {
    using csdb::convert::Step;
    for (Step crashAt : {Step::Read, Step::Written, Step::Verified, Step::Recorded, Step::Removed}) {
        const auto chain = makeChain(0, 400);
        const auto dir = writeDb("berkeleydb", "resume", chain);
        auto o = opts(dir, "berkeleydb", "lmdb");
        int batches = 0;
        o.hook = [&](Step s, uint32_t, uint32_t) {
            if (s == crashAt && ++batches == 3) throw std::runtime_error("simulated crash");
        };
        EXPECT_THROW(csdb::convert::run(o), std::runtime_error);
        ASSERT_TRUE(fs::exists(dir / ".convert"));
        o.hook = nullptr;
        auto r = csdb::convert::run(o);
        ASSERT_TRUE(r.ok) << r.error << " crash at step " << static_cast<int>(crashAt);
        expectSame("lmdb", dir, chain);
        fs::remove_all(dir);
    }
}

TEST(Convert, StopsBetweenBatchesAndResumes) {
    const auto chain = makeChain(0, 400);
    const auto dir = writeDb("rocksdb", "stop", chain);
    auto o = opts(dir, "rocksdb", "lmdb");
    int asked = 0;
    o.stopRequested = [&] { return ++asked == 2; };
    auto r = csdb::convert::run(o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.stopped);
    o.stopRequested = nullptr;
    r = csdb::convert::run(o);
    ASSERT_TRUE(r.ok) << r.error;
    expectSame("lmdb", dir, chain);
    fs::remove_all(dir);
}

TEST(Convert, BrokenChainStopsWithNothingRemoved) {
    auto chain = makeChain(0, 200);
    chain.blocks[150] = makeChain(150, 1).blocks[0];        // a block that does not link to 149
    chain.hashes[150] = makeChain(150, 1).hashes[0];
    const auto dir = writeDb("lmdb", "broken", chain);
    auto r = csdb::convert::run(opts(dir, "lmdb", "rocksdb", 1000));  // one batch: nothing may be removed
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("150"), std::string::npos) << r.error;
    auto db = csdb::backend::open("lmdb", dir.string());
    cs::Bytes v;
    for (uint32_t s = 0; s < 200; ++s) ASSERT_TRUE(db->get(s, &v)) << s;
    fs::remove_all(dir);
}

TEST(Convert, RefusesAStateFileForAnotherConversion) {
    const auto chain = makeChain(0, 50);
    const auto dir = writeDb("berkeleydb", "other_state", chain);
    csdb::convert::State s;
    s.from = "rocksdb"; s.to = "lmdb"; s.first = 0; s.last = 49;
    ASSERT_TRUE(csdb::convert::writeState(dir.string(), s));
    auto r = csdb::convert::run(opts(dir, "berkeleydb", "lmdb"));
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("unfinished conversion from rocksdb to lmdb"), std::string::npos) << r.error;
    fs::remove_all(dir);
}
```

If `Pool::add_user_field` has another signature in this tree, use any field setter that changes the serialized bytes; the test only needs distinct, hash-linked pools.

- [ ] **Step 2: Run to verify they fail**

Run: `cd build && nice -n 19 ninja -j2 nodetests`
Expected: compile error, `csdb/storage_convert.hpp` not found.

- [ ] **Step 3: Implement `storage_convert.hpp`** with exactly the declarations of the Interfaces block above (namespace `csdb::convert`, includes `<cstdint> <functional> <string>`).

- [ ] **Step 4: Implement `storage_convert.cpp`**

```cpp
#include <csdb/storage_convert.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    for (auto it = fs::recursive_directory_iterator(p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) n += it->file_size(ec);
    }
    return n;
}

bool syncWrite(const fs::path& path, const std::string& text) {
    const auto tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

struct Block {
    uint32_t seq;
    cs::Bytes key, bytes;
    std::string prevHex;
};

void say(const Options& o, const std::string& m) {
    if (o.log) o.log(m);
}
}  // namespace

bool readState(const std::string& dbPath, State& s) {
    std::ifstream f(fs::path(dbPath) / kStateFile);
    if (!f) return false;
    std::string line;
    int seen = 0;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto k = line.substr(0, eq), v = line.substr(eq + 1);
        try {
            if (k == "from") { s.from = v; ++seen; }
            else if (k == "to") { s.to = v; ++seen; }
            else if (k == "first") { s.first = static_cast<uint32_t>(std::stoul(v)); ++seen; }
            else if (k == "last") { s.last = static_cast<uint32_t>(std::stoul(v)); ++seen; }
            else if (k == "done_down_to") { s.doneDownTo = std::stoll(v); ++seen; }
            else if (k == "next_prev_hash") { s.nextPrevHash = v; }
            else if (k == "contracts") { s.contractsCopied = v == "copied"; }
        }
        catch (const std::exception&) {
            return false;
        }
    }
    return seen == 5 && backend::isKnown(s.from) && backend::isKnown(s.to) && s.first <= s.last;
}

bool writeState(const std::string& dbPath, const State& s) {
    std::ostringstream o;
    o << "from=" << s.from << "\nto=" << s.to << "\nfirst=" << s.first << "\nlast=" << s.last
      << "\ndone_down_to=" << s.doneDownTo << "\nnext_prev_hash=" << s.nextPrevHash
      << "\ncontracts=" << (s.contractsCopied ? "copied" : "pending") << "\n";
    return syncWrite(fs::path(dbPath) / kStateFile, o.str());
}

uint64_t estimatePeakBytes(const std::string& dbPath, const std::string& from, uint32_t batch) {
    uint64_t bytes = 0;
    for (const auto& p : backend::files(from, dbPath)) bytes += fs::is_directory(p) ? folderBytes(p) : fs::file_size(p);
    auto db = backend::open(from, dbPath);
    if (!db) return bytes;
    auto it = db->new_iterator();
    it->seek_to_first();
    if (!it->is_valid()) return 0;
    const uint64_t first = it->key();
    it->seek_to_last();
    const uint64_t blocks = it->key() - first + 1;
    const uint64_t perBlock = bytes / std::max<uint64_t>(blocks, 1);
    // one batch in the new format, plus the same again for compaction and the engines' logs
    return 2 * perBlock * std::min<uint64_t>(batch, blocks) + (256ULL << 20);
}

Result run(const Options& opt) {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    Result r;
    const fs::path dir(opt.dbPath);
    const std::string newDir = (dir / kNewDir).string();

    State st;
    if (fs::exists(dir / kStateFile)) {
        if (!readState(opt.dbPath, st)) { r.error = "db/.convert is unreadable"; return r; }
        if (st.from != opt.from || st.to != opt.to) {
            r.error = "an unfinished conversion from " + st.from + " to " + st.to + " is in db/.convert";
            return r;
        }
        say(opt, "resuming the conversion from " + st.from + " to " + st.to + " below block " + std::to_string(st.doneDownTo));
    }

    auto oldDb = backend::open(opt.from, opt.dbPath);
    if (!oldDb) { r.error = "cannot open the " + opt.from + " database"; return r; }
    if (!fs::exists(dir / kStateFile)) {
        auto it = oldDb->new_iterator();
        it->seek_to_first();
        if (!it->is_valid()) { r.error = "the " + opt.from + " database holds no block"; return r; }
        st.from = opt.from; st.to = opt.to;
        st.first = it->key();
        it->seek_to_last();
        st.last = it->key();
        if (!writeState(opt.dbPath, st)) { r.error = "cannot write db/.convert"; return r; }
        say(opt, "converting blocks " + std::to_string(st.first) + ".." + std::to_string(st.last) + " from " + st.from + " to " + st.to);
    }
    r.bytesBefore = folderBytes(dir);

    auto newDb = backend::open(opt.to, newDir);
    if (!newDb) { r.error = "cannot create the " + opt.to + " database in db/.convert-new"; return r; }

    if (!st.contractsCopied) {
        std::vector<std::pair<cs::Bytes, cs::Bytes>> records;
        if (!oldDb->forEachContractData([&](const cs::Bytes& k, const cs::Bytes& v) { records.emplace_back(k, v); return true; })) {
            r.error = "cannot read the contract records: " + oldDb->last_error_message();
            return r;
        }
        for (const auto& [k, v] : records) {
            if (!newDb->updateContractData(k, v)) { r.error = "cannot write a contract record"; return r; }
        }
        newDb->flush();
        for (const auto& [k, v] : records) {
            cs::Bytes back;
            if (!newDb->getContractData(k, back) || back != v) { r.error = "a contract record differs after copying"; return r; }
        }
        st.contractsCopied = true;
        if (!writeState(opt.dbPath, st)) { r.error = "cannot write db/.convert"; return r; }
    }
    {
        uint64_t n = 0;
        newDb->forEachContractData([&](const cs::Bytes&, const cs::Bytes&) { ++n; return true; });
        r.contracts = n;
    }

    // a batch recorded before a crash may still be partly in the old database
    if (st.doneDownTo >= 0) {
        std::vector<cs::Bytes> leftovers;
        const int64_t top = std::min<int64_t>(st.last, st.doneDownTo + opt.batch - 1);
        for (int64_t s = st.doneDownTo; s <= top; ++s) {
            cs::Bytes b;
            if (oldDb->get(static_cast<uint32_t>(s), &b)) {
                auto p = csdb::Pool::from_binary(std::move(b));
                if (p.is_valid()) leftovers.push_back(p.hash().to_binary());
            }
        }
        if (!leftovers.empty() && !oldDb->remove_batch(leftovers)) { r.error = "cannot remove converted blocks"; return r; }
    }

    int64_t hi = st.doneDownTo < 0 ? int64_t(st.last) : st.doneDownTo - 1;
    while (hi >= int64_t(st.first)) {
        const int64_t lo = std::max<int64_t>(st.first, hi - opt.batch + 1);
        std::vector<Block> blocks;
        blocks.reserve(static_cast<size_t>(hi - lo + 1));
        std::string expected = st.nextPrevHash;
        for (int64_t s = hi; s >= lo; --s) {
            Block b{static_cast<uint32_t>(s), {}, {}, {}};
            if (!oldDb->get(b.seq, &b.bytes)) { r.error = "block " + std::to_string(s) + " is missing in the " + opt.from + " database"; return r; }
            auto pool = csdb::Pool::from_binary(cs::Bytes(b.bytes));
            if (!pool.is_valid() || pool.sequence() != b.seq) { r.error = "block " + std::to_string(s) + " does not parse"; return r; }
            b.key = pool.hash().to_binary();
            uint32_t stored = 0;
            if (!oldDb->seq_no(b.key, &stored) || stored != b.seq) { r.error = "block " + std::to_string(s) + " is stored under another hash"; return r; }
            if (!expected.empty() && hex(b.key) != expected) { r.error = "block " + std::to_string(s) + " is not the previous block of block " + std::to_string(s + 1); return r; }
            b.prevHex = hex(pool.previous_hash().to_binary());
            expected = b.prevHex;
            blocks.push_back(std::move(b));
        }
        if (opt.hook) opt.hook(Step::Read, uint32_t(lo), uint32_t(hi));

        for (const auto& b : blocks) {
            if (!newDb->put(b.key, b.seq, b.bytes)) { r.error = "cannot write block " + std::to_string(b.seq) + ": " + newDb->last_error_message(); return r; }
        }
        if (!newDb->flush()) { r.error = "cannot flush the " + opt.to + " database"; return r; }
        if (opt.hook) opt.hook(Step::Written, uint32_t(lo), uint32_t(hi));

        for (const auto& b : blocks) {
            cs::Bytes bySeq, byKey;
            if (!newDb->get(b.seq, &bySeq) || bySeq != b.bytes || !newDb->get(b.key, &byKey) || byKey != b.bytes) {
                r.error = "block " + std::to_string(b.seq) + " differs after copying";
                return r;
            }
        }
        if (opt.hook) opt.hook(Step::Verified, uint32_t(lo), uint32_t(hi));

        st.doneDownTo = lo;
        st.nextPrevHash = blocks.back().prevHex;
        if (!writeState(opt.dbPath, st)) { r.error = "cannot write db/.convert"; return r; }
        if (opt.hook) opt.hook(Step::Recorded, uint32_t(lo), uint32_t(hi));

        std::vector<cs::Bytes> keys;
        keys.reserve(blocks.size());
        for (const auto& b : blocks) keys.push_back(b.key);
        if (!oldDb->remove_batch(keys)) { r.error = "cannot remove converted blocks from the " + opt.from + " database"; return r; }
        oldDb->compact();
        if (opt.hook) opt.hook(Step::Removed, uint32_t(lo), uint32_t(hi));

        say(opt, "converted blocks " + std::to_string(lo) + ".." + std::to_string(st.last));
        hi = lo - 1;
        if (hi >= int64_t(st.first) && opt.stopRequested && opt.stopRequested()) {
            r.stopped = true;
            r.error = "stopped on request, the conversion resumes at the next start";
            return r;
        }
    }

    // whole new database: the recorded range, every block, an unbroken hash chain
    {
        auto it = newDb->new_iterator();
        it->seek_to_first();
        if (!it->is_valid() || it->key() != st.first) { r.error = "the converted database does not start at block " + std::to_string(st.first); return r; }
        std::string prevHash;
        uint64_t count = 0;
        for (uint32_t expectSeq = st.first; it->is_valid(); it->next(), ++expectSeq, ++count) {
            if (it->key() != expectSeq) { r.error = "block " + std::to_string(expectSeq) + " is missing after the conversion"; return r; }
            auto pool = csdb::Pool::from_binary(it->value());
            if (!prevHash.empty() && hex(pool.previous_hash().to_binary()) != prevHash) { r.error = "hash chain broken at block " + std::to_string(expectSeq); return r; }
            prevHash = hex(pool.hash().to_binary());
        }
        if (count != uint64_t(st.last) - st.first + 1) { r.error = "the converted database has " + std::to_string(count) + " blocks"; return r; }
        r.blocks = count;
    }

    newDb.reset();
    oldDb.reset();
    std::error_code ec;
    for (const auto& p : backend::files(opt.from, opt.dbPath)) fs::remove_all(p, ec);
    for (const auto& e : fs::directory_iterator(newDir)) {
        fs::rename(e.path(), dir / e.path().filename(), ec);
        if (ec) { r.error = "cannot move " + e.path().string() + ": " + ec.message(); return r; }
    }
    fs::remove_all(newDir, ec);
    backend::writeMarker(opt.dbPath, opt.to);
    fs::remove(dir / kStateFile, ec);

    r.bytesAfter = folderBytes(dir);
    r.seconds = std::chrono::duration<double>(clock::now() - started).count();
    r.ok = true;
    say(opt, "conversion done: " + std::to_string(r.blocks) + " blocks, " + std::to_string(r.contracts) + " contract records, "
             + std::to_string(r.bytesBefore >> 20) + " MB -> " + std::to_string(r.bytesAfter >> 20) + " MB in "
             + std::to_string(static_cast<int>(r.seconds)) + " s");
    return r;
}

}  // namespace csdb::convert
```

Notes for the implementer:
- `RocksDB` writes asynchronously only through `Storage`; `DatabaseRocksDB::put` used here is synchronous, and its `flush()` syncs the WAL.
- If the berkeleydb backend's `put` refuses an existing key (check `db_blocks_->put` flags in `DatabaseBerkeleyDB::put`), remove the key before writing it again in the resume path.
- `Database::last_error_message()` exists on the base class (used by `storage.cpp`).

- [ ] **Step 5: Run the tests**

Run: `cd build && nice -n 19 ninja -j2 nodetests && ./tests/csnode/nodetests --gtest_filter='AllDirections/ConvertPair.*:Convert.*'`
Expected: 6 + 5 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add csdb/include/csdb/storage_convert.hpp csdb/src/storage_convert.cpp csdb/CMakeLists.txt tests/csnode/storage_convert_tests.cpp
git commit -m "csdb: convert a block database to another backend, batch by batch, resumable"
```

---

### Task 4: Start-up decision, prompt and flags

**Files:**
- Create: `client/src/dbcheck.hpp`, `client/src/dbcheck.cpp`
- Modify: `client/src/main.cpp` (options block ~line 50; call after `Config::read` ~line 180)
- Modify: `client/CMakeLists.txt` (add `src/dbcheck.cpp`)
- Modify: `tests/csnode/CMakeLists.txt` (append `"${CMAKE_CURRENT_SOURCE_DIR}/../../client/src/dbcheck.cpp"` to `SRCS`)
- Test: `tests/csnode/dbcheck_tests.cpp`

**Interfaces:**
- Consumes: `csdb::backend::{detect, readMarker}`, `csdb::convert::{run, estimatePeakBytes, readState, State, Options, Result}`.
- Produces:

```cpp
namespace dbcheck {
enum class Action { Open, Convert, Ask, StopMismatch, StopBoth, Resume };
struct Facts {
    std::vector<std::string> found;   // csdb::backend::detect
    std::string configured;           // Config storage settings dbBackend
    bool convertStatePresent = false; // db/.convert exists
    bool convertFlag = false;         // --convert-db or --convert-only
    bool terminal = false;            // isatty(STDIN_FILENO)
};
Action decide(const Facts& f);
// runs the decision; returns -1 to continue the normal start, otherwise the process exit code
int check(const std::string& dbPath, const std::string& configured, bool convertFlag, bool convertOnly, uint32_t batch);
}
```

Decision table (`decide`):

| condition (first match) | Action |
|---|---|
| `convertStatePresent` | Resume |
| `found.size() > 1` | StopBoth |
| `found.empty()` or `found[0] == configured` | Open |
| `convertFlag` | Convert |
| `terminal` | Ask |
| otherwise | StopMismatch |

`check`:
- Open → if `--convert-only`, print "nothing to convert: the block database is already <configured>" and return 0; else return -1.
- StopBoth → print `found both X and Y in <dbPath>, remove the one you do not want` and return 3.
- StopMismatch → print the no-terminal message of the Global Constraints and return 3.
- Ask → print found, configured, database size (sum of `csdb::backend::files`), estimated peak, free space (`std::filesystem::space(dbPath).available`); read a line from stdin; `y`/`Y`/`yes` → Convert, anything else → print "nothing changed" and return 3.
- Convert / Resume → if free space < `estimatePeakBytes`, print both numbers in MB and return 3; else install a SIGINT/SIGTERM handler that sets an `std::atomic<bool>` used as `stopRequested`, call `csdb::convert::run` with `log` printing to `std::cout` (and `cslog()`), restore the handlers; on `stopped` return 0 with "conversion paused, it resumes at the next start"; on error print it and return 3; on success return 0 if `--convert-only`, else -1. Resume uses `from`/`to` from `readState`, and refuses (3) when the state's `to` differs from `configured` ("db/.convert converts to X, config.ini asks for Y").

- [ ] **Step 1: Write the failing test**

```cpp
// tests/csnode/dbcheck_tests.cpp
#include "gtest/gtest.h"

#include "../../client/src/dbcheck.hpp"

using dbcheck::Action;
using dbcheck::Facts;

TEST(DbCheck, DecisionTable) {
    EXPECT_EQ(dbcheck::decide(Facts{{}, "lmdb"}), Action::Open);
    EXPECT_EQ(dbcheck::decide(Facts{{"lmdb"}, "lmdb"}), Action::Open);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, false, false}), Action::StopMismatch);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, false, true}), Action::Ask);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", false, true, false}), Action::Convert);
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb"}, "lmdb", true, false, false}), Action::Resume);
}

TEST(DbCheck, BothFormatsStop) {
    EXPECT_EQ(dbcheck::decide(Facts{{"berkeleydb", "lmdb"}, "lmdb", false, true, true}), Action::StopBoth);
}

TEST(DbCheck, NoDatabaseStartsNormally) {
    namespace fs = std::filesystem;
    const auto missing = fs::temp_directory_path() / "cs_dbcheck_missing";
    fs::remove_all(missing);
    for (std::string b : {"berkeleydb", "lmdb", "rocksdb"}) {
        EXPECT_EQ(dbcheck::check(missing.string(), b, false, false, 100000), -1) << b;  // first start
        EXPECT_EQ(dbcheck::check(missing.string(), b, true, false, 100000), -1) << b;   // --convert-db with nothing to convert
    }
    EXPECT_FALSE(fs::exists(missing));
    const auto empty = fs::temp_directory_path() / "cs_dbcheck_empty_dir";
    fs::remove_all(empty);
    fs::create_directories(empty);
    EXPECT_EQ(dbcheck::check(empty.string(), "lmdb", false, false, 100000), -1);
    fs::remove_all(empty);
}

TEST(DbCheck, ConvertOnlyOnMatchingDatabaseExitsZero) {
    const auto dir = std::filesystem::temp_directory_path() / "cs_dbcheck_empty";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", true, true, 100000), 0);
    EXPECT_EQ(dbcheck::check(dir.string(), "lmdb", false, false, 100000), -1);
    std::filesystem::remove_all(dir);
}
```

(`dbcheck.hpp` must include `<filesystem> <string> <vector> <cstdint>`.)

- [ ] **Step 2: Run to verify it fails**

Run: `cd build && nice -n 19 ninja -j2 nodetests`
Expected: compile error, `dbcheck.hpp` not found.

- [ ] **Step 3: Implement `dbcheck.hpp` / `dbcheck.cpp`** following the table and the `check` description above. Use `std::cout` for every message (the logger may not be configured yet at this point of `main`), each line prefixed with `block database: `.

- [ ] **Step 4: Wire it into `main.cpp`**

Options (after `"db-path"`):

```cpp
        ("convert-db", "convert an existing block database to the configured db_backend without asking")
        ("convert-only", "convert the block database to the configured db_backend, then exit")
        ("convert-batch", po::value<uint32_t>()->default_value(100000), "blocks per conversion batch")
```

After `auto config = Config::read(vm, true); if (!config.isGood()) { panic(); }`:

```cpp
    {
        const bool convertOnly = vm.count("convert-only") > 0;
        const int rc = dbcheck::check(config.getPathToDB(), config.getStorageSettings().dbBackend,
                                      convertOnly || vm.count("convert-db") > 0, convertOnly,
                                      vm["convert-batch"].as<uint32_t>());
        if (rc >= 0) {
            return rc;
        }
    }
```

Check the accessor names on `Config` (`getPathToDB()` is used by `node.cpp`; `getStorageSettings()` by `blockchain.cpp`) and include `"dbcheck.hpp"`.

- [ ] **Step 5: Run the tests and a manual smoke test**

Run: `cd build && nice -n 19 ninja -j2 nodetests node && ./tests/csnode/nodetests --gtest_filter='DbCheck.*'`
Expected: 4 tests PASS.

Smoke (no network needed, uses the unit-test chain writer through a tiny folder):

```bash
mkdir -p /tmp/cs-smoke && cd /tmp/cs-smoke
printf '[storage]\ndb_backend=lmdb\n' > config.ini
# reuse a BerkeleyDB folder from a private-network run, or skip this step locally
../build/client/node --db-path db --config-file config.ini </dev/null; echo "exit $?"
```

Expected on a BerkeleyDB `db/`: the no-terminal message and `exit 3`.

- [ ] **Step 6: Commit**

```bash
git add client/src/dbcheck.hpp client/src/dbcheck.cpp client/src/main.cpp client/CMakeLists.txt tests/csnode/CMakeLists.txt tests/csnode/dbcheck_tests.cpp
git commit -m "client: recognise a block database of another backend and offer to convert it"
```

---

### Task 5: Docker

**Files:**
- Modify: `packaging/docker/entrypoint.sh` (block at ~line 97–118)
- Modify: `packaging/docker/test/test-entrypoint.sh`
- Modify: `packaging/docker/.env.example`, `packaging/docker/README.md`

**Interfaces:**
- Consumes: node flag `--convert-db`, exit code 3.
- Produces: env `CREDITS_CONVERT_DB` (`yes` to convert; anything else = no).

- [ ] **Step 1: Write the failing entrypoint tests** (follow the existing case style of `test-entrypoint.sh`: a fake `node` script that records its arguments)

```bash
# case: backend mismatch without CREDITS_CONVERT_DB refuses with the hint
setup_case mismatch_refuses
echo "mode=full backend=berkeleydb" > "$DATA/.storage"; touch "$DATA/db/blockchain.db"
write_env CREDITS_BACKEND=lmdb
run_entrypoint; expect_exit 3; expect_log "CREDITS_CONVERT_DB=yes"

# case: with CREDITS_CONVERT_DB=yes the node gets --convert-db and .storage follows the config
setup_case mismatch_converts
echo "mode=full backend=berkeleydb" > "$DATA/.storage"; touch "$DATA/db/blockchain.db"
write_env CREDITS_BACKEND=lmdb CREDITS_CONVERT_DB=yes
run_entrypoint; expect_exit 0; expect_args "--convert-db"; expect_file_content "$DATA/.storage" "mode=full backend=lmdb"
```

Use the helper names the file already defines; if a helper is missing (for example `expect_args`), add it next to the existing ones.

- [ ] **Step 2: Run to verify they fail**

Run: `bash packaging/docker/test/test-entrypoint.sh`
Expected: the two new cases FAIL (the refusal message has no hint; no `--convert-db`).

- [ ] **Step 3: Implement**

Replace the backend-mismatch refusal:

```bash
        if [ "$rec_backend" != "$cfg_backend" ]; then
            if [ "${CREDITS_CONVERT_DB:-}" = yes ]; then
                log "converting the block database from $rec_backend to $cfg_backend (CREDITS_CONVERT_DB=yes)"
                CONVERT_ARGS=(--convert-db)
            else
                refuse "the data was written by $rec_backend, config.ini asks for $cfg_backend: set CREDITS_CONVERT_DB=yes to convert it, or set CREDITS_BACKEND=$rec_backend" 3
            fi
        fi
```

Declare `CONVERT_ARGS=()` near the top, and add `"${CONVERT_ARGS[@]}"` to the final `exec "$OPT/node" ...` line. The `.storage` write after the block already records the configured backend. `.env.example`: `# CREDITS_CONVERT_DB=yes   convert an existing block database to CREDITS_BACKEND instead of refusing to start`. README: one paragraph "Changing the block database" (convert in place, needs about 1 GB free beyond the database, resumes if interrupted).

- [ ] **Step 4: Run**

Run: `bash packaging/docker/test/test-entrypoint.sh`
Expected: all cases PASS (12 existing + 2 new).

- [ ] **Step 5: Commit**

```bash
git add packaging/docker
git commit -m "docker: convert the block database on a backend change with CREDITS_CONVERT_DB=yes"
```

---

### Task 6: Private-network scenario `db-convert`

**Files:**
- Modify: `ci/testnet/run.sh` (option parsing near `STORAGE_SWITCH_AT` ~line 98; main loop near the storage switch ~line 540; final checks near ~line 698; header comment ~line 45)
- Modify: `.github/workflows/robustness.yml` (matrix, next to `storage-switch`)

**Interfaces:**
- Consumes: node flags `--convert-only`, `--convert-db`; the runner's `start_node`, `PIDS`, `WORK`, `seq_of`.
- Produces: env `CONVERT_AT` (sequence, 0 = off). At `CONVERT_AT`: n4 (berkeleydb) is stopped, its `config.ini` gets `db_backend=lmdb`, `node --convert-only` runs to completion, then n4 starts again. n6 (berkeleydb) gets `db_backend=rocksdb`, is started with `--convert-db`, killed with SIGKILL as soon as its log shows the second "converted blocks" line, then started again with `--convert-db` (must resume).

- [ ] **Step 1: Add the option and the header comment**

```bash
#        CONVERT_AT (default 0 = off, needs DB_BACKEND=berkeleydb): at that sequence n4 is converted to
#        lmdb with --convert-only and restarted, and n6 is converted to rocksdb with --convert-db, killed
#        in the middle of the conversion and started again: it must resume and finish
CONVERT_AT=${CONVERT_AT:-0}
```

- [ ] **Step 2: Add the action to the main loop** (after the storage-switch block)

```bash
    if [ "$CONVERT_AT" -gt 0 ] && [ "${converted:-0}" -eq 0 ] && [ "$min" -ge "$CONVERT_AT" ]; then
        converted=1
        for i in 4 6; do kill -TERM "${PIDS[$i]}"; done
        for i in 4 6; do wait "${PIDS[$i]}" 2>/dev/null || true; done
        sed -i 's/^db_backend=.*/db_backend=lmdb/' "$WORK/n4/config.ini"
        sed -i 's/^db_backend=.*/db_backend=rocksdb/' "$WORK/n6/config.ini"
        t0=$(date +%s)
        (cd "$WORK/n4" && "$BIN" --db-path db --config-file config.ini --convert-only --convert-batch 200 </dev/null >> stdout.log 2>&1) \
            || { echo "FAIL: n4 conversion to lmdb failed"; tail -20 "$WORK/n4/stdout.log"; exit 1; }
        echo "convert test: n4 berkeleydb -> lmdb at sequence $min in $(( $(date +%s) - t0 )) s"
        start_node 4
        (cd "$WORK/n6" && exec "$BIN" --db-path db --config-file config.ini --convert-db --convert-batch 200 </dev/null >> stdout.log 2>&1) &
        cpid=$!
        until [ "$(grep -ac 'converted blocks' "$WORK/n6/stdout.log")" -ge 2 ] || ! kill -0 "$cpid" 2>/dev/null; do sleep 0.2; done
        kill -KILL "$cpid" 2>/dev/null || true; wait "$cpid" 2>/dev/null || true
        [ -f "$WORK/n6/db/.convert" ] || { echo "FAIL: n6 was killed but has no db/.convert to resume"; exit 1; }
        echo "convert test: n6 killed during its conversion to rocksdb, restarting it"
        EXTRA_ARGS_6="--convert-db --convert-batch 200" start_node 6
    fi
```

Support per-node extra arguments in `start_node` (one line):

```bash
    local extra_var="EXTRA_ARGS_$i"
    (cd "$WORK/n$i" && ulimit -c unlimited && exec "$bin" --db-path db --config-file config.ini ${!extra_var:-} </dev/null >> stdout.log 2>&1) &
```

and set `EXTRA_ARGS_6=` back to empty after this restart, so a later restart of n6 does not convert again.

- [ ] **Step 3: Add the final checks** (near the other end-of-run checks)

```bash
if [ "$CONVERT_AT" -gt 0 ]; then
    for i in 4 6; do
        want=$([ "$i" = 4 ] && echo lmdb || echo rocksdb)
        [ "$(cat "$WORK/n$i/db/.backend" 2>/dev/null)" = "$want" ] || { echo "FAIL: n$i db/.backend is not $want"; exit 1; }
        [ ! -e "$WORK/n$i/db/.convert" ] || { echo "FAIL: n$i still has db/.convert"; exit 1; }
        [ ! -e "$WORK/n$i/db/blockchain.db" ] || { echo "FAIL: n$i still has its BerkeleyDB files"; exit 1; }
        grep -aq 'conversion done' "$WORK/n$i/stdout.log" || { echo "FAIL: n$i did not finish its conversion"; exit 1; }
    done
    grep -aq 'resuming the conversion' "$WORK/n6/stdout.log" || { echo "FAIL: n6 did not resume its conversion"; exit 1; }
    for i in 4 6; do
        after=$(awk '/conversion done/{f=1} f' "$WORK/n$i/stdout.log")
        grep -aq 'QUICK START!' <<<"$after" || { echo "FAIL: n$i did not quick-start after the conversion"; exit 1; }
    done
    echo "convert test: n4 now lmdb, n6 now rocksdb (resumed after a kill), both quick-started"
fi
# every node started from an empty db/: the new start-up check must have stayed silent there
for i in $(seq 1 "$ALL_NODES"); do
    first_start=$(awk '/conversion done|resuming the conversion/{exit} {print}' "$WORK/n$i/stdout.log")
    if [ "$CONVERT_AT" -eq 0 ] || { [ "$i" != 4 ] && [ "$i" != 6 ]; }; then
        ! grep -aq 'block database:' <<<"$first_start" || { echo "FAIL: n$i printed a block database message on a normal start"; exit 1; }
    fi
done
```

(The digest check that already ends every scenario proves the converted nodes keep the network's state.)

- [ ] **Step 4: Add the scenario to `robustness.yml`**

```yaml
          - name: db-convert
            env: DB_BACKEND=berkeleydb CONVERT_AT=1500 TARGET_SEQ=2500
```

- [ ] **Step 5: Validate locally without running the network**

Run: `bash -n ci/testnet/run.sh && python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/robustness.yml'))"`
Expected: no output, exit 0.

- [ ] **Step 6: Commit and run on Actions**

```bash
git add ci/testnet/run.sh .github/workflows/robustness.yml
git commit -m "ci(robustness): convert two nodes' block databases mid-run, one of them killed and resumed"
git push -u origin <branch>
gh workflow run robustness.yml -R EnzinoBB/node --ref <branch> -f only=db-convert,storage-switch,all-storage
```

Put the "every node started from an empty db/" check outside the `CONVERT_AT` block, so all 27 existing scenarios run it too.

Expected: the three scenarios PASS; `db-convert` prints both "convert test:" lines and "PASS: 7 nodes reached sequence 2500 with matching state digests".

---

### Task 7: Conversion on mainnet data and the time comparison

**Files:**
- Modify: `.github/workflows/upgrade-compat.yml` (step "Run the three phases")

**Interfaces:**
- Consumes: the community binary built by the workflow (`node-community`), `--convert-only`, the official folder after phase 2 (BerkeleyDB, a few million mainnet blocks), `ci/snapshot/check_node.py balances|history`.
- Produces: log lines `TIMING ...` and a job summary table.

- [ ] **Step 1: Record the network sync rate of phase 1** right after `s1=$(seq_now)`:

```bash
          sync_rate=$(( s1 / (30 * 60) ))   # blocks per second from the network, phase 1
          echo "TIMING network sync: $s1 blocks in 1800 s ($sync_rate blocks/s)"
```

- [ ] **Step 2: Add the conversion phases between phase 2 and phase 3** (after `stop community`):

```bash
          convert() {  # convert <backend>; prints and appends a TIMING line
            sed -i "/^\[storage\]/,/^\[/{/^db_backend=/d}" "$node/config.ini"
            grep -q '^\[storage\]' "$node/config.ini" || printf '\n[storage]\n' >> "$node/config.ini"
            sed -i "/^\[storage\]/a db_backend=$1" "$node/config.ini"
            before=$(du -sb "$node/db" | cut -f1); t0=$(date +%s)
            docker run --rm --network host --user "$(id -u)" -v "$node:/node" -w /node ubuntu:20.04 \
              ./node-community --db-path db --config-file config.ini --convert-only > "$node/convert-$1.log" 2>&1 \
              || { echo "FAIL: conversion to $1"; tail -20 "$node/convert-$1.log"; exit 1; }
            secs=$(( $(date +%s) - t0 )); after=$(du -sb "$node/db" | cut -f1)
            blocks=$(grep -a 'conversion done' "$node/convert-$1.log" | sed -E 's/.*done: ([0-9]+) blocks.*/\1/')
            echo "TIMING convert to $1: $blocks blocks in $secs s, $((before >> 20)) MB -> $((after >> 20)) MB"
            echo "| to $1 | $blocks | $secs s | $((before >> 20)) MB → $((after >> 20)) MB | $(( 186000000 * secs / (blocks > 0 ? blocks : 1) / 3600 )) h |" >> "$GITHUB_STEP_SUMMARY"
          }
          {
            echo "### Block database conversion vs network sync"
            echo "Network sync (phase 1): $s1 blocks in 30 min → $(( 186000000 / (sync_rate > 0 ? sync_rate : 1) / 3600 )) h for 186 M blocks"
            echo ""
            echo "| conversion | blocks | time | size | whole chain (est.) |"
            echo "|---|---|---|---|---|"
          } >> "$GITHUB_STEP_SUMMARY"

          echo "== phase 2b: convert the official folder to lmdb, run the community node 5 minutes"
          convert lmdb
          phase node-community 5
          after=$(docker logs credits 2>&1)
          grep -aq 'QUICK START!' <<<"$after" || { echo "FAIL: no quick start after the conversion"; docker logs --tail 40 credits; exit 1; }
          python3 ci/snapshot/check_node.py balances --api 127.0.0.1:9090 --count 10 --tries 40 --min-compared 3 \
            || { echo "FAIL: balances differ from the monitor after the conversion"; exit 1; }
          python3 ci/snapshot/check_node.py history --api 127.0.0.1:9090 --count 10 --tries 40 --min-compared 3 \
            || { echo "FAIL: histories differ from the monitor after the conversion"; exit 1; }
          stop community-lmdb

          echo "== phase 2c: lmdb -> rocksdb -> berkeleydb, back to the official format"
          convert rocksdb
          convert berkeleydb
```

Phase 3 then runs the official node on the folder converted back to BerkeleyDB: it proves that the official node opens a converted database (Review Focus 5). The `[storage]` block added for the conversion must not break the official config parser: if phase 3 fails on it, remove the `[storage]` section after `convert berkeleydb`.

- [ ] **Step 3: Validate the YAML and push with an `upgrade-test-*` tag**

```bash
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/upgrade-compat.yml'))"
git add .github/workflows/upgrade-compat.yml
git commit -m "ci(upgrade): convert the official folder to lmdb, rocksdb and back, and time it against the network sync"
git push -u origin <branch>
git tag upgrade-test-db-convert-1 && git push origin upgrade-test-db-convert-1
```

Expected: PASS; the job summary shows the table. Report the "whole chain (est.)" column for each conversion next to the network-sync estimate to the user, with the ship decision of the spec (ship only if the conversion is clearly faster).

---

### Task 8: Operator documentation

**Files:**
- Modify: `packaging/make-package.sh` (the README heredoc of the package)

- [ ] **Step 1: Add a section** to the package README:

```text
Changing the block database (db_backend)
  The node recognises the format of an existing db/ folder. If config.ini asks for another one, it
  offers to convert it (from a terminal), or stops and explains how (without a terminal). Run
  ./node --convert-only to convert and exit, or ./node --convert-db to convert and start.
  The conversion goes batch by batch and needs about 1 GB free beyond the database; if it is
  interrupted it resumes at the next start. Checkpoints and the transaction index are kept, so the
  node quick-starts afterwards. The official node only reads berkeleydb: convert back before
  running it on this folder.
```

- [ ] **Step 2: Check the script still runs**

Run: `bash -n packaging/make-package.sh`
Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add packaging/make-package.sh
git commit -m "packaging: document the block database conversion"
```

---

## Delivery

Build Tasks 1–4 and Task 7 on one feature branch first, and run Task 7 (the mainnet timing) before opening any PR: it needs the `--convert-only` flag of Task 4. Report its table to the user. If the conversion is not clearly faster than the network sync, keep only Task 2 plus a detection-only Task 4 (StopMismatch/StopBoth messages, no conversion), as the spec says.

If it is worth shipping, open the PRs from `master`, in this order so that each builds and is cherry-pickable:
1. Tasks 1–3 (csdb, three commits);
2. Task 4 (client);
3. Task 5 (Docker);
4. Tasks 6–8 (CI and docs).

Label the node PRs `testnet`.
