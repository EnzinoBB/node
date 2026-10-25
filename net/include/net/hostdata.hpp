#ifndef NET_HOSTDATA_HPP
#define NET_HOSTDATA_HPP

#include <cstdint>

// The 64-bit host_data a node announces in its p2p datagrams. Bits 0-23 are the official layout
// (platform, node version); bits 24-31 are capability flags and bits 32-47 the minor (community)
// version, both 0 on official nodes; 48-63 stay 0.
namespace cs::hostdata {

constexpr uint8_t kSnapshotProtocolV1 = 1;  // bit 24: answers snapshot requests (MsgTypes 40-43)

inline uint64_t encode(uint8_t platform, uint16_t version, uint8_t capabilities, uint16_t minorVersion) {
    return uint64_t(platform) | (uint64_t(version) << 8) | (uint64_t(capabilities) << 24) | (uint64_t(minorVersion) << 32);
}

inline uint8_t platform(uint64_t data) {
    return static_cast<uint8_t>(data);
}

inline uint16_t version(uint64_t data) {
    return static_cast<uint16_t>(data >> 8);
}

inline uint8_t capabilities(uint64_t data) {
    return static_cast<uint8_t>(data >> 24);
}

inline uint16_t minorVersion(uint64_t data) {
    return static_cast<uint16_t>(data >> 32);
}

}  // namespace cs::hostdata

#endif  // NET_HOSTDATA_HPP
