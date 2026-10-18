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

INSTANTIATE_TEST_CASE_P(AllBackends, ContractIter, ::testing::Values("berkeleydb", "lmdb", "rocksdb"));
