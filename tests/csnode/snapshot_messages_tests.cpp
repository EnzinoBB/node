#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <string>

#include <csnode/snapshot_messages.hpp>
#include <net/packet.hpp>

using namespace cs::snapshot_p2p;

namespace {
template <typename T>
bool decodeBytes(const cs::Bytes& bytes, T& message) {
    return decode(bytes.data(), bytes.size(), message);
}

// every prefix of a valid payload is refused
template <typename T>
void expectTruncationsRefused(const cs::Bytes& bytes) {
    for (size_t size = 0; size < bytes.size(); ++size) {
        T message;
        EXPECT_FALSE(decode(bytes.data(), size, message)) << "accepted " << size << " of " << bytes.size() << " bytes";
    }
}

void putU64(cs::Bytes& bytes, size_t offset, uint64_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

const std::string kName = "blocks.bin.lz4";
}  // namespace

TEST(SnapshotMessages, ManifestRequestRoundTrip) {
    ManifestRequest in;
    in.protocol = 7;
    ManifestRequest out;
    ASSERT_TRUE(decodeBytes(encode(in), out));
    EXPECT_EQ(out.protocol, 7);
    expectTruncationsRefused<ManifestRequest>(encode(in));
}

TEST(SnapshotMessages, ManifestRequestVersions) {
    ManifestRequest out;
    cs::Bytes current{kProtocol};
    EXPECT_TRUE(decodeBytes(current, out));
    current.push_back(0);
    EXPECT_FALSE(decodeBytes(current, out));
    // a later protocol with more fields is read
    cs::Bytes later{2, 9, 9, 9};
    ASSERT_TRUE(decodeBytes(later, out));
    EXPECT_EQ(out.protocol, 2);
    cs::Bytes zero{0};
    EXPECT_FALSE(decodeBytes(zero, out));
}

TEST(SnapshotMessages, ManifestReplyRoundTrip) {
    ManifestReply in;
    in.status = ManifestStatus::Ready;
    in.lastSeq = std::numeric_limits<uint64_t>::max();
    in.round = 186'600'000;
    in.servingTrusted = true;
    in.manifestJson = "{\"format\":2}";
    ManifestReply out;
    ASSERT_TRUE(decodeBytes(encode(in), out));
    EXPECT_EQ(out.status, ManifestStatus::Ready);
    EXPECT_EQ(out.lastSeq, in.lastSeq);
    EXPECT_EQ(out.round, in.round);
    EXPECT_TRUE(out.servingTrusted);
    EXPECT_EQ(out.manifestJson, in.manifestJson);
    expectTruncationsRefused<ManifestReply>(encode(in));
}

TEST(SnapshotMessages, ManifestReplyWithoutManifest) {
    for (auto status : {ManifestStatus::NotServing, ManifestStatus::NotSynced, ManifestStatus::Building}) {
        ManifestReply in;
        in.status = status;
        in.lastSeq = 5;
        in.manifestJson = "ignored";
        ManifestReply out;
        ASSERT_TRUE(decodeBytes(encode(in), out));
        EXPECT_EQ(out.status, status);
        EXPECT_TRUE(out.manifestJson.empty());
    }
}

TEST(SnapshotMessages, ManifestReplyBadValuesRefused) {
    ManifestReply in;
    in.status = ManifestStatus::NotServing;
    auto bytes = encode(in);
    ManifestReply out;

    auto badStatus = bytes;
    badStatus[0] = 4;
    EXPECT_FALSE(decodeBytes(badStatus, out));

    auto badFlag = bytes;
    badFlag[17] = 2;
    EXPECT_FALSE(decodeBytes(badFlag, out));

    auto trailing = bytes;
    trailing.push_back(0);
    EXPECT_FALSE(decodeBytes(trailing, out));

    // ready with an empty manifest
    in.status = ManifestStatus::Ready;
    EXPECT_FALSE(decodeBytes(encode(in), out));
}

TEST(SnapshotMessages, ManifestJsonLimit) {
    ManifestReply in;
    in.status = ManifestStatus::Ready;
    in.manifestJson.assign(kMaxManifestJson, 'x');
    ManifestReply out;
    EXPECT_TRUE(decodeBytes(encode(in), out));
    in.manifestJson.push_back('x');
    EXPECT_FALSE(decodeBytes(encode(in), out));
}

TEST(SnapshotMessages, PieceRequestRoundTrip) {
    PieceRequest in;
    in.seq = 186'500'000;
    in.name = kName;
    in.chunk = 401;
    PieceRequest out;
    ASSERT_TRUE(decodeBytes(encode(in), out));
    EXPECT_EQ(out.seq, in.seq);
    EXPECT_EQ(out.name, in.name);
    EXPECT_EQ(out.chunk, in.chunk);
    expectTruncationsRefused<PieceRequest>(encode(in));
}

TEST(SnapshotMessages, PieceRequestNamesRefused) {
    PieceRequest in;
    PieceRequest out;
    in.name = "../../etc/passwd";
    EXPECT_FALSE(decodeBytes(encode(in), out));
    in.name = std::string(65, 'a');
    EXPECT_FALSE(decodeBytes(encode(in), out));
    in.name = "";
    EXPECT_FALSE(decodeBytes(encode(in), out));
    in.name = "contracts.bin.lz4";
    EXPECT_TRUE(decodeBytes(encode(in), out));
}

TEST(SnapshotMessages, PieceReplyRoundTrip) {
    PieceReply in;
    in.status = PieceStatus::Ok;
    in.seq = 3;
    in.name = kName;
    in.chunk = 0;
    in.data.assign(kMaxPieceBytes, 0xab);
    PieceReply out;
    ASSERT_TRUE(decodeBytes(encode(in), out));
    EXPECT_EQ(out.status, PieceStatus::Ok);
    EXPECT_EQ(out.data, in.data);
    EXPECT_EQ(out.name, kName);

    in.data.push_back(0);
    EXPECT_FALSE(decodeBytes(encode(in), out));

    in.data.clear();
    EXPECT_FALSE(decodeBytes(encode(in), out));
}

TEST(SnapshotMessages, PieceReplyBusy) {
    PieceReply in;
    in.status = PieceStatus::Busy;
    in.retryMs = 2500;
    in.seq = 9;
    in.name = kName;
    in.chunk = 12;
    in.data = {1, 2, 3};  // not sent unless ok
    PieceReply out;
    const auto bytes = encode(in);
    ASSERT_TRUE(decodeBytes(bytes, out));
    EXPECT_EQ(out.status, PieceStatus::Busy);
    EXPECT_EQ(out.retryMs, 2500u);
    EXPECT_EQ(out.chunk, 12u);
    EXPECT_TRUE(out.data.empty());
    expectTruncationsRefused<PieceReply>(bytes);

    auto badStatus = bytes;
    badStatus[0] = 4;
    EXPECT_FALSE(decodeBytes(badStatus, out));
}

TEST(SnapshotMessages, HugeLengthsDoNotWrap) {
    PieceReply in;
    in.status = PieceStatus::Ok;
    in.name = kName;
    in.data = {1, 2, 3, 4};
    auto bytes = encode(in);
    PieceReply out;
    // status(1) retry(4) seq(8) then the name's length
    auto name = bytes;
    putU64(name, 13, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(decodeBytes(name, out));
    // the data's length follows name and chunk
    auto data = bytes;
    putU64(data, 13 + 8 + kName.size() + 4, std::numeric_limits<uint64_t>::max() - 2);
    EXPECT_FALSE(decodeBytes(data, out));
}

TEST(SnapshotMessages, OversizedPayloadsRefusedBeforeParsing) {
    cs::Bytes big(kMaxPieceReply + 1, 0);
    PieceReply piece;
    EXPECT_FALSE(decodeBytes(big, piece));
    ManifestReply manifest;
    big.resize(kMaxManifestReply + 1);
    EXPECT_FALSE(decodeBytes(big, manifest));
    PieceRequest request;
    big.resize(kMaxPieceRequest + 1);
    EXPECT_FALSE(decodeBytes(big, request));
    ManifestRequest manifestRequest;
    big.resize(kMaxManifestRequest + 1);
    EXPECT_FALSE(decodeBytes(big, manifestRequest));
    EXPECT_FALSE(decode(nullptr, 0, manifestRequest));
}

TEST(SnapshotMessages, MessageTypeNames) {
    EXPECT_EQ(static_cast<int>(MsgTypes::SnapshotManifestRequest), 40);
    EXPECT_EQ(static_cast<int>(MsgTypes::SnapshotPieceReply), 43);
    EXPECT_STREQ(Packet::messageTypeToString(MsgTypes::SnapshotManifestRequest), "SnapshotManifestRequest");
    EXPECT_STREQ(Packet::messageTypeToString(MsgTypes::SnapshotManifestReply), "SnapshotManifestReply");
    EXPECT_STREQ(Packet::messageTypeToString(MsgTypes::SnapshotPieceRequest), "SnapshotPieceRequest");
    EXPECT_STREQ(Packet::messageTypeToString(MsgTypes::SnapshotPieceReply), "SnapshotPieceReply");
}
