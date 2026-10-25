#ifndef SNAPSHOT_SERVICE_HPP
#define SNAPSHOT_SERVICE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <csnode/snapshot_server.hpp>
#include <lib/system/common.hpp>

class BlockChain;

namespace cs {

// Snapshots served by this node, in the node: every 10 s it reads [snapshot] from the configuration
// and starts, stops or moves the HTTP service (serve_port 0: off); while it serves, it builds the
// snapshot of each new periodic checkpoint into snapshot/ at low priority (and of the latest one
// when it starts serving), and serves it once complete. Only a node synced with the network serves.
class SnapshotService {
public:
    SnapshotService(BlockChain& blockchain, const cs::PrivateKey& key, std::string nodeVersion);
    ~SnapshotService();

    void start();
    void stop();
    // slot of BlockChain::checkpointSavedEvent: a new periodic checkpoint to build from
    void onCheckpointSaved(cs::Sequence sequence);

private:
    void run();
    void buildLatest();

    BlockChain& blockchain_;
    cs::PrivateKey key_;
    std::string nodeVersion_;
    std::filesystem::path root_ = "snapshot";
    std::unique_ptr<SnapshotServer> server_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> buildPending_{true};
    static constexpr int kMaxBuildAttempts = 5;
    int failedBuilds_ = 0;
    std::chrono::steady_clock::time_point retryAt_{};
    std::mutex m_;
    std::condition_variable cv_;
};

}  // namespace cs

#endif  // SNAPSHOT_SERVICE_HPP
