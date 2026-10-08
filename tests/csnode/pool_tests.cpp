#define TESTING

#include "gtest/gtest.h"

#include <cstring>

#include <csdb/pool.hpp>

// serialized pool and its hashing length (written as a raw size_t right after the hashed prefix)
static cs::Bytes poolBinary(size_t& hashingLength) {
    csdb::Pool pool;
    pool.set_sequence(1);

    cs::Bytes bytes = pool.to_binary_updated();
    hashingLength = pool.hashingLength();

    return bytes;
}

TEST(Pool, AcceptsSerializedPool) {
    size_t hashingLength = 0;
    cs::Bytes bytes = poolBinary(hashingLength);

    ASSERT_TRUE(csdb::Pool::from_binary(std::move(bytes)).is_valid());
}

// pool bytes come from the network (blocks requested while syncing)
TEST(Pool, RejectsHashingLengthPastData) {
    size_t hashingLength = 0;
    cs::Bytes bytes = poolBinary(hashingLength);

    ASSERT_GT(hashingLength, 0u);
    ASSERT_LE(hashingLength + sizeof(size_t), bytes.size());

    const size_t forged = bytes.size() + 1024 * 1024;
    std::memcpy(bytes.data() + hashingLength, &forged, sizeof(forged));

    ASSERT_FALSE(csdb::Pool::from_binary(cs::Bytes(bytes)).is_valid());
    ASSERT_TRUE(csdb::Pool::hash_from_binary(cs::Bytes(bytes)).is_empty());
}
