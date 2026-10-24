#ifndef STATEDIGEST_HPP
#define STATEDIGEST_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <csnode/walletscache.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Elliptic-curve multiset hash (ECMH) over ristretto255: an order-independent digest of a set of
// items, updated in O(1) per added or removed item. Two nodes holding the same set get the same
// digest regardless of the order in which the items were added.
class MultisetHash {
public:
    using Digest = std::array<uint8_t, 32>;
    using Element = std::array<uint8_t, 32>;

    MultisetHash();

    // the group element an item contributes; callers may keep it to remove the item later
    static Element toElement(const cs::Bytes& item);

    void add(const cs::Bytes& item);
    void remove(const cs::Bytes& item);
    void add(const Element& element);
    void remove(const Element& element);
    void reset();

    const Digest& digest() const {
        return sum_;
    }

private:
    Digest sum_;
};

// Canonical serialisation of the balance-relevant state of a wallet: key, balance, delegated amount
// and delegations in both directions, sorted. Empty for a wallet without such state, so that empty
// records do not change the digest.
cs::Bytes serializeWalletState(const WalletsCache::WalletData& wallet);

// The state a checkpoint holds, saved with it as state_digest.bin: the digest of the wallets
// (MultiWallets::stateDigest), their number, and a hash of the consensus settings the checkpoint
// carries (BlockChain::consensusSettingsDigest). A snapshot made from the checkpoint declares the
// same values, and a node that loads the checkpoint recomputes and compares them.
struct CheckpointDigest {
    static constexpr uint8_t kVersion = 1;
    static constexpr const char* kFileName = "state_digest.bin";

    MultisetHash::Digest digest{};
    uint64_t wallets = 0;
    std::array<uint8_t, 32> settings{};

    bool write(const std::filesystem::path& file) const;
    // nullopt for a missing, truncated or unknown-version file
    static std::optional<CheckpointDigest> read(const std::filesystem::path& file);

    bool operator==(const CheckpointDigest& other) const {
        return digest == other.digest && wallets == other.wallets && settings == other.settings;
    }
    bool operator!=(const CheckpointDigest& other) const {
        return !(*this == other);
    }
};

// "STATE DIGEST #<sequence> <hex> wallets <n>": the form every digest line of the log has, so that
// scripts read the same fields from the periodic, the start-up and the checkpoint lines
std::string stateDigestLine(cs::Sequence sequence, const MultisetHash::Digest& digest, uint64_t wallets);

}  // namespace cs

#endif  // STATEDIGEST_HPP
