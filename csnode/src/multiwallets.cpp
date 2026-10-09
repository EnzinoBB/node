#include "csnode/multiwallets.hpp"
#include "lib/system/common.hpp"
#include "csnode/nodecore.hpp"
#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>
#include <csnode/configholder.hpp>

bool cs::MultiWallets::contains(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& byKey = indexes_.get<Tags::ByPublicKey>();
    return byKey.find(key) != byKey.end();
}

size_t cs::MultiWallets::size() const {
    cs::Lock lock(mutex_);
    return indexes_.size();
}

csdb::Amount cs::MultiWallets::balance(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    auto it = keys.find(key);
    return it == keys.end() ? csdb::Amount(0) : it->balance_;
}

uint64_t cs::MultiWallets::transactionsCount(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    auto it = keys.find(key);
    return it == keys.end() ? 0 : it->transNum_;
}

#ifdef MONITOR_NODE
uint64_t cs::MultiWallets::createTime(const cs::PublicKey& key) const {
    cs::Lock lock(mutex_);

    auto& keys = indexes_.get<Tags::ByPublicKey>();
    auto it = keys.find(key);
    return it == keys.end() ? 0 : it->createTime_;
}
#endif

bool cs::MultiWallets::getWalletData(cs::MultiWallets::InternalData& data) const {
  cs::Lock lock(mutex_);

  auto& keys = indexes_.get<Tags::ByPublicKey>();

  auto it = keys.find(data.key_);
  if (it == keys.end()) {
    return false;
  }
  
  data = *it;
  return true;
}

void cs::MultiWallets::onWalletCacheUpdated(const cs::WalletsCache::WalletData& data) {
    //csdebug() << __func__;
    cs::Lock lock(mutex_);
    auto& byKey = indexes_.get<Tags::ByPublicKey>();
    const auto& conf = cs::ConfigHolder::instance().config();
    if (conf->getBalanceChangeFlag() && data.key_ == conf->getBalanceChangeKey()) {
        csdebug() << "Wallet updated: " 
	    << conf->getBalanceChangeAddress()
	    << ", balance: " << data.balance_.to_string() 
	    << ", delegated: " << data.delegated_.to_string(); 
    }

    InternalData stored = data;
    stored.hasStateElement_ = false;
    if (digestReady_) {
        const auto state = cs::serializeWalletState(stored);
        if (!state.empty()) {
            stored.stateElement_ = cs::MultisetHash::toElement(state);
            stored.hasStateElement_ = true;
        }
    }

    if (auto iter = byKey.find(data.key_); iter != byKey.end()) {
        // the stored element, not the stored record: delegation maps are shared and already updated in place
        if (digestReady_ && iter->hasStateElement_) {
            digest_.remove(iter->stateElement_);
        }
        byKey.replace(iter, stored);
    }
    else {
        indexes_.insert(stored);
    }

    if (stored.hasStateElement_) {
        digest_.add(stored.stateElement_);
    }
}

cs::MultisetHash::Digest cs::MultiWallets::stateDigest() const {
    cs::Lock lock(mutex_);

    if (!digestReady_) {
        digest_.reset();
        auto& byKey = indexes_.get<Tags::ByPublicKey>();
        for (auto it = byKey.begin(); it != byKey.end(); ++it) {
            const auto state = cs::serializeWalletState(*it);
            const bool hasState = !state.empty();
            const auto element = hasState ? cs::MultisetHash::toElement(state) : cs::MultisetHash::Element{};
            if (hasState) {
                digest_.add(element);
            }
            // the element is not a key of any index, so updating it in place keeps the container valid
            const_cast<InternalData&>(*it).stateElement_ = element;
            const_cast<InternalData&>(*it).hasStateElement_ = hasState;
        }
        digestReady_ = true;
    }

    return digest_.digest();
}

void cs::MultiWallets::iterate(std::function<bool(const PublicKey& key, const InternalData& data)> func) {
    cs::Lock lock(mutex_);
    for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
        if (!func(it->key_, *it)) {
            break;
        }
    }
}

csdb::Amount cs::MultiWallets::checkWallets() {
    cs::Lock lock(mutex_);
    csdb::Amount total{ 0 };
    for (auto it = indexes_.begin(); it != indexes_.end(); ++it) {
        total += it->balance_;
        total += it->delegated_;
    }
    return total;
}
