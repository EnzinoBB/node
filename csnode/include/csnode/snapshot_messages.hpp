#ifndef SNAPSHOT_MESSAGES_HPP
#define SNAPSHOT_MESSAGES_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include <lib/system/common.hpp>

// Payloads of the four p2p snapshot messages (MsgTypes 40-43,
// docs/specs/2026-10-25-snapshot-p2p-design.md, section 4.2). Integers are little endian, strings
// and byte arrays carry a u64 length first, as cs::ODataStream writes them. decode() checks the
// payload size against the message's limit before reading anything, every length against what is
// left, and refuses trailing bytes, unknown status values and names outside the snapshot's file
// table.
namespace cs::snapshot_p2p {

constexpr uint8_t kProtocol = 1;

constexpr size_t kMaxName = 64;
constexpr size_t kMaxManifestJson = 256 * 1024;
constexpr size_t kMaxPieceBytes = 1024 * 1024;

constexpr size_t kMaxManifestRequest = 16;
constexpr size_t kMaxManifestReply = kMaxManifestJson + 64;
constexpr size_t kMaxPieceRequest = 128;
constexpr size_t kMaxPieceReply = kMaxPieceBytes + 128;

struct ManifestRequest {
    uint8_t protocol = kProtocol;
};

enum class ManifestStatus : uint8_t {
    Ready = 0,
    NotServing = 1,
    NotSynced = 2,
    Building = 3
};

struct ManifestReply {
    ManifestStatus status = ManifestStatus::NotServing;
    uint64_t lastSeq = 0;           // the sender's last block
    uint64_t round = 0;             // the sender's current round
    bool servingTrusted = false;    // the sender is a confidant of the current round
    std::string manifestJson;       // snapshot.json, only when Ready
};

struct PieceRequest {
    uint64_t seq = 0;               // the snapshot's checkpoint block
    std::string name;               // a file of the snapshot
    uint32_t chunk = 0;             // index of the manifest chunk
};

enum class PieceStatus : uint8_t {
    Ok = 0,
    Unknown = 1,                    // no such snapshot, file or chunk
    Busy = 2,                       // ask again after retryMs
    NotServing = 3
};

struct PieceReply {
    PieceStatus status = PieceStatus::NotServing;
    uint32_t retryMs = 0;           // only meaningful when Busy
    uint64_t seq = 0;
    std::string name;
    uint32_t chunk = 0;
    cs::Bytes data;                 // the chunk's bytes, only when Ok
};

cs::Bytes encode(const ManifestRequest& message);
cs::Bytes encode(const ManifestReply& message);
cs::Bytes encode(const PieceRequest& message);
cs::Bytes encode(const PieceReply& message);

bool decode(const uint8_t* data, size_t size, ManifestRequest& message);
bool decode(const uint8_t* data, size_t size, ManifestReply& message);
bool decode(const uint8_t* data, size_t size, PieceRequest& message);
bool decode(const uint8_t* data, size_t size, PieceReply& message);

}  // namespace cs::snapshot_p2p

#endif  // SNAPSHOT_MESSAGES_HPP
