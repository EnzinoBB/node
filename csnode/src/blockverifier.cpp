#include <csnode/blockverifier.hpp>

#include <algorithm>

#include <cscrypto/cscrypto.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/nodeutils.hpp>
#include <csnode/odatastream.hpp>
#include <lib/system/utils.hpp>

namespace cs {

bool BlockVerifier::verify(const csdb::Pool& block, cs::Sequence expectedSequence, const csdb::PoolHash& expectedHash, std::string& reason) {
    if (!block.is_valid()) {
        reason = "does not parse";
        return false;
    }
    if (block.sequence() != expectedSequence) {
        reason = "has sequence " + std::to_string(block.sequence()) + ", " + std::to_string(expectedSequence) + " expected";
        return false;
    }
    if (!expectedHash.is_empty() && block.hash() != expectedHash) {
        reason = "has another hash than the chain expects";
        return false;
    }
    return verifySignatures(block, reason);
}

bool BlockVerifier::verifySignatures(const csdb::Pool& block, std::string& reason) {
    if (block.sequence() == 0) {
        return true;
    }
    const auto& confidants = block.confidants();
    if (confidants.empty() || block.signatures().empty()) {
        reason = "has no confidants or no signatures";
        return false;
    }
    cs::Hash hash{};
    const auto bytes = block.hash().to_binary();
    if (bytes.size() != hash.size()) {
        reason = "has no hash";
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    const auto mask = cs::Utils::bitsToMask(block.numberTrusted(), block.realTrusted());
    if (!NodeUtils::checkGroupSignature(confidants, mask, block.signatures(), hash)) {
        reason = "has signatures that do not verify against its confidants";
        return false;
    }
    return true;
}

bool BlockVerifier::verifyConfirmations(const csdb::Pool& block, const cs::PublicKeys& previousConfidants, std::string& reason) {
    if (block.sequence() < 2) {
        return true;
    }
    const auto confMask = cs::Utils::bitsToMask(block.numberConfirmations(), block.roundConfirmationMask());
    if (BlockChain::isBootstrap(block) || confMask.size() <= 1) {
        return true;
    }
    cs::Bytes trustedToHash;
    cs::ODataStream stream(trustedToHash);
    stream << block.sequence();
    stream << block.confidants();
    const cs::Hash trustedHash = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());
    if (!NodeUtils::checkGroupSignature(previousConfidants, confMask, block.roundConfirmations(), trustedHash)) {
        reason = "has round confirmations that do not verify against the previous block's confidants";
        return false;
    }
    return true;
}

}  // namespace cs
