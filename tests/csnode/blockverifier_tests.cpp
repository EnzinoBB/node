#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/blockverifier.hpp>
#include <csnode/odatastream.hpp>

namespace {
struct Keys {
    std::vector<cs::PublicKey> pub;
    std::vector<cs::PrivateKey> priv;
};

Keys makeKeys(size_t n) {
    // keys live in sodium's guarded memory: the library must be initialised first
    EXPECT_TRUE(cscrypto::cryptoInit());
    Keys k;
    for (size_t i = 0; i < n; ++i) {
        cs::PublicKey p;
        k.priv.push_back(cscrypto::generateKeyPair(p));
        k.pub.push_back(p);
    }
    return k;
}

cs::Hash toHash(const csdb::PoolHash& h) {
    cs::Hash out{};
    const auto b = h.to_binary();
    std::copy(b.begin(), b.end(), out.begin());
    return out;
}

// a block of `seq` whose confidants are `keys`, signed by those in `signers` (bit i = confidant i),
// confirmed by the previous round's confidants `prev` (all of them), parsed back from its bytes
csdb::Pool signedBlock(cs::Sequence seq, const Keys& keys, uint64_t signers, const Keys* prev = nullptr,
                       const std::string& payload = "payload") {
    csdb::Pool p(csdb::PoolHash{}, seq);
    p.set_confidants(keys.pub);
    p.add_number_trusted(static_cast<uint8_t>(keys.pub.size()));
    p.add_real_trusted(signers);
    p.add_user_field(1, payload);
    if (prev) {
        cs::Bytes trustedToHash;
        cs::ODataStream stream(trustedToHash);
        stream << seq;
        stream << keys.pub;
        const cs::Hash h = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());
        std::vector<cs::Signature> confirmations;
        for (const auto& k : prev->priv) {
            confirmations.push_back(cscrypto::generateSignature(k, h.data(), h.size()));
        }
        p.add_number_confirmations(static_cast<uint8_t>(prev->pub.size()));
        p.add_confirmation_mask((1ull << prev->pub.size()) - 1);
        p.add_round_confirmations(confirmations);
    }
    // the signatures are outside the hashed part: the hash of a composed copy is the block's hash,
    // and the signatures are set before the block itself is composed (it is read-only after)
    auto probe = p.clone();
    EXPECT_TRUE(probe.compose());
    const cs::Hash hash = toHash(probe.hash());
    std::vector<cs::Signature> sigs;
    for (size_t i = 0; i < keys.priv.size(); ++i) {
        if (signers >> i & 1) {
            sigs.push_back(cscrypto::generateSignature(keys.priv[i], hash.data(), hash.size()));
        }
    }
    p.set_signatures(sigs);
    EXPECT_TRUE(p.compose());
    return csdb::Pool::from_binary(p.to_binary());
}
}  // namespace

TEST(BlockVerifier, ValidBlockPasses) {
    const auto keys = makeKeys(5);
    const auto block = signedBlock(10, keys, 0b10111);
    std::string reason;
    EXPECT_TRUE(cs::BlockVerifier::verify(block, 10, block.hash(), reason)) << reason;
    EXPECT_TRUE(cs::BlockVerifier::verify(block, 10, csdb::PoolHash{}, reason)) << reason;
}

TEST(BlockVerifier, WrongSequenceOrHashFails) {
    const auto keys = makeKeys(3);
    const auto block = signedBlock(10, keys, 0b111);
    const auto other = signedBlock(10, keys, 0b111, nullptr, "another payload");
    std::string reason;
    EXPECT_FALSE(cs::BlockVerifier::verify(block, 11, block.hash(), reason));
    EXPECT_FALSE(cs::BlockVerifier::verify(block, 10, other.hash(), reason));
}

TEST(BlockVerifier, AlteredOrMissingSignatureFails) {
    const auto keys = makeKeys(3);
    const auto block = signedBlock(10, keys, 0b111);
    std::string reason;

    // the pool signatures follow the hashed part and its length (a size_t): flip a bit of the first
    auto bytes = block.to_binary();
    bytes[block.hashingLength() + sizeof(size_t)] ^= 1;
    const auto altered = csdb::Pool::from_binary(std::move(bytes));
    ASSERT_TRUE(altered.is_valid());
    EXPECT_EQ(altered.hash(), block.hash());   // outside the hash: only the signature check sees it
    EXPECT_FALSE(cs::BlockVerifier::verifySignatures(altered, reason));

    // signatures by keys other than the confidants'
    auto wrongKeys = makeKeys(3);
    wrongKeys.pub = keys.pub;
    EXPECT_FALSE(cs::BlockVerifier::verifySignatures(signedBlock(10, wrongKeys, 0b111), reason));

    // more signatures than the mask has entries: realTrusted with a bit beyond numberTrusted
    {
        auto four = makeKeys(4);
        csdb::Pool p(csdb::PoolHash{}, 10);
        p.set_confidants(keys.pub);
        p.add_number_trusted(3);
        p.add_real_trusted(0b1111);
        auto probe = p.clone();
        ASSERT_TRUE(probe.compose());
        const cs::Hash hash = toHash(probe.hash());
        std::vector<cs::Signature> sigs;
        for (size_t i = 0; i < 3; ++i) {
            sigs.push_back(cscrypto::generateSignature(keys.priv[i], hash.data(), hash.size()));
        }
        sigs.push_back(cscrypto::generateSignature(four.priv[3], hash.data(), hash.size()));
        p.set_signatures(sigs);
        ASSERT_TRUE(p.compose());
        const auto extra = csdb::Pool::from_binary(p.to_binary());
        ASSERT_TRUE(extra.is_valid());
        EXPECT_FALSE(cs::BlockVerifier::verifySignatures(extra, reason));
    }

    // no confidants, no signatures
    {
        csdb::Pool p(csdb::PoolHash{}, 10);
        p.add_number_trusted(0);
        p.add_real_trusted(0);
        ASSERT_TRUE(p.compose());
        const auto empty = csdb::Pool::from_binary(p.to_binary());
        ASSERT_TRUE(empty.is_valid());
        EXPECT_FALSE(cs::BlockVerifier::verifySignatures(empty, reason));
        EXPECT_NE(reason.find("no confidants"), std::string::npos) << reason;
    }

    // a block that does not parse is never taken for the genesis block
    EXPECT_FALSE(cs::BlockVerifier::verifySignatures(csdb::Pool{}, reason));
    EXPECT_FALSE(cs::BlockVerifier::verify(csdb::Pool{}, 0, csdb::PoolHash{}, reason));
}

TEST(BlockVerifier, GenesisNeedsNoSignature) {
    csdb::Pool genesis(csdb::PoolHash{}, 0);
    ASSERT_TRUE(genesis.compose());
    const auto parsed = csdb::Pool::from_binary(genesis.to_binary());
    std::string reason;
    EXPECT_TRUE(cs::BlockVerifier::verify(parsed, 0, parsed.hash(), reason)) << reason;
}

TEST(BlockVerifier, RoundConfirmationsAgainstThePreviousConfidants) {
    const auto previous = makeKeys(3);
    const auto keys = makeKeys(3);
    const auto block = signedBlock(20, keys, 0b111, &previous);
    std::string reason;
    EXPECT_TRUE(cs::BlockVerifier::verifyConfirmations(block, previous.pub, reason)) << reason;
    EXPECT_FALSE(cs::BlockVerifier::verifyConfirmations(block, keys.pub, reason));
    // a normal round's block without confirmations is refused, as storeBlock does
    EXPECT_FALSE(cs::BlockVerifier::verifyConfirmations(signedBlock(20, keys, 0b111), previous.pub, reason));
    // confirmations from a set of another size
    EXPECT_FALSE(cs::BlockVerifier::verifyConfirmations(block, makeKeys(4).pub, reason));
}
