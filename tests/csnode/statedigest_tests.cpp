#include <gtest/gtest.h>

#include <csnode/nodecore.hpp>
#include <csnode/statedigest.hpp>

namespace {
cs::Bytes item(uint8_t b) {
    return cs::Bytes(16, b);
}

cs::WalletsCache::WalletData wallet(uint8_t keyByte, int32_t balance) {
    cs::WalletsCache::WalletData w;
    w.key_.fill(keyByte);
    w.balance_ = csdb::Amount{ balance };
    return w;
}
}  // namespace

TEST(MultisetHash, OrderIndependent) {
    cs::MultisetHash ab;
    ab.add(item(1));
    ab.add(item(2));

    cs::MultisetHash ba;
    ba.add(item(2));
    ba.add(item(1));

    ASSERT_EQ(ab.digest(), ba.digest());
}

TEST(MultisetHash, RemoveUndoesAdd) {
    cs::MultisetHash empty;
    cs::MultisetHash h;
    h.add(item(1));
    ASSERT_NE(h.digest(), empty.digest());

    h.add(item(2));
    h.remove(item(2));
    cs::MultisetHash one;
    one.add(item(1));
    ASSERT_EQ(h.digest(), one.digest());

    h.remove(item(1));
    ASSERT_EQ(h.digest(), empty.digest());
}

TEST(MultisetHash, DifferentSetsDiffer) {
    cs::MultisetHash a;
    a.add(item(1));
    cs::MultisetHash b;
    b.add(item(2));
    ASSERT_NE(a.digest(), b.digest());
}

TEST(WalletState, EmptyWalletHasNoState) {
    ASSERT_TRUE(cs::serializeWalletState(wallet(1, 0)).empty());
    ASSERT_FALSE(cs::serializeWalletState(wallet(1, 5)).empty());
}

TEST(WalletState, BalanceChangesSerialisation) {
    ASSERT_NE(cs::serializeWalletState(wallet(1, 5)), cs::serializeWalletState(wallet(1, 6)));
    ASSERT_NE(cs::serializeWalletState(wallet(1, 5)), cs::serializeWalletState(wallet(2, 5)));
}

// the order of delegation entries depends on apply/revert history, the digest must not
TEST(WalletState, DelegationOrderDoesNotMatter) {
    cs::PublicKey other{};
    other.fill(9);

    auto w1 = wallet(1, 5);
    w1.delegateTargets_ = std::make_shared<std::map<cs::PublicKey, std::vector<cs::TimeMoney>>>();
    (*w1.delegateTargets_)[other] = { cs::TimeMoney(1'000'000, 2'000, csdb::Amount{ 1 }), cs::TimeMoney(1'000'000, 3'000, csdb::Amount{ 2 }) };

    auto w2 = wallet(1, 5);
    w2.delegateTargets_ = std::make_shared<std::map<cs::PublicKey, std::vector<cs::TimeMoney>>>();
    (*w2.delegateTargets_)[other] = { cs::TimeMoney(1'000'000, 3'000, csdb::Amount{ 2 }), cs::TimeMoney(1'000'000, 2'000, csdb::Amount{ 1 }) };

    ASSERT_EQ(cs::serializeWalletState(w1), cs::serializeWalletState(w2));
}
