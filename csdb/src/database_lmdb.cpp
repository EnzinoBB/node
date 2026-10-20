#include <csdb/database_lmdb.hpp>

#include <lmdb.h>

#include <limits>

#include <boost/filesystem.hpp>

namespace csdb {

namespace {
// a sparse file on 64-bit systems: only the pages in use take disk space
constexpr size_t kMapSize = size_t(1) << 40;

MDB_val toVal(const cs::Bytes& bytes) {
    return MDB_val{bytes.size(), const_cast<cs::Byte*>(bytes.data())};
}

MDB_val toVal(uint32_t& seq) {
    return MDB_val{sizeof(seq), &seq};
}

void assign(cs::Bytes* out, const MDB_val& val) {
    auto begin = static_cast<const cs::Byte*>(val.mv_data);
    out->assign(begin, begin + val.mv_size);
}

// a transaction aborted on scope exit unless committed
class Txn {
public:
    Txn(MDB_env* env, bool readOnly) {
        status_ = mdb_txn_begin(env, nullptr, readOnly ? MDB_RDONLY : 0, &txn_);
    }
    ~Txn() {
        if (txn_ != nullptr) {
            mdb_txn_abort(txn_);
        }
    }
    int status() const {
        return status_;
    }
    MDB_txn* get() const {
        return txn_;
    }
    int commit() {
        int status = mdb_txn_commit(txn_);
        txn_ = nullptr;
        return status;
    }

private:
    MDB_txn* txn_ = nullptr;
    int status_ = 0;
};
}  // namespace

DatabaseLmdb::DatabaseLmdb() = default;

DatabaseLmdb::~DatabaseLmdb() {
    if (env_ != nullptr) {
        mdb_env_sync(env_, 1);
        mdb_env_close(env_);
    }
}

bool DatabaseLmdb::fail(int status, const char* what) {
    set_last_error(status == MDB_NOTFOUND ? NotFound : UnknownError, "LMDB %s: %s", what, mdb_strerror(status));
    return false;
}

bool DatabaseLmdb::open(const std::string& path) {
    boost::system::error_code ec;
    boost::filesystem::create_directories(path, ec);

    int status = mdb_env_create(&env_);
    if (status != 0) {
        env_ = nullptr;
        return fail(status, "env_create");
    }
    status = mdb_env_set_mapsize(env_, kMapSize);
    status = status ? status : mdb_env_set_maxdbs(env_, 3);
    // MDB_NOTLS: transactions are not bound to threads (sync, API and node threads all read);
    // MDB_NOSYNC as BerkeleyDB's DB_TXN_NOSYNC: a system crash may lose the last blocks, which the
    // node fetches again, but not the database's integrity
    status = status ? status : mdb_env_open(env_, path.c_str(), MDB_NOTLS | MDB_NOSYNC, 0644);
    if (status != 0) {
        mdb_env_close(env_);
        env_ = nullptr;
        return fail(status, "env_open");
    }

    const char* step = "txn_begin";
    {
        Txn txn(env_, false);
        status = txn.status();
        if (status == 0) {
            step = "dbi_open";
            status = mdb_dbi_open(txn.get(), "blocks", MDB_CREATE | MDB_INTEGERKEY, &blocks_);
            status = status ? status : mdb_dbi_open(txn.get(), "seq_no", MDB_CREATE, &seqNo_);
            status = status ? status : mdb_dbi_open(txn.get(), "contracts", MDB_CREATE, &contracts_);
            status = status ? status : txn.commit();
        }
    }   // an uncommitted transaction is aborted here, before the environment is closed
    if (status != 0) {
        // without its tables the environment is of no use: close it, so that is_open() is false and
        // the storage refuses to start instead of running on handles every call rejects
        mdb_env_close(env_);
        env_ = nullptr;
        return fail(status, step);
    }

    set_last_error();
    return true;
}

bool DatabaseLmdb::is_open() const {
    return env_ != nullptr;
}

bool DatabaseLmdb::put(const cs::Bytes& key, uint32_t seq_no, const cs::Bytes& value) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, false);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val seq = toVal(seq_no);
    MDB_val block = toVal(value);
    MDB_val hash = toVal(key);
    int status = mdb_put(txn.get(), blocks_, &seq, &block, 0);
    status = status ? status : mdb_put(txn.get(), seqNo_, &hash, &seq, 0);
    status = status ? status : txn.commit();
    if (status != 0) {
        return fail(status, "put");
    }
    set_last_error();
    return true;
}

bool DatabaseLmdb::get(const cs::Bytes& key, cs::Bytes* value) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, true);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val hash = toVal(key);
    MDB_val seq;
    int status = mdb_get(txn.get(), seqNo_, &hash, &seq);
    if (status != 0) {
        return fail(status, "get");
    }
    if (value == nullptr) {
        return true;  // only asked whether the block is stored
    }
    MDB_val block;
    status = mdb_get(txn.get(), blocks_, &seq, &block);
    if (status != 0) {
        return fail(status, "get");
    }
    assign(value, block);
    set_last_error();
    return true;
}

bool DatabaseLmdb::get(const uint32_t seq_no, cs::Bytes* value) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    if (value == nullptr) {
        return false;
    }
    Txn txn(env_, true);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    uint32_t key = seq_no;
    MDB_val seq = toVal(key);
    MDB_val block;
    int status = mdb_get(txn.get(), blocks_, &seq, &block);
    if (status != 0) {
        return fail(status, "get");
    }
    assign(value, block);
    set_last_error();
    return true;
}

bool DatabaseLmdb::remove(const cs::Bytes& key) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, false);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val hash = toVal(key);
    MDB_val found;
    int status = mdb_get(txn.get(), seqNo_, &hash, &found);
    if (status != 0) {
        return fail(status, "remove");
    }
    uint32_t seqNo = *static_cast<const uint32_t*>(found.mv_data);
    MDB_val seq = toVal(seqNo);
    status = mdb_del(txn.get(), seqNo_, &hash, nullptr);
    status = status ? status : mdb_del(txn.get(), blocks_, &seq, nullptr);
    status = status ? status : txn.commit();
    if (status != 0) {
        return fail(status, "remove");
    }
    set_last_error();
    return true;
}

// Pruning removes many old blocks in a row: one transaction for all of them instead of one each.
// Every commit copies the changed tree path and frees the old pages, which LMDB can reuse only
// once no later reader may see them; millions of tiny transactions build a backlog of such pages
// and the file grows (seen on mainnet: a 1.7 GB file holding 360 MB of blocks).
bool DatabaseLmdb::flush() {
    if (env_ == nullptr) {
        return false;
    }
    const int status = mdb_env_sync(env_, 1);
    return status == MDB_SUCCESS || fail(status, "env_sync");
}

bool DatabaseLmdb::remove_batch(const std::vector<cs::Bytes>& keys) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, false);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    for (const auto& key : keys) {
        MDB_val hash = toVal(key);
        MDB_val found;
        int status = mdb_get(txn.get(), seqNo_, &hash, &found);
        if (status == MDB_NOTFOUND) {
            continue;
        }
        if (status != 0) {
            return fail(status, "remove_batch");
        }
        uint32_t seqNo = *static_cast<const uint32_t*>(found.mv_data);
        MDB_val seq = toVal(seqNo);
        status = mdb_del(txn.get(), seqNo_, &hash, nullptr);
        if (status == 0) {
            status = mdb_del(txn.get(), blocks_, &seq, nullptr);
        }
        if (status != 0 && status != MDB_NOTFOUND) {
            return fail(status, "remove_batch");
        }
    }
    int status = txn.commit();
    if (status != 0) {
        return fail(status, "remove_batch");
    }
    set_last_error();
    return true;
}

bool DatabaseLmdb::seq_no(const cs::Bytes& key, uint32_t* value) {
    if (value == nullptr) {
        set_last_error(InvalidArgument);
        return false;
    }
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, true);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val hash = toVal(key);
    MDB_val seq;
    int status = mdb_get(txn.get(), seqNo_, &hash, &seq);
    if (status != 0) {
        return fail(status, "seq_no");
    }
    *value = *static_cast<const uint32_t*>(seq.mv_data);
    set_last_error();
    return true;
}

bool DatabaseLmdb::write_batch(const ItemList&) {
    set_last_error(NotSupported);
    return false;
}

bool DatabaseLmdb::updateContractData(const cs::Bytes& key, const cs::Bytes& data) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, false);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val k = toVal(key);
    MDB_val v = toVal(data);
    int status = mdb_put(txn.get(), contracts_, &k, &v, 0);
    status = status ? status : txn.commit();
    if (status != 0) {
        return fail(status, "updateContractData");
    }
    set_last_error();
    return true;
}

bool DatabaseLmdb::getContractData(const cs::Bytes& key, cs::Bytes& data) {
    if (!is_open()) {
        set_last_error(NotOpen);
        return false;
    }
    Txn txn(env_, true);
    if (txn.status() != 0) {
        return fail(txn.status(), "txn_begin");
    }
    MDB_val k = toVal(key);
    MDB_val v;
    int status = mdb_get(txn.get(), contracts_, &k, &v);
    if (status != 0) {
        return fail(status, "getContractData");
    }
    assign(&data, v);
    set_last_error();
    return true;
}

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

// Walks the blocks in sequence order. It holds a read transaction, which keeps LMDB from reusing
// the pages it sees: iterators are short-lived (the storage scan at start-up).
class DatabaseLmdb::Iterator final : public Database::Iterator {
public:
    Iterator(MDB_env* env, unsigned int blocks) {
        if (mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn_) != 0) {
            txn_ = nullptr;
            return;
        }
        if (mdb_cursor_open(txn_, blocks, &cursor_) != 0) {
            cursor_ = nullptr;
        }
    }
    ~Iterator() final {
        if (cursor_ != nullptr) {
            mdb_cursor_close(cursor_);
        }
        if (txn_ != nullptr) {
            mdb_txn_abort(txn_);
        }
    }

    bool is_valid() const final {
        return valid_;
    }
    void seek_to_first() final {
        move(nullptr, MDB_FIRST);
    }
    void seek_to_last() final {
        move(nullptr, MDB_LAST);
    }
    void seek(const cs::Bytes&) final {
        valid_ = false;  // by hash is not used, as with BerkeleyDB
    }
    void seek(const uint32_t sec_no) final {
        uint32_t key = sec_no;
        MDB_val k = toVal(key);
        move(&k, MDB_SET_KEY);
    }
    void next() final {
        move(nullptr, MDB_NEXT);
    }
    void prev() final {
        move(nullptr, MDB_PREV);
    }
    uint32_t key() const final {
        return valid_ ? key_ : std::numeric_limits<uint32_t>::max();
    }
    cs::Bytes value() const final {
        return valid_ ? value_ : cs::Bytes{};
    }

private:
    void move(MDB_val* key, MDB_cursor_op op) {
        valid_ = false;
        if (cursor_ == nullptr) {
            return;
        }
        MDB_val k{};
        MDB_val v{};
        if (key != nullptr) {
            k = *key;
        }
        if (mdb_cursor_get(cursor_, &k, &v, op) != 0) {
            return;
        }
        key_ = *static_cast<const uint32_t*>(k.mv_data);
        assign(&value_, v);
        valid_ = true;
    }

    MDB_txn* txn_ = nullptr;
    MDB_cursor* cursor_ = nullptr;
    bool valid_ = false;
    uint32_t key_ = 0;
    cs::Bytes value_;
};

Database::IteratorPtr DatabaseLmdb::new_iterator() {
    if (!is_open()) {
        return nullptr;
    }
    return std::make_shared<Iterator>(env_, blocks_);
}

}  // namespace csdb
