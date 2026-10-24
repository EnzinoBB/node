#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

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

namespace {
cs::CheckpointDigest sampleCheckpointDigest() {
    cs::CheckpointDigest d;
    for (size_t i = 0; i < d.digest.size(); ++i) {
        d.digest[i] = static_cast<uint8_t>(i);
        d.settings[i] = static_cast<uint8_t>(255 - i);
    }
    d.wallets = 147463;
    return d;
}

std::filesystem::path tempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}
}  // namespace

TEST(CheckpointDigest, WriteReadRoundTrip) {
    const auto file = tempFile("state_digest_roundtrip.bin");
    const auto d = sampleCheckpointDigest();
    ASSERT_TRUE(d.write(file));
    const auto read = cs::CheckpointDigest::read(file);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, d);
    std::filesystem::remove(file);
}

TEST(CheckpointDigest, MissingFileReadsNothing) {
    EXPECT_FALSE(cs::CheckpointDigest::read(tempFile("state_digest_missing.bin")).has_value());
}

TEST(CheckpointDigest, TruncatedOrLongerFileRejected) {
    const auto file = tempFile("state_digest_truncated.bin");
    ASSERT_TRUE(sampleCheckpointDigest().write(file));
    const auto size = std::filesystem::file_size(file);
    std::filesystem::resize_file(file, size - 1);
    EXPECT_FALSE(cs::CheckpointDigest::read(file).has_value());
    std::filesystem::resize_file(file, size + 1);
    EXPECT_FALSE(cs::CheckpointDigest::read(file).has_value());
    std::filesystem::remove(file);
}

TEST(CheckpointDigest, UnknownVersionRejected) {
    const auto file = tempFile("state_digest_version.bin");
    ASSERT_TRUE(sampleCheckpointDigest().write(file));
    {
        std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
        const char version = 2;
        f.write(&version, 1);
    }
    EXPECT_FALSE(cs::CheckpointDigest::read(file).has_value());
    std::filesystem::remove(file);
}

TEST(CheckpointDigest, ComparesEveryField) {
    const auto d = sampleCheckpointDigest();
    auto other = d;
    other.wallets += 1;
    EXPECT_NE(d, other);
    other = d;
    other.settings[0] ^= 1;
    EXPECT_NE(d, other);
    other = d;
    other.digest[31] ^= 1;
    EXPECT_NE(d, other);
}

TEST(CheckpointDigest, LogLineHasTheScriptsFormat) {
    const auto d = sampleCheckpointDigest();
    const auto line = cs::stateDigestLine(186400000, d.digest, d.wallets);
    EXPECT_EQ(line.rfind("STATE DIGEST #186400000 000102", 0), 0u);
    EXPECT_EQ(line.substr(line.size() - std::string(" wallets 147463").size()), " wallets 147463");
    EXPECT_EQ(line.size(), std::string("STATE DIGEST #186400000 ").size() + 64 + std::string(" wallets 147463").size());
}
