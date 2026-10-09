#ifndef STATEDIGEST_HPP
#define STATEDIGEST_HPP

#include <array>
#include <cstdint>

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

}  // namespace cs

#endif  // STATEDIGEST_HPP
