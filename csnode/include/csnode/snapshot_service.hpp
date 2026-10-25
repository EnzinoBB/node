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

#include <csnode/snapshot_provider.hpp>
#include <lib/system/common.hpp>

class BlockChain;

namespace cs {

// Snapshots served by this node to its p2p neighbours: every 10 s it reads [snapshot] from the
// configuration; while serve is on, it builds the snapshot of each new periodic checkpoint into
// snapshot/ at low priority (and of the latest one when serving starts) and hands the newest complete
// one to the provider, which answers the requests; serve off unloads it. Whether the node is synced
// is the provider's question, asked per request.
class SnapshotService {
public:
    SnapshotService(BlockChain& blockchain, SnapshotProvider& provider, const cs::PrivateKey& key, std::string nodeVersion);
    ~SnapshotService();

    void start();
    void stop();
    // slot of BlockChain::checkpointSavedEvent: a new periodic checkpoint to build from
    void onCheckpointSaved(cs::Sequence sequence);

private:
    void run();
    void buildLatest();
    // the newest snapshot/<S> with a snapshot.json, into the provider
    void loadNewest();

    BlockChain& blockchain_;
    cs::PrivateKey key_;
    std::string nodeVersion_;
    std::filesystem::path root_ = "snapshot";
    SnapshotProvider& provider_;
    bool serving_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> buildPending_{true};
    static constexpr int kMaxBuildAttempts = 5;
    std::atomic<int> failedBuilds_{0};   // reset by onCheckpointSaved on the thread that stores blocks
    std::chrono::steady_clock::time_point retryAt_{};
    std::mutex m_;
    std::condition_variable cv_;
};

}  // namespace cs

#endif  // SNAPSHOT_SERVICE_HPP
