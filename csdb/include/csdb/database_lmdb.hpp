/**
 * @file database_lmdb.hpp
 * Block storage on LMDB, for pruned nodes ([storage] block_db=lmdb).
 */

#ifndef _CREDITS_CSDB_DATABASE_LMDB_H_INCLUDED_
#define _CREDITS_CSDB_DATABASE_LMDB_H_INCLUDED_

#include <csdb/database.hpp>

struct MDB_env;

namespace csdb {

// Same contract as DatabaseBerkeleyDB: blocks by 0-based sequence, a hash -> sequence index and
// contract data. BerkeleyDB's Recno keeps a placeholder for every removed record, so the pages of
// pruned blocks are never reused and its file keeps growing; LMDB frees the pages of removed
// records and reuses them, so a pruned node's storage stays the size of the blocks it keeps.
class DatabaseLmdb : public Database {
public:
    DatabaseLmdb();
    ~DatabaseLmdb() override;

    bool open(const std::string& path);

private:
    bool is_open() const final;
    bool put(const cs::Bytes& key, uint32_t seq_no, const cs::Bytes& value) final;
    bool get(const cs::Bytes& key, cs::Bytes* value) final;
    bool get(const uint32_t seq_no, cs::Bytes* value) final;
    bool remove(const cs::Bytes& key) final;
    bool remove_batch(const std::vector<cs::Bytes>& keys) final;
    // the environment runs with MDB_NOSYNC: write the data to disk now
    bool flush() final;
    bool seq_no(const cs::Bytes& key, uint32_t* value) final;
    bool write_batch(const ItemList&) final;
    IteratorPtr new_iterator() final;

    bool updateContractData(const cs::Bytes& key, const cs::Bytes& data) final;
    bool getContractData(const cs::Bytes& key, cs::Bytes& data) final;

    class Iterator;

    bool fail(int status, const char* what);

    MDB_env* env_ = nullptr;
    unsigned int blocks_ = 0;     // MDB_dbi: sequence (uint32, MDB_INTEGERKEY) -> block
    unsigned int seqNo_ = 0;      // MDB_dbi: block hash -> sequence
    unsigned int contracts_ = 0;  // MDB_dbi: contract address -> contract data
};

}  // namespace csdb
#endif  // _CREDITS_CSDB_DATABASE_LMDB_H_INCLUDED_
