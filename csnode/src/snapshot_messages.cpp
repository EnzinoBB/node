#include <csnode/snapshot_messages.hpp>

#include <cstring>

#include <csnode/datastream.hpp>
#include <csnode/snapshot_manifest.hpp>

namespace cs::snapshot_p2p {

namespace {
// A small reader with an explicit limit for every field; every length is checked against the bytes
// left before anything is read or allocated.
class Reader {
public:
    Reader(const uint8_t* data, size_t size)
    : data_(data)
    , size_(size) {
    }

    template <typename T>
    bool value(T& out) {
        if (size_ - pos_ < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return true;
    }

    bool text(std::string& out, size_t limit) {
        uint64_t length = 0;
        if (!value(length) || length > limit || length > size_ - pos_) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(length));
        pos_ += static_cast<size_t>(length);
        return true;
    }

    bool bytes(cs::Bytes& out, size_t limit) {
        uint64_t length = 0;
        if (!value(length) || length > limit || length > size_ - pos_) {
            return false;
        }
        out.assign(data_ + pos_, data_ + pos_ + static_cast<size_t>(length));
        pos_ += static_cast<size_t>(length);
        return true;
    }

    bool atEnd() const {
        return pos_ == size_;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

bool knownName(const std::string& name) {
    return name.size() <= kMaxName && SnapshotManifest::isKnownFileName(name);
}
}  // namespace

cs::Bytes encode(const ManifestRequest& message) {
    cs::Bytes result;
    cs::ODataStream stream(result);
    stream << message.protocol;
    return result;
}

cs::Bytes encode(const ManifestReply& message) {
    cs::Bytes result;
    cs::ODataStream stream(result);
    stream << static_cast<uint8_t>(message.status) << message.lastSeq << message.round
           << static_cast<uint8_t>(message.servingTrusted ? 1 : 0);
    if (message.status == ManifestStatus::Ready) {
        stream << message.manifestJson;
    }
    return result;
}

cs::Bytes encode(const PieceRequest& message) {
    cs::Bytes result;
    cs::ODataStream stream(result);
    stream << message.seq << message.name << message.chunk;
    return result;
}

cs::Bytes encode(const PieceReply& message) {
    cs::Bytes result;
    cs::ODataStream stream(result);
    stream << static_cast<uint8_t>(message.status) << message.retryMs << message.seq << message.name << message.chunk;
    if (message.status == PieceStatus::Ok) {
        stream << message.data;
    }
    return result;
}

bool decode(const uint8_t* data, size_t size, ManifestRequest& message) {
    if (data == nullptr || size > kMaxManifestRequest) {
        return false;
    }
    Reader reader(data, size);
    ManifestRequest result;
    if (!reader.value(result.protocol) || result.protocol == 0) {
        return false;
    }
    // a later protocol may append fields: the request is still read, the reply says what this node speaks
    if (result.protocol == kProtocol && !reader.atEnd()) {
        return false;
    }
    message = result;
    return true;
}

bool decode(const uint8_t* data, size_t size, ManifestReply& message) {
    if (data == nullptr || size > kMaxManifestReply) {
        return false;
    }
    Reader reader(data, size);
    ManifestReply result;
    uint8_t status = 0;
    uint8_t trusted = 0;
    if (!reader.value(status) || status > static_cast<uint8_t>(ManifestStatus::Building) ||
        !reader.value(result.lastSeq) || !reader.value(result.round) || !reader.value(trusted) || trusted > 1) {
        return false;
    }
    result.status = static_cast<ManifestStatus>(status);
    result.servingTrusted = trusted == 1;
    if (result.status == ManifestStatus::Ready &&
        (!reader.text(result.manifestJson, kMaxManifestJson) || result.manifestJson.empty())) {
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }
    message = std::move(result);
    return true;
}

bool decode(const uint8_t* data, size_t size, PieceRequest& message) {
    if (data == nullptr || size > kMaxPieceRequest) {
        return false;
    }
    Reader reader(data, size);
    PieceRequest result;
    if (!reader.value(result.seq) || !reader.text(result.name, kMaxName) || !knownName(result.name) ||
        !reader.value(result.chunk) || !reader.atEnd()) {
        return false;
    }
    message = std::move(result);
    return true;
}

bool decode(const uint8_t* data, size_t size, PieceReply& message) {
    if (data == nullptr || size > kMaxPieceReply) {
        return false;
    }
    Reader reader(data, size);
    PieceReply result;
    uint8_t status = 0;
    if (!reader.value(status) || status > static_cast<uint8_t>(PieceStatus::NotServing) ||
        !reader.value(result.retryMs) || !reader.value(result.seq) || !reader.text(result.name, kMaxName) ||
        !knownName(result.name) || !reader.value(result.chunk)) {
        return false;
    }
    result.status = static_cast<PieceStatus>(status);
    if (result.status == PieceStatus::Ok && (!reader.bytes(result.data, kMaxPieceBytes) || result.data.empty())) {
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }
    message = std::move(result);
    return true;
}

}  // namespace cs::snapshot_p2p
