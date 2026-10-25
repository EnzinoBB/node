#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <csnode/snapshot_agreement.hpp>

namespace {

cs::PublicKey key(uint8_t n) {
    cs::PublicKey k{};
    k.fill(n);
    return k;
}

cs::SourceManifest source(uint8_t n, const std::string& address, cs::Sequence seq, const std::string& digest = "d1",
                          const std::string& head = "", bool trusted = false) {
    cs::SourceManifest s;
    s.key = key(n);
    s.address = address;
    s.trusted = trusted;
    s.manifest.seq = seq;
    s.manifest.headHash = head.empty() ? "h" + std::to_string(seq) : head;
    s.manifest.stateDigest = digest;
    s.manifest.settingsDigest = "s1";
    return s;
}

}  // namespace

TEST(SnapshotAgreement, NetPrefix) {
    EXPECT_EQ(cs::netPrefix("10.1.2.3"), "10.1.2");
    EXPECT_EQ(cs::netPrefix("10.1.2.3:6000"), "10.1.2");
    EXPECT_EQ(cs::netPrefix("::ffff:10.1.2.3"), "10.1.2");
    EXPECT_EQ(cs::netPrefix("2001:db8:1:2::1"), cs::netPrefix("2001:db8:1:ffff::7"));
    EXPECT_EQ(cs::netPrefix("[2001:db8:1:2::1]:6000"), cs::netPrefix("2001:db8:1::"));
    EXPECT_NE(cs::netPrefix("2001:db8:1::1"), cs::netPrefix("2001:db8:2::1"));
    EXPECT_EQ(cs::netPrefix("2001:db8:1::1"), "2001:db8:1::/48");
    EXPECT_EQ(cs::netPrefix(""), "");
    EXPECT_EQ(cs::netPrefix("not an address"), "");
}

TEST(SnapshotAgreement, Threshold) {
    std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000)};
    auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.shortfall, 1u);
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].independent, 1u);

    v.push_back(source(2, "10.0.2.1", 1000));
    r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 1000u);
    EXPECT_EQ(r.chosen->independent, 2u);
    EXPECT_EQ(r.shortfall, 0u);
    EXPECT_TRUE(r.groups[0].chosen);
}

TEST(SnapshotAgreement, MinSourcesOne) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000)};
    const auto r = cs::agree(v, 1);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->keys, std::vector<cs::PublicKey>{key(1)});
}

TEST(SnapshotAgreement, SameSlash24CountsOnce) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000), source(2, "10.0.1.200", 1000)};
    const auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.shortfall, 1u);
    ASSERT_EQ(r.groups.size(), 1u);
    EXPECT_EQ(r.groups[0].keys.size(), 2u);
    EXPECT_EQ(r.groups[0].independent, 1u);
}

TEST(SnapshotAgreement, TrustedKeysCountedSeparately) {
    // two trusted keys behind one /24 (an operator's own machines) count twice; an untrusted one there adds one
    std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000, "d1", "", true), source(2, "10.0.1.2", 1000, "d1", "", true)};
    auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->independent, 2u);
    v.push_back(source(3, "10.0.1.3", 1000));
    v.push_back(source(4, "10.0.1.4", 1000));
    r = cs::agree(v, 4);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.groups[0].independent, 3u);
    EXPECT_EQ(r.shortfall, 1u);
}

TEST(SnapshotAgreement, Ipv6Slash48) {
    std::vector<cs::SourceManifest> v{source(1, "2001:db8:1:1::1", 1000), source(2, "2001:db8:1:2::1", 1000)};
    auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    v.push_back(source(3, "2001:db8:2::1", 1000));
    r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->independent, 2u);
    EXPECT_EQ(r.chosen->keys.size(), 3u);
}

TEST(SnapshotAgreement, UnknownAddressesCountTogether) {
    const std::vector<cs::SourceManifest> v{source(1, "", 1000), source(2, "", 1000), source(3, "garbage", 1000)};
    const auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.groups[0].independent, 1u);
}

TEST(SnapshotAgreement, SameKeyCountsOnce) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000), source(1, "10.0.2.1", 1000)};
    const auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
}

TEST(SnapshotAgreement, MoreSupportWinsOverAHigherBlock) {
    // (a) two forged sources at a made-up later block against three honest ones
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000), source(2, "10.0.2.1", 1000), source(3, "10.0.3.1", 1000),
                                            source(4, "10.0.4.1", 2000, "forged"), source(5, "10.0.5.1", 2000, "forged")};
    const auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 1000u);
    EXPECT_EQ(r.chosen->independent, 3u);
    ASSERT_EQ(r.groups.size(), 2u);
    EXPECT_TRUE(r.groups[0].chosen);
    EXPECT_FALSE(r.groups[1].chosen);
    EXPECT_EQ(r.groups[1].seq, 2000u);
    EXPECT_NE(cs::describe(r.groups[1]).find("fewer independent sources"), std::string::npos);
}

TEST(SnapshotAgreement, HigherBlockBreaksATie) {
    // (b) equal support: the newer snapshot
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000), source(2, "10.0.2.1", 1000), source(4, "10.0.4.1", 2000),
                                            source(5, "10.0.5.1", 2000)};
    const auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 2000u);
    EXPECT_NE(cs::describe(r.groups[1]).find("lower block"), std::string::npos);
}

TEST(SnapshotAgreement, ConfiguredKeyWins) {
    // (c) a group holding a key of [snapshot] sources wins over one without, both above the threshold
    std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000, "a", "", true), source(2, "10.0.2.1", 1000, "a"),
                                      source(3, "10.0.3.1", 2000, "b"), source(4, "10.0.4.1", 2000, "b")};
    auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 1000u);
    EXPECT_TRUE(r.chosen->configured);
    EXPECT_NE(cs::describe(r.groups[1]).find("no configured source"), std::string::npos);
    // even when the other group has more support
    v.push_back(source(5, "10.0.5.1", 2000, "b"));
    r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 1000u);
    // but a configured key alone does not lift a group below min_sources
    r = cs::agree({source(1, "10.0.1.1", 1000, "a", "", true), source(3, "10.0.3.1", 2000, "b"), source(4, "10.0.4.1", 2000, "b")}, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 2000u);
}

TEST(SnapshotAgreement, OnlyTheLowerBlockReachesTheThreshold) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000), source(2, "10.0.2.1", 1000), source(3, "10.0.3.1", 2000)};
    const auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->seq, 1000u);
    EXPECT_EQ(r.groups.size(), 2u);
}

TEST(SnapshotAgreement, DigestMajorityInsideABlock) {
    // 3 of 4 declare one digest, the fourth another: the majority is chosen, the minority reported
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000, "good"), source(2, "10.0.2.1", 1000, "good"),
                                            source(3, "10.0.3.1", 1000, "good"), source(4, "10.0.4.1", 1000, "forged")};
    const auto r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->stateDigest, "good");
    EXPECT_EQ(r.chosen->independent, 3u);
    ASSERT_EQ(r.groups.size(), 2u);
    EXPECT_EQ(r.groups[1].stateDigest, "forged");
    EXPECT_FALSE(r.groups[1].chosen);
    EXPECT_NE(cs::describe(r.groups[1]).find("1 source"), std::string::npos);
}

TEST(SnapshotAgreement, TieIsNotAnAgreement) {
    std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 2000, "a"), source(2, "10.0.2.1", 2000, "a"),
                                      source(3, "10.0.3.1", 2000, "b"), source(4, "10.0.4.1", 2000, "b")};
    auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.shortfall, 0u);   // groups are large enough, they conflict
    EXPECT_NE(cs::describe(r.groups[0]).find("conflict"), std::string::npos);
    // a weaker group is not taken instead
    v.push_back(source(5, "10.0.5.1", 1000));
    v.push_back(source(6, "10.0.6.1", 1000));
    r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    // one more source for one side settles it
    v.push_back(source(7, "10.0.7.1", 2000, "a"));
    r = cs::agree(v, 2);
    ASSERT_TRUE(r.chosen);
    EXPECT_EQ(r.chosen->stateDigest, "a");
}

TEST(SnapshotAgreement, DifferentHeadsAreDifferentBlocks) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000, "d1", "x"), source(2, "10.0.2.1", 1000, "d1", "y")};
    const auto r = cs::agree(v, 2);
    EXPECT_FALSE(r.chosen);
    EXPECT_EQ(r.groups.size(), 2u);
    EXPECT_EQ(r.shortfall, 1u);
}

TEST(SnapshotAgreement, Shortfall) {
    const std::vector<cs::SourceManifest> v{source(1, "10.0.1.1", 1000)};
    EXPECT_EQ(cs::agree(v, 3).shortfall, 2u);
    EXPECT_EQ(cs::agree({}, 2).shortfall, 2u);
    EXPECT_TRUE(cs::agree({}, 2).groups.empty());
}
