#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/archive/archive_exception.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>

#include <csnode/archive_stream.hpp>

namespace {
struct Data {
    uint64_t sequence = 0;
    int64_t signedValue = 0;
    std::map<std::string, int64_t> counts;
    std::vector<uint8_t> bytes;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar & sequence;
        ar & signedValue;
        ar & counts;
        ar & bytes;
    }
};

Data sample() {
    Data d;
    d.sequence = 186600000;
    d.signedValue = -1234567890123;
    d.counts = {{"a", 1}, {"contract", 1ll << 40}};
    d.bytes.assign(200000, 0x5a);   // more than one chunk of the reader
    d.bytes[123456] = 7;
    return d;
}

// an archive written here, with the header the other kind of system writes
std::string archiveWithLongSize(unsigned char longSize) {
    std::ostringstream out(std::ios::binary);
    {
        boost::archive::binary_oarchive oa(out);
        const auto d = sample();
        oa << d;
    }
    auto bytes = out.str();
    EXPECT_EQ(static_cast<unsigned char>(bytes[cs::ArchiveInputBuf::kLongSizeOffset]), sizeof(long));
    bytes[cs::ArchiveInputBuf::kLongSizeOffset] = static_cast<char>(longSize);
    return bytes;
}

void expectSample(const Data& d) {
    const auto want = sample();
    EXPECT_EQ(d.sequence, want.sequence);
    EXPECT_EQ(d.signedValue, want.signedValue);
    EXPECT_EQ(d.counts, want.counts);
    EXPECT_EQ(d.bytes, want.bytes);
}
}  // namespace

// a checkpoint from Windows (long of 4 bytes) on Linux/macOS, and from Linux/macOS on Windows
TEST(ArchiveInput, ReadsTheHeaderOfEitherSystem) {
    for (unsigned char longSize : {4, 8}) {
        std::istringstream source(archiveWithLongSize(longSize), std::ios::binary);
        cs::ArchiveInput in(source);
        boost::archive::binary_iarchive ia(in);
        Data d;
        ia >> d;
        expectSample(d);
    }
}

TEST(ArchiveInput, PlainReaderRefusesTheOtherSystem) {
    const unsigned char other = sizeof(long) == 8 ? 4 : 8;
    std::istringstream source(archiveWithLongSize(other), std::ios::binary);
    EXPECT_THROW(boost::archive::binary_iarchive ia(source), boost::archive::archive_exception);
}

// other sizes than the two systems' are left for binary_iarchive to refuse
TEST(ArchiveInput, OtherHeadersPassUnchanged) {
    std::istringstream source(archiveWithLongSize(2), std::ios::binary);
    cs::ArchiveInput in(source);
    EXPECT_THROW(boost::archive::binary_iarchive ia(in), boost::archive::archive_exception);

    std::istringstream garbage(std::string("not an archive at all, just bytes"), std::ios::binary);
    cs::ArchiveInput in2(garbage);
    EXPECT_ANY_THROW(boost::archive::binary_iarchive ia(in2));   // as without ArchiveInput

    std::istringstream empty(std::string(), std::ios::binary);
    cs::ArchiveInput in3(empty);
    EXPECT_ANY_THROW(boost::archive::binary_iarchive ia(in3));
}

// written through ArchiveOutput, the header says long is 8 bytes on every system and the rest is unchanged
TEST(ArchiveOutput, WritesTheSameHeaderOnEverySystem) {
    std::ostringstream plain(std::ios::binary);
    {
        boost::archive::binary_oarchive oa(plain);
        const auto d = sample();
        oa << d;
    }
    std::ostringstream canonical(std::ios::binary);
    {
        cs::ArchiveOutput out(canonical);
        boost::archive::binary_oarchive oa(out);
        const auto d = sample();
        oa << d;
    }
    auto expected = plain.str();
    expected[cs::ArchiveInputBuf::kLongSizeOffset] = 8;
    EXPECT_EQ(canonical.str(), expected);

    std::istringstream source(canonical.str(), std::ios::binary);
    cs::ArchiveInput in(source);
    boost::archive::binary_iarchive ia(in);
    Data d;
    ia >> d;
    expectSample(d);
}

// the header of a Windows node (long of 4 bytes), written byte by byte or at once
TEST(ArchiveOutput, RewritesAFourByteLong) {
    const auto windows = archiveWithLongSize(4);
    for (bool byByte : {false, true}) {
        std::ostringstream target(std::ios::binary);
        {
            cs::ArchiveOutput out(target);
            if (byByte) {
                for (char c : windows) {
                    out.put(c);
                }
            }
            else {
                out.write(windows.data(), static_cast<std::streamsize>(windows.size()));
            }
        }
        auto expected = windows;
        expected[cs::ArchiveInputBuf::kLongSizeOffset] = 8;
        EXPECT_EQ(target.str(), expected);
    }
    // not an archive header: passed through
    std::ostringstream target(std::ios::binary);
    {
        cs::ArchiveOutput out(target);
        out << std::string(40, '\x04');
    }
    EXPECT_EQ(target.str(), std::string(40, '\x04'));
}
