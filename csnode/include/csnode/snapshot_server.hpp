#ifndef SNAPSHOT_SERVER_HPP
#define SNAPSHOT_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <csnode/snapshot_manifest.hpp>
#include <lib/system/common.hpp>

namespace cs {

// The read-only HTTP service of the snapshots this node built (docs/specs/2026-10-24-snapshot-serving-design.md,
// section 5): GET and HEAD of /snapshot/latest.json and /snapshot/<seq>/<name>, one Range of at most
// 16 MiB, at most maxConnections connections, a bandwidth cap shared by all of them. It answers 503
// while the node is not synced with the network or no snapshot is built (only synced nodes serve).
// Files are looked up by the names of the manifest it loaded, never by a path from the request.
class SnapshotServer {
public:
    struct Settings {
        std::string bind;              // empty: every interface
        uint16_t port = 0;             // 0: stopped
        size_t maxConnections = 8;
        size_t rateKBps = 0;           // kilobytes a second, all connections together; 0: unlimited
        bool operator==(const Settings& o) const {
            return bind == o.bind && port == o.port && maxConnections == o.maxConnections && rateKBps == o.rateKBps;
        }
    };
    // the node's last block and the current round: synced when the block is within 3 rounds
    using SyncedTo = std::function<std::pair<cs::Sequence, cs::Sequence>()>;

    static constexpr uint64_t kMaxRange = 16ull << 20;
    static constexpr size_t kMaxRequestsPerConnection = 64;
    static constexpr int kIdleTimeoutSec = 15;
    static constexpr int kWriteTimeoutSec = 30;
    static constexpr cs::Sequence kSyncedRounds = 3;

    SnapshotServer(std::filesystem::path root, SyncedTo syncedTo);
    ~SnapshotServer();

    // starts, stops (port 0), moves to another address or applies new limits, while it runs
    void apply(const Settings& settings);
    // the newest complete snapshot under root (one holding a snapshot.json that verifies)
    void reload();
    void stop();

    uint16_t port() const {
        return boundPort_.load();
    }
    bool running() const {
        return boundPort_.load() != 0;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<uint16_t> boundPort_{0};
};

}  // namespace cs

#endif  // SNAPSHOT_SERVER_HPP
