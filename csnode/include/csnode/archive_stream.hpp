#ifndef ARCHIVE_STREAM_HPP
#define ARCHIVE_STREAM_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <streambuf>
#include <vector>

namespace cs {

// The input of a boost binary_iarchive that reads a checkpoint written on another system.
//
// A binary_oarchive header records the native sizes of int, long, float and double, and
// binary_iarchive refuses a header whose sizes differ from its own: long is 8 bytes on Linux and
// macOS, 4 on Windows, so a checkpoint (or a snapshot made of one) from one system did not load on
// the other. No checkpoint field is a long: 64-bit fields are int64_t/uint64_t, written as 8 bytes
// on every system, and boost writes its own counts as 8-byte size_t on 64-bit systems. The size of
// long in the header is the only difference, and it is read here as this system's own.
//
// Header of boost 1.83: size_t length (22) | "serialization::archive" | uint16 library version |
// sizeof(int) | sizeof(long) | sizeof(float) | sizeof(double) | int 1 (byte order). Anything else is
// passed through unchanged, for binary_iarchive to accept or refuse as before.
class ArchiveInputBuf : public std::streambuf {
public:
    explicit ArchiveInputBuf(std::streambuf* source)
    : source_(source) {
        head_.resize(kHeaderSize);
        head_.resize(static_cast<size_t>(source_->sgetn(head_.data(), static_cast<std::streamsize>(kHeaderSize))));
        patchHeader();
        setg(head_.data(), head_.data(), head_.data() + head_.size());
    }

    static constexpr size_t kSignatureSize = 22;
    static constexpr size_t kLongSizeOffset = sizeof(uint64_t) + kSignatureSize + sizeof(uint16_t) + 1;
    static constexpr size_t kHeaderSize = kLongSizeOffset + 3 + sizeof(int32_t);

protected:
    int_type underflow() override {
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }
        buffer_.resize(kChunk);
        const auto n = source_->sgetn(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        if (n <= 0) {
            return traits_type::eof();
        }
        setg(buffer_.data(), buffer_.data(), buffer_.data() + n);
        return traits_type::to_int_type(*gptr());
    }

private:
    static constexpr size_t kChunk = 1 << 16;

    void patchHeader() {
        if (head_.size() < kHeaderSize) {
            return;
        }
        uint64_t length = 0;
        for (size_t i = 0; i < sizeof(length); ++i) {
            length |= static_cast<uint64_t>(static_cast<unsigned char>(head_[i])) << (8 * i);
        }
        static const char kSignature[] = "serialization::archive";
        if (length != kSignatureSize || std::memcmp(head_.data() + sizeof(uint64_t), kSignature, kSignatureSize) != 0) {
            return;
        }
        auto byteAt = [this](size_t i) { return static_cast<unsigned char>(head_[i]); };
        const bool sizesAsExpected = byteAt(kLongSizeOffset - 1) == sizeof(int32_t) && byteAt(kLongSizeOffset + 1) == sizeof(float) &&
                                     byteAt(kLongSizeOffset + 2) == sizeof(double);
        const auto longSize = byteAt(kLongSizeOffset);
        if (sizesAsExpected && (longSize == 4 || longSize == 8)) {
            head_[kLongSizeOffset] = static_cast<char>(sizeof(long));
        }
    }

    std::streambuf* source_;
    std::vector<char> head_;
    std::vector<char> buffer_;
};

// usage: std::ifstream ifs(path, std::ios::binary); cs::ArchiveInput in(ifs); boost::archive::binary_iarchive ia(in);
class ArchiveInput : public std::istream {
public:
    explicit ArchiveInput(std::istream& source)
    : std::istream(nullptr)
    , buf_(source.rdbuf()) {
        rdbuf(&buf_);
        if (!source) {
            setstate(std::ios::badbit);
        }
    }

private:
    ArchiveInputBuf buf_;
};

// The output of a boost binary_oarchive for a checkpoint: the header records long as 8 bytes on
// every system, so that a checkpoint (and a snapshot made of one) has the same bytes on Windows as
// on Linux and macOS, and nodes without ArchiveInput read it too. Only that header byte changes.
class ArchiveOutputBuf : public std::streambuf {
public:
    explicit ArchiveOutputBuf(std::streambuf* target)
    : target_(target) {
    }

protected:
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) {
            return traits_type::not_eof(c);
        }
        const char ch = traits_type::to_char_type(c);
        return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (n <= 0) {
            return 0;
        }
        const auto count = static_cast<size_t>(n);
        if (written_ > ArchiveInputBuf::kLongSizeOffset) {
            return put(s, n);
        }
        std::vector<char> copy(s, s + count);
        for (size_t i = 0; i < count && written_ + i <= ArchiveInputBuf::kLongSizeOffset; ++i) {
            const auto at = written_ + i;
            if (at < ArchiveInputBuf::kLongSizeOffset) {
                head_[at] = copy[i];
            }
            else if (signatureWritten() && (copy[i] == 4 || copy[i] == 8)) {
                copy[i] = 8;
            }
        }
        return put(copy.data(), n);
    }

    int sync() override {
        return target_->pubsync();
    }

private:
    std::streamsize put(const char* s, std::streamsize n) {
        const auto done = target_->sputn(s, n);
        written_ += static_cast<size_t>(done > 0 ? done : 0);
        return done;
    }

    bool signatureWritten() const {
        static const char kSignature[] = "serialization::archive";
        for (size_t i = 1; i < sizeof(uint64_t); ++i) {
            if (head_[i] != 0) {
                return false;
            }
        }
        return static_cast<unsigned char>(head_[0]) == ArchiveInputBuf::kSignatureSize &&
               std::memcmp(head_.data() + sizeof(uint64_t), kSignature, ArchiveInputBuf::kSignatureSize) == 0 &&
               static_cast<unsigned char>(head_[ArchiveInputBuf::kLongSizeOffset - 1]) == sizeof(int32_t);
    }

    std::streambuf* target_;
    size_t written_ = 0;
    std::array<char, ArchiveInputBuf::kLongSizeOffset> head_{};
};

// usage: std::ofstream ofs(path, std::ios::binary); cs::ArchiveOutput out(ofs); boost::archive::binary_oarchive oa(out);
class ArchiveOutput : public std::ostream {
public:
    explicit ArchiveOutput(std::ostream& target)
    : std::ostream(nullptr)
    , buf_(target.rdbuf()) {
        rdbuf(&buf_);
        if (!target) {
            setstate(std::ios::badbit);
        }
    }

    ~ArchiveOutput() override {
        flush();
    }

private:
    ArchiveOutputBuf buf_;
};

}  // namespace cs

#endif  // ARCHIVE_STREAM_HPP
