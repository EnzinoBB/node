#define TESTING

#include "clientconfigmock.hpp"
#include "gtest/gtest.h"

#include <array>
#include <filesystem>
#include <functional>
#include <vector>
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

// The block rebuilt from this node's block and the writer's round package must be the writer's block,
// byte for byte (its hash is what the package's signers signed): mask, timestamp and the block reward
// come from the package, everything else, the bootstrap flag included, from this node's block
TEST(BlockChain, BlockFromPackageMetaIsTheWritersBlock) {
    const cs::PublicKey k1{1}, k2{2}, k3{3}, k4{4}, k5{5};
    auto base = [&](const cs::Bytes& mask, const std::string& timestamp, const std::string& reward) {
        csdb::Pool p;
        p.set_sequence(1433);
        p.set_previous_hash(csdb::PoolHash::calc_from_data(cs::Bytes{1, 2, 3}));
        p.add_number_trusted(static_cast<uint8_t>(mask.size()));
        p.add_real_trusted(cs::Utils::maskToBits(mask));
        p.set_confidants({k1, k2, k3, k4, k5});
        p.setRoundCost(csdb::Amount(0));
        BlockChain::setTimestamp(p, timestamp);
        BlockChain::setBootstrap(p, true);
        if (!reward.empty()) {
            p.add_user_field(BlockChain::kFieldBlockReward, reward);
        }
        p.add_number_confirmations(3);
        p.add_confirmation_mask(7);
        p.add_round_confirmations({cs::Signature{}, cs::Signature{}, cs::Signature{}});
        return p;
    };
    const cs::Bytes writerMask{0, 1, 255, 255, 2};
    csdb::Pool writer = base(writerMask, "1792732732167", "reward-of-three");
    csdb::Pool own = base(cs::Bytes{0, 1, 2, 3, 4}, "1792732732100", "reward-of-five");
    ASSERT_TRUE(writer.compose());

    cs::PoolMetaInfo meta;
    meta.sequenceNumber = 1433;
    meta.previousHash = writer.previous_hash();
    meta.realTrustedMask = writerMask;
    meta.timestamp = "1792732732167";
    meta.reward = "reward-of-three";

    csdb::Pool rebuilt = BlockChain::blockFromPackageMeta(own, meta);
    ASSERT_TRUE(rebuilt.compose());
    EXPECT_EQ(rebuilt.hash(), writer.hash());
    EXPECT_TRUE(BlockChain::isBootstrap(rebuilt));

    // without a reward on either side
    csdb::Pool writerNoReward = base(writerMask, "1792732732167", "");
    ASSERT_TRUE(writerNoReward.compose());
    meta.reward.clear();
    csdb::Pool rebuiltNoReward = BlockChain::blockFromPackageMeta(base(cs::Bytes{0, 1, 2, 3, 4}, "1", "reward"), meta);
    ASSERT_TRUE(rebuiltNoReward.compose());
    EXPECT_EQ(rebuiltNoReward.hash(), writerNoReward.hash());
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

namespace {
void setEverySetting(BlockChain& chain) {
    chain.setBlockReward(csdb::Amount{ 3 });
    chain.setMiningCoefficient(csdb::Amount{ 2 });
    chain.setStakingOn(true);
    chain.setMiningOn(true);
    chain.setTimeMinStage1(700);
    BlockChain::PendingConsensusSettings pending;
    pending.round = 900;
    pending.stakingOn = true;
    pending.blockReward = csdb::Amount{ 5 };
    chain.setPendingConsensusSettings(pending);
    chain.setStartingDPOS(450);
    csdb::Pool orders;
    orders.add_transaction(specialOrderTransaction(1, 26, 1000));
    orders.add_transaction(specialOrderTransaction(2, 35, 777));
    chain.recordSpecialOrders(orders);
}
}  // namespace

// a checkpoint saves the hash of the consensus settings with the caches, and a quick start
// rejects the checkpoint when the loaded settings hash differently: every field must come back
// from blockchain.dat and consensus_pending.dat exactly
TEST(BlockChain, ConsensusSettingsDigestSurvivesCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_settings_digest_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    std::array<uint8_t, 32> savedDigest{};
    {
        BlockChain saved(genesis, start);
        setEverySetting(saved);
        savedDigest = saved.consensusSettingsDigest();
        ASSERT_EQ(saved.checkpointDigest().settings, savedDigest);
        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    ASSERT_NE(loaded.consensusSettingsDigest(), savedDigest);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    ASSERT_EQ(loaded.consensusSettingsDigest(), savedDigest);

    std::filesystem::remove_all(dir);
}

TEST(BlockChain, ConsensusSettingsDigestCoversEverySetting) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain base(genesis, start);
    setEverySetting(base);
    const auto digest = base.consensusSettingsDigest();

    const std::vector<std::function<void(BlockChain&)>> changes = {
        [](BlockChain& c) { c.setBlockReward(csdb::Amount{ 4 }); },
        [](BlockChain& c) { c.setMiningCoefficient(csdb::Amount{ 1 }); },
        [](BlockChain& c) { c.setStakingOn(false); },
        [](BlockChain& c) { c.setMiningOn(false); },
        [](BlockChain& c) { c.setTimeMinStage1(800); },
        [](BlockChain& c) { auto p = c.getPendingConsensusSettings(); p.round = 901; c.setPendingConsensusSettings(p); },
        [](BlockChain& c) { auto p = c.getPendingConsensusSettings(); p.miningCoefficient = csdb::Amount{ 9 }; c.setPendingConsensusSettings(p); },
        [](BlockChain& c) { c.setStartingDPOS(451); },
        [](BlockChain& c) { csdb::Pool o; o.add_transaction(specialOrderTransaction(3, 26, 2000)); c.recordSpecialOrders(o); },
    };
    for (size_t i = 0; i < changes.size(); ++i) {
        BlockChain changed(genesis, start);
        setEverySetting(changed);
        ASSERT_EQ(changed.consensusSettingsDigest(), digest);
        changes[i](changed);
        EXPECT_NE(changed.consensusSettingsDigest(), digest) << "change " << i;
    }
}
