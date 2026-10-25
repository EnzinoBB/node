#ifndef SNAPSHOT_AGREEMENT_HPP
#define SNAPSHOT_AGREEMENT_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <csnode/snapshot_manifest.hpp>
#include <lib/system/common.hpp>

// Agreement of independent sources on a snapshot (docs/specs/2026-10-25-snapshot-p2p-design.md,
// section 6.3): pure functions over the manifests collected from the p2p neighbours.
namespace cs {

// a manifest as received from one source: its p2p key, its address in the routing table and
// whether the operator listed the key in [snapshot] sources
struct SourceManifest {
    cs::PublicKey key{};
    std::string address;
    bool trusted = false;
    SnapshotManifest manifest;
};

// The independence unit of an address: the /24 of an IPv4 address ("a.b.c"), the /48 of an
// IPv6 one ("2001:db8:1::/48"); an IPv4-mapped IPv6 address counts as IPv4. A port after the
// address (a.b.c.d:port, [v6]:port) is ignored. Empty for an address that does not parse.
std::string netPrefix(const std::string& address);
// the wider block around it: /16 for IPv4, /32 for IPv6; empty when the address does not parse
std::string netBlock(const std::string& address);

// sources declaring one (seq, head_hash, state_digest, settings_digest)
struct AgreementGroup {
    cs::Sequence seq = 0;
    std::string headHash;
    std::string stateDigest;
    std::string settingsDigest;
    std::vector<size_t> sources;          // indices into agree()'s input
    std::vector<cs::PublicKey> keys;      // their keys, same order
    size_t independent = 0;               // counting sources: one per /24 or /48, trusted keys once each
    bool configured = false;              // holds a key of [snapshot] sources
    bool chosen = false;
    std::string reason;                   // why it was not chosen
};

using GroupReport = AgreementGroup;

struct AgreementResult {
    std::optional<AgreementGroup> chosen;
    std::vector<AgreementGroup> groups;   // every group, strongest first
    size_t shortfall = 0;                 // counting sources the best group lacks (0 when chosen)
};

// Groups by (seq, head_hash, state_digest, settings_digest). Among the groups with at least
// minSources counting sources, the chosen one is, in this order: a group holding a key of
// [snapshot] sources (trusted), then the group with most counting sources, then the highest seq
// (only a tie-breaker: a later block never wins over more support, so that min_sources forged
// sources at a made-up later block do not beat a larger honest group; an honest network's newest
// snapshot gains sources as the nodes rebuild, and the fetcher asks again every 30 s). Two groups
// equal on all three are a conflict: nothing is chosen. Sources are counted by netPrefix(address),
// trusted ones by key; a source whose address is unknown or does not parse does not count; a key that
// appears twice counts once. Every group that is not chosen carries the reason.
AgreementResult agree(const std::vector<SourceManifest>& sources, size_t minSources);

// "block S hash H digest D : N sources (keys k1 k2 …)" with shortened hashes and keys
std::string describe(const AgreementGroup& group);

}  // namespace cs

#endif  // SNAPSHOT_AGREEMENT_HPP
