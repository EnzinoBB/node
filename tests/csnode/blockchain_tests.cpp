#define TESTING

#include "clientconfigmock.hpp"
#include "gtest/gtest.h"

#include <filesystem>
#include <limits>
#include <set>

#include <csnode/blockchain.hpp>
#include <csnode/blockchain_serializer.hpp>

#include <csdb/amount_commission.hpp>
#include <csdb/currency.hpp>
#include <csdb/pool.hpp>
#include <csnode/nodecore.hpp>

TEST(BlockChain, block_service_info) {
    csdb::Pool block{};

    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));

    block = csdb::Pool{};

    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));
}

// mining and staking are toggled independently (special transaction order 37) and
// restored from quick-start caches through these accessors
TEST(BlockChain, MiningAndStakingSettersAreIndependent) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain blockChain(genesis, start);

    blockChain.setMiningOn(true);
    blockChain.setStakingOn(false);
    ASSERT_TRUE(blockChain.getMiningOn());
    ASSERT_FALSE(blockChain.getStakingOn());

    blockChain.setMiningOn(false);
    blockChain.setStakingOn(true);
    ASSERT_FALSE(blockChain.getMiningOn());
    ASSERT_TRUE(blockChain.getStakingOn());
}

// an "uncertain" own block is replaced in place by the network's version only when testContentEqual() holds;
// rewards and fees are credited through the trusted mask, so a different mask must not count as equal content
TEST(BlockChain, ContentEqualRequiresSameTrustedMask) {
    csdb::Pool own;
    own.set_sequence(10);
    own.add_number_trusted(3);
    own.add_real_trusted(0);

    csdb::Pool canonical = own.clone();
    ASSERT_TRUE(BlockChain::testContentEqual(own, canonical));

    canonical.add_real_trusted(cs::Utils::maskToBits(cs::Bytes{ 0, 255, 1 }));
    ASSERT_FALSE(BlockChain::testContentEqual(own, canonical));
}

// these members are saved into quick-start caches and read back on the --set-bc-top path,
// possibly before any consensus-settings change has set them
TEST(BlockChain, ConsensusSettingsHaveDefinedDefaults) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain blockChain(genesis, start);

    ASSERT_TRUE(blockChain.getBlockReward() == csdb::Amount{ 0 });
    ASSERT_TRUE(blockChain.getMiningCoefficient() == csdb::Amount{ 0 });
    ASSERT_FALSE(blockChain.getMiningOn());
    ASSERT_FALSE(blockChain.getStakingOn());
    ASSERT_EQ(blockChain.getTimeMinStage1(), 500u);
}

// a pending order-37 change and the order-9 StartingDPOS must survive a quick-start checkpoint,
// and a checkpoint written before this was saved must still load
TEST(BlockChain, PendingConsensusSettingsSurviveCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_pending_settings_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    {
        BlockChain saved(genesis, start);
        BlockChain::PendingConsensusSettings pending;
        pending.round = 400;
        pending.stakingOn = true;
        pending.miningOn = true;
        pending.blockReward = csdb::Amount{ 1 };
        saved.setPendingConsensusSettings(pending);
        saved.setStartingDPOS(450);

        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    ASSERT_EQ(loaded.getPendingConsensusSettings().round, 400u);
    ASSERT_TRUE(loaded.getPendingConsensusSettings().miningOn);
    ASSERT_TRUE(loaded.getPendingConsensusSettings().blockReward == csdb::Amount{ 1 });
    ASSERT_EQ(loaded.getStartingDPOS(), 450u);

    // older checkpoints have no companion file: nothing pending, StartingDPOS untouched
    std::filesystem::remove(dir / "consensus_pending.dat");
    serializer.load(dir);
    ASSERT_EQ(loaded.getPendingConsensusSettings().round, std::numeric_limits<cs::Sequence>::max());
    ASSERT_EQ(loaded.getStartingDPOS(), 0u);

    std::filesystem::remove_all(dir);
}

namespace {
// a transaction carrying a special order: u16 order number (little endian) then its fields
csdb::Transaction specialOrderTransaction(int64_t id, uint16_t order, uint64_t value) {
    csdb::Transaction transaction{id, csdb::Address::from_public_key(cs::PublicKey{}), csdb::Address::from_public_key(cs::PublicKey{}),
                                  csdb::Currency{1}, csdb::Amount{0}, csdb::AmountCommission{0.}, csdb::AmountCommission{0.}, cs::Signature{}};
    std::string payload(reinterpret_cast<const char*>(&order), sizeof(order));
    payload.append(reinterpret_cast<const char*>(&value), sizeof(value));
    transaction.add_user_field(cs::trx_uf::sp::managing, payload);
    return transaction;
}

uint64_t orderValue(const std::string& payload) {
    uint64_t value = 0;
    std::copy(payload.begin() + sizeof(uint16_t), payload.end(), reinterpret_cast<char*>(&value));
    return value;
}
}  // namespace

TEST(BlockChain, SpecialOrdersSurviveCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_special_orders_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    {
        BlockChain saved(genesis, start);
        csdb::Pool first;
        first.add_transaction(specialOrderTransaction(1, 26, 1000));  // MaxTransactionSize
        first.add_transaction(specialOrderTransaction(2, 35, 777));   // syncroChangeRound
        first.add_transaction(specialOrderTransaction(3, 37, 1));     // kept by the pending settings instead
        saved.recordSpecialOrders(first);
        csdb::Pool second;
        second.add_transaction(specialOrderTransaction(4, 26, 2000));  // a later order of the same kind wins
        saved.recordSpecialOrders(second);
        ASSERT_EQ(saved.getSpecialOrders().size(), 2u);

        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    const auto& orders = loaded.getSpecialOrders();
    ASSERT_EQ(orders.size(), 2u);
    ASSERT_EQ(orderValue(orders.at(26)), 2000u);
    ASSERT_EQ(orderValue(orders.at(35)), 777u);

    // older checkpoints have no companion file: nothing to replay
    std::filesystem::remove(dir / "consensus_pending.dat");
    serializer.load(dir);
    ASSERT_TRUE(loaded.getSpecialOrders().empty());

    std::filesystem::remove_all(dir);
}
