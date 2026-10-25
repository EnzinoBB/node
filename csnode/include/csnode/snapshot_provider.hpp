#ifndef SNAPSHOT_PROVIDER_HPP
#define SNAPSHOT_PROVIDER_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <csnode/snapshot_manifest.hpp>
#include <csnode/snapshot_messages.hpp>
#include <lib/system/common.hpp>

namespace cs {

// what the node is doing when a snapshot request arrives
struct SnapshotNodeState {
    cs::Sequence lastSeq = 0;
    cs::RoundNumber round = 0;
    bool synced = false;      // design section 5.2
    bool trusted = false;     // a confidant or the writer of the current round: half the budget
};

// Answers the snapshot requests of p2p neighbours from the newest snapshot this node built
// (docs/specs/2026-10-25-snapshot-p2p-design.md, section 5). Used on the node's processor thread;
// load() and unload() come from the snapshot service's thread. It keeps no state per requester
// beyond a table of the peers served in the last 30 s (at most 64), and every reply is bounded
// by a global token bucket and a per-peer one.
class SnapshotProvider {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    struct Settings {
        bool serve = false;
        size_t rateKBps = 2000;   // all peers together, KB/s; 0: unlimited
        size_t maxPeers = 8;      // peers served within 30 s
    };

    using NodeState = SnapshotNodeState;

    static constexpr size_t kMaxPeers = 64;
    static constexpr auto kPeerWindow = std::chrono::seconds(30);
    static constexpr auto kPieceWindow = std::chrono::milliseconds(100);
    static constexpr size_t kPiecesPerWindow = 2;
    static constexpr auto kManifestInterval = std::chrono::seconds(5);
    static constexpr uint64_t kBurstBytes = 4ull << 20;
    static constexpr uint32_t kMaxRetryMs = 5000;

    explicit SnapshotProvider(Clock clock = std::chrono::steady_clock::now);
    ~SnapshotProvider();

    // dir = snapshot/<S>: its snapshot.json must parse, verify, name S, and its files must have the
    // declared sizes; on failure the previous snapshot stays loaded
    bool load(const std::filesystem::path& dir, std::string* error = nullptr);
    void unload();
    std::optional<cs::Sequence> loadedSeq() const;

    // nullopt: no answer (the peer asked again too soon, or the budget is spent; it asks again)
    std::optional<snapshot_p2p::ManifestReply> onManifestRequest(const cs::PublicKey& sender, const Settings& settings,
                                                                  const NodeState& state);
    snapshot_p2p::PieceReply onPieceRequest(const cs::PublicKey& sender, const snapshot_p2p::PieceRequest& request,
                                            const Settings& settings, const NodeState& state);

    // a line about the traffic since the last call, empty when there was none
    std::string takeSummary();
    // a peer that just received the last byte of a whole snapshot: once per peer and snapshot
    std::function<void(const cs::PublicKey&, cs::Sequence, uint64_t bytes, std::chrono::seconds took)> onPeerCompleted;

private:
    struct Loaded;

    struct Bucket {
        double tokens = 0;
        std::chrono::steady_clock::time_point at{};
        // refills at rate bytes/s up to burst; true and the cost taken when there is enough
        bool take(double cost, double rate, double burst, std::chrono::steady_clock::time_point now, uint32_t& retryMs);
    };

    struct Peer {
        std::chrono::steady_clock::time_point lastSeen{};
        std::chrono::steady_clock::time_point lastPiece{};
        std::chrono::steady_clock::time_point firstPiece{};
        std::chrono::steady_clock::time_point windowStart{};
        size_t piecesInWindow = 0;
        std::chrono::steady_clock::time_point lastManifest{};
        bool manifestSent = false;
        Bucket bucket;
        bool bucketStarted = false;
        cs::Sequence seq = 0;
        uint64_t bytes = 0;      // of seq
        bool completed = false;
    };

    Peer& peer(const cs::PublicKey& key, std::chrono::steady_clock::time_point now);
    size_t activePeers(std::chrono::steady_clock::time_point now, const cs::PublicKey& except) const;

    Clock clock_;
    mutable std::mutex loadedLock_;
    std::shared_ptr<const Loaded> loaded_;

    // processor thread only
    std::map<cs::PublicKey, Peer> peers_;
    Bucket global_;
    bool globalStarted_ = false;
    uint64_t piecesServed_ = 0;
    uint64_t bytesServed_ = 0;
    uint64_t busyAnswers_ = 0;
    uint64_t manifestsServed_ = 0;
};

}  // namespace cs

#endif  // SNAPSHOT_PROVIDER_HPP
