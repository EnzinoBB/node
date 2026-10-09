#include <csnode/statedigest.hpp>

#include <algorithm>
#include <tuple>
#include <vector>

#include <sodium.h>

#include <csnode/nodecore.hpp>

namespace {
static_assert(crypto_core_ristretto255_BYTES == 32, "ristretto255 element size");

void putU8(cs::Bytes& out, uint8_t value) {
    out.push_back(value);
}

void putU32(cs::Bytes& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void putU64(cs::Bytes& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void putAmount(cs::Bytes& out, const csdb::Amount& amount) {
    putU32(out, static_cast<uint32_t>(amount.integral()));
    putU64(out, amount.fraction());
}

void putKey(cs::Bytes& out, const cs::PublicKey& key) {
    out.insert(out.end(), key.begin(), key.end());
}

using Delegations = std::shared_ptr<std::map<cs::PublicKey, std::vector<cs::TimeMoney>>>;

// std::map keeps the counterparts sorted by key; the entries of each counterpart are sorted here,
// since their order depends on how blocks were applied and reverted
void putDelegations(cs::Bytes& out, const Delegations& delegations) {
    if (!delegations) {
        putU32(out, 0);
        return;
    }

    putU32(out, static_cast<uint32_t>(delegations->size()));
    for (const auto& [counterpart, entries] : *delegations) {
        auto sorted = entries;
        std::sort(sorted.begin(), sorted.end(), [](const cs::TimeMoney& lhs, const cs::TimeMoney& rhs) {
            return std::make_tuple(lhs.time, lhs.initialTime, lhs.amount.integral(), lhs.amount.fraction(), static_cast<uint8_t>(lhs.coeff)) <
                   std::make_tuple(rhs.time, rhs.initialTime, rhs.amount.integral(), rhs.amount.fraction(), static_cast<uint8_t>(rhs.coeff));
        });

        putKey(out, counterpart);
        putU32(out, static_cast<uint32_t>(sorted.size()));
        for (const auto& entry : sorted) {
            putU64(out, entry.initialTime);
            putU64(out, entry.time);
            putAmount(out, entry.amount);
            putU8(out, static_cast<uint8_t>(entry.coeff));
        }
    }
}

bool hasEntries(const Delegations& delegations) {
    if (!delegations) {
        return false;
    }
    return std::any_of(delegations->begin(), delegations->end(), [](const auto& item) { return !item.second.empty(); });
}
}  // namespace

namespace cs {

MultisetHash::MultisetHash() {
    reset();
}

MultisetHash::Element MultisetHash::toElement(const cs::Bytes& item) {
    std::array<uint8_t, crypto_core_ristretto255_HASHBYTES> hash;
    crypto_generichash(hash.data(), hash.size(), item.data(), item.size(), nullptr, 0);

    Element element;
    crypto_core_ristretto255_from_hash(element.data(), hash.data());
    return element;
}

void MultisetHash::add(const cs::Bytes& item) {
    add(toElement(item));
}

void MultisetHash::remove(const cs::Bytes& item) {
    remove(toElement(item));
}

void MultisetHash::add(const Element& element) {
    crypto_core_ristretto255_add(sum_.data(), sum_.data(), element.data());
}

void MultisetHash::remove(const Element& element) {
    crypto_core_ristretto255_sub(sum_.data(), sum_.data(), element.data());
}

void MultisetHash::reset() {
    sum_.fill(0);  // the identity element of ristretto255
}

cs::Bytes serializeWalletState(const WalletsCache::WalletData& wallet) {
    const bool hasState = wallet.balance_ != csdb::Amount{0} || wallet.delegated_ != csdb::Amount{0} ||
                          hasEntries(wallet.delegateSources_) || hasEntries(wallet.delegateTargets_);
    if (!hasState) {
        return {};
    }

    cs::Bytes out;
    putU8(out, 1);  // format version
    putKey(out, wallet.key_);
    putAmount(out, wallet.balance_);
    putAmount(out, wallet.delegated_);
    putDelegations(out, wallet.delegateSources_);
    putDelegations(out, wallet.delegateTargets_);
    return out;
}

}  // namespace cs
