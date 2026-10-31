#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <config.hpp>
#include <net/hostdata.hpp>

TEST(HostData, OfficialFieldsUnchanged) {
    const uint64_t data = cs::hostdata::encode(3, 532, cs::hostdata::kSnapshotProtocolV1, 13);
    // the official decoders read bits 0-7 and 8-23 only
    EXPECT_EQ(static_cast<uint8_t>(data), 3);
    EXPECT_EQ(static_cast<uint16_t>(data >> 8), 532);
    EXPECT_EQ(cs::hostdata::platform(data), 3);
    EXPECT_EQ(cs::hostdata::version(data), 532);
    EXPECT_EQ(data & 0xffffffu, cs::hostdata::encode(3, 532, 0, 0));
}

TEST(HostData, CapabilityAndMinorVersion) {
    const uint64_t data = cs::hostdata::encode(0, 532, cs::hostdata::kSnapshotProtocolV1, 13);
    EXPECT_EQ(cs::hostdata::capabilities(data), cs::hostdata::kSnapshotProtocolV1);
    EXPECT_EQ(cs::hostdata::minorVersion(data), 13);
    EXPECT_EQ((data >> 24) & 0xff, 1u);
    EXPECT_EQ(data >> 48, 0u);

    const uint64_t full = cs::hostdata::encode(0xff, 0xffff, 0xff, 0xffff);
    EXPECT_EQ(full, 0x0000ffffffffffffull);
    EXPECT_EQ(cs::hostdata::minorVersion(full), 0xffff);
}

TEST(HostData, OfficialNodeAnnouncesNothing) {
    const uint64_t official = cs::hostdata::encode(1, 532, 0, 0);
    EXPECT_EQ(cs::hostdata::capabilities(official), 0);
    EXPECT_EQ(cs::hostdata::minorVersion(official), 0);
}

TEST(HostData, KnownPeersJson) {
    EXPECT_EQ(Config::knownPeersJson({}), "[]\n");

    cs::PeerData a;
    a.id = "3rUevsW5xfob6qDxWMDFwwTQCq39SYhzstuyfUGSDvF2";
    a.ip = "10.0.0.1";
    a.port = 6000;
    a.version = 532;
    a.platform = 1;
    a.capabilities = 1;
    a.minorVersion = 13;
    cs::PeerData b;
    b.id = "ESWr7ZThCmP2DVNNgrcsCvZZ2rrhDnFRW8oUFEpKpgdD";
    b.ip = "10.0.0.2";
    b.port = 6001;
    EXPECT_EQ(Config::knownPeersJson({a, b}),
              "[\n"
              " {\"id\":\"3rUevsW5xfob6qDxWMDFwwTQCq39SYhzstuyfUGSDvF2\",\"ip\":\"10.0.0.1\",\"port\":6000,\"version\":532,"
              "\"minor\":13,\"platform\":1,\"capabilities\":1},\n"
              " {\"id\":\"ESWr7ZThCmP2DVNNgrcsCvZZ2rrhDnFRW8oUFEpKpgdD\",\"ip\":\"10.0.0.2\",\"port\":6001,\"version\":0,"
              "\"minor\":0,\"platform\":-1,\"capabilities\":0}\n"
              "]\n");
}
