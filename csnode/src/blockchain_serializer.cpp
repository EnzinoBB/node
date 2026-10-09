#include <fstream>
#include <limits>
#include <sstream>

//#include <boost/archive/text_oarchive.hpp>
//#include <boost/archive/text_iarchive.hpp>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

#include <csnode/blockchain.hpp>
#include <csnode/blockchain_serializer.hpp>
#include <csnode/serializers_helper.hpp>
#include <lib/system/logger.hpp>

namespace {
const std::string kDataFileName = "blockchain.dat";
const std::string kPendingFileName = "consensus_pending.dat";
constexpr uint8_t kPendingFormat = 1;
} // namespace

namespace cs {

void BlockChain_Serializer::bind(BlockChain& bchain, std::set<cs::PublicKey>& initialConfidants) {
    previousNonEmpty_ = reinterpret_cast<decltype(previousNonEmpty_)>(&bchain.previousNonEmpty_);
    lastNonEmptyBlock_ = reinterpret_cast<decltype(lastNonEmptyBlock_)>(&bchain.lastNonEmptyBlock_);
    totalTransactionsCount_ = &bchain.totalTransactionsCount_;
    uuid_ = &bchain.uuid_;
    lastSequence_ = &bchain.lastSequence_;
    initialConfidants_ = reinterpret_cast<decltype(initialConfidants_)>(&initialConfidants);
    blockRewardIntegral_ = reinterpret_cast<decltype(blockRewardIntegral_)>(&bchain.blockRewardIntegral_);
    blockRewardFraction_ = reinterpret_cast<decltype(blockRewardFraction_)>(&bchain.blockRewardFraction_);
    miningCoefficientIntegral_ = reinterpret_cast<decltype(miningCoefficientIntegral_)>(&bchain.miningCoefficientIntegral_);
    miningCoefficientFraction_ = reinterpret_cast<decltype(miningCoefficientFraction_)>(&bchain.miningCoefficientFraction_);
    stakingOn_ = &bchain.stakingOn_;
    miningOn_ = &bchain.miningOn_;
    TimeMinStage1_ = &bchain.TimeMinStage1_;
    pendingRound_ = &bchain.pendingConsensusSettings_.round;
    pendingStakingOn_ = &bchain.pendingConsensusSettings_.stakingOn;
    pendingMiningOn_ = &bchain.pendingConsensusSettings_.miningOn;
    pendingBlockReward_ = &bchain.pendingConsensusSettings_.blockReward;
    pendingMiningCoefficient_ = &bchain.pendingConsensusSettings_.miningCoefficient;
    startingDPOS_ = &bchain.startingDPOS_;
    csdebug() << "Blockchain bindings made";
}

void BlockChain_Serializer::clear(const std::filesystem::path& rootDir) {
    cslog() << "TRACE: BlockChain_Serializer::clear called for " << rootDir
            << "; resetting in-memory lastSequence_ from " << lastSequence_->load() << " to 0";
    previousNonEmpty_->clear();
    lastNonEmptyBlock_->poolSeq = 0;
    lastNonEmptyBlock_->transCount = 0;
    *totalTransactionsCount_ = 0;
    uuid_->store(0);
    lastSequence_->store(0);
    initialConfidants_->clear();
    *blockRewardIntegral_ = 0L;
    *blockRewardFraction_ = 0ULL;
    *miningCoefficientIntegral_= 0L;
    *miningCoefficientFraction_ = 0ULL;
    *stakingOn_ = false;
    *miningOn_ = false;
    *TimeMinStage1_ = 500;
    resetPending();
}

void BlockChain_Serializer::save(const std::filesystem::path& rootDir) {
    std::ofstream ofs(rootDir / kDataFileName, std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    oa << *previousNonEmpty_;
    oa << *lastNonEmptyBlock_;
    oa << *totalTransactionsCount_;
    oa << uuid_->load();
    oa << lastSequence_->load();
    oa << *initialConfidants_;
    oa << *blockRewardIntegral_ << *blockRewardFraction_;
    oa << *miningCoefficientIntegral_ << *miningCoefficientFraction_;
    uint8_t stakingOn = *stakingOn_ ? 0U : 1U;
    oa << stakingOn;
    uint8_t miningOn = *miningOn_ ? 0U : 1U;
    oa << miningOn;
    oa << *TimeMinStage1_;
    savePending(rootDir);
}

void BlockChain_Serializer::savePending(const std::filesystem::path& rootDir) {
    std::ofstream ofs(rootDir / kPendingFileName, std::ios::binary);
    boost::archive::binary_oarchive oa(ofs);
    oa << kPendingFormat;
    oa << *pendingRound_;
    oa << *pendingStakingOn_ << *pendingMiningOn_;
    oa << pendingBlockReward_->integral() << pendingBlockReward_->fraction();
    oa << pendingMiningCoefficient_->integral() << pendingMiningCoefficient_->fraction();
    oa << *startingDPOS_;
}

void BlockChain_Serializer::loadPending(const std::filesystem::path& rootDir) {
    resetPending();
    const auto path = rootDir / kPendingFileName;
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::ifstream ifs(path, std::ios::binary);
    boost::archive::binary_iarchive ia(ifs);
    uint8_t format = 0;
    ia >> format;
    if (format != kPendingFormat) {
        cswarning() << "BlockChain_Serializer: unknown " << kPendingFileName << " format " << int(format) << ", ignored";
        return;
    }
    int32_t rewardIntegral = 0, coefficientIntegral = 0;
    uint64_t rewardFraction = 0, coefficientFraction = 0;
    ia >> *pendingRound_;
    ia >> *pendingStakingOn_ >> *pendingMiningOn_;
    ia >> rewardIntegral >> rewardFraction;
    ia >> coefficientIntegral >> coefficientFraction;
    ia >> *startingDPOS_;
    *pendingBlockReward_ = csdb::Amount(rewardIntegral, rewardFraction);
    *pendingMiningCoefficient_ = csdb::Amount(coefficientIntegral, coefficientFraction);
}

void BlockChain_Serializer::resetPending() {
    *pendingRound_ = std::numeric_limits<cs::Sequence>::max();
    *pendingStakingOn_ = false;
    *pendingMiningOn_ = false;
    *pendingBlockReward_ = csdb::Amount{0};
    *pendingMiningCoefficient_ = csdb::Amount{0};
    *startingDPOS_ = 0;
}

::cscrypto::Hash BlockChain_Serializer::hash() {
    save(".");
    auto result = SerializersHelper::getHashFromFile(kDataFileName);
    std::filesystem::remove(kDataFileName);
    std::filesystem::remove(kPendingFileName);  // written by save(), not part of the checkpoint hash
    return result;
}

void BlockChain_Serializer::load(const std::filesystem::path& rootDir) {
    std::ifstream ifs(rootDir / kDataFileName, std::ios::binary);
    boost::archive::binary_iarchive ia(ifs);
    ia >> *previousNonEmpty_;
    ia >> *lastNonEmptyBlock_;
    ia >> *totalTransactionsCount_;

    uint64_t uuid;
    ia >> uuid;
    uuid_->store(uuid);

    Sequence lastSequence;
    ia >> lastSequence;
    cslog() << "TRACE: BlockChain_Serializer::load called for " << rootDir
            << "; setting in-memory lastSequence_ from " << lastSequence_->load()
            << " to " << lastSequence;
    lastSequence_->store(lastSequence);
    ia >> *initialConfidants_;

    ia >> *blockRewardIntegral_ >> *blockRewardFraction_;
    ia >> *miningCoefficientIntegral_ >> *miningCoefficientFraction_;
    uint8_t stakingOn;
    uint8_t miningOn;
    ia >> stakingOn;
    ia >> miningOn;
    *stakingOn_ = stakingOn == 0U ? true : false;
    *miningOn_ = miningOn == 0U ? true : false;

    ia >> *TimeMinStage1_;
    loadPending(rootDir);
}
}  // namespace cs
