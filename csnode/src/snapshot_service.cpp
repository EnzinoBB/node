#include <csnode/snapshot_service.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>

#include <lib/system/platform.hpp>

#include <csnode/blockchain.hpp>
#include <csnode/configholder.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/statedigest.hpp>
#include <lib/system/logger.hpp>

namespace cs {

namespace {
namespace fs = std::filesystem;
constexpr auto kPeriod = std::chrono::seconds(10);

bool isDigits(const std::string& s) {
    return !s.empty() && s.size() <= 12 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// the highest periodic checkpoint that records its state (qs/0, the shutdown one, is not periodic)
cs::Sequence latestCheckpoint() {
    cs::Sequence best = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("qs", ec)) {
        const auto name = entry.path().filename().string();
        if (isDigits(name) && fs::exists(entry.path() / CheckpointDigest::kFileName, ec)) {
            best = std::max<cs::Sequence>(best, std::stoull(name));
        }
    }
    return best;
}
}  // namespace

SnapshotService::SnapshotService(BlockChain& blockchain, SnapshotProvider& provider, const cs::PrivateKey& key, std::string nodeVersion)
: blockchain_(blockchain), key_(key), nodeVersion_(std::move(nodeVersion)), provider_(provider) {
}

SnapshotService::~SnapshotService() {
    stop();
}

void SnapshotService::start() {
    stop_ = false;
    thread_ = std::thread([this]() { run(); });
}

void SnapshotService::stop() {
    if (!thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    provider_.unload();
}

void SnapshotService::onCheckpointSaved(cs::Sequence sequence) {
    (void)sequence;
    {
        // under the lock: the service thread cannot miss it between its check and its wait
        std::lock_guard<std::mutex> lock(m_);
        failedBuilds_ = 0;
        buildPending_ = true;
    }
    cv_.notify_all();
}

void SnapshotService::run() {
    // the builder works on this thread: below the node's other work
    cs::platform::lowerThreadPriority();
    while (!stop_) {
        const bool serve = cs::ConfigHolder::instance().config()->getSnapshotSettings().serve;   // the observer may swap the config
        if (serve != serving_) {
            serving_ = serve;
            if (serve) {
                loadNewest();
                buildPending_ = true;
            }
            else {
                provider_.unload();
                cslog() << "SNAPSHOT: not serving ([snapshot] serve=off)";
            }
        }
        if (serve && retryAt_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= retryAt_) {
            retryAt_ = {};
            buildPending_ = true;
        }
        if (serve && buildPending_.exchange(false)) {
            buildLatest();
        }
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, kPeriod, [this]() { return stop_.load() || (serving_ && buildPending_.load()); });
    }
}

void SnapshotService::buildLatest() {
    const cs::Sequence S = latestCheckpoint();
    std::error_code ec;
    if (S == 0 || fs::exists(root_ / std::to_string(S) / "snapshot.json", ec)) {
        return;   // nothing new: no checkpoint with a digest yet, or already built
    }
    const auto cfg = cs::ConfigHolder::instance().config()->getSnapshotSettings();
    SnapshotBuildInput in;
    in.seq = S;
    in.checkpointDir = fs::path("qs") / std::to_string(S);
    // pruning may be removing the lowest blocks right now (it follows the same checkpoint): start above it
    in.firstStored = std::max(blockchain_.getFirstStoredSequence(), blockchain_.pruneTarget());
    in.snapshotBlocks = cfg.snapshotBlocks;
    in.nodeVersion = nodeVersion_;
    in.stop = &stop_;
    in.blockBytes = [this](cs::Sequence seq, cs::Bytes& out) {
        return blockchain_.blockBytes(seq, out) && !out.empty();
    };
    // no contract records: the contracts database holds the latest state of each contract, later than
    // S, while the signed snapshot is the state at S; the checkpoint carries the states at S
    // (smartcontracts.dat) and the node fetches any it lacks from the confidants
    fs::create_directories(root_, ec);
    // the node's own database shares the disk: a build that could fill it is not started
    uint64_t estimate = 512ull << 20;
    for (const auto& entry : fs::directory_iterator(in.checkpointDir, ec)) {
        estimate += entry.is_regular_file(ec) ? entry.file_size(ec) / 4 : 0;
    }
    const auto space = fs::space(root_, ec);
    if (!ec && space.available < 2 * estimate) {
        cswarning() << "SNAPSHOT: not enough free disk for the snapshot of checkpoint " << S << " (" << space.available / (1u << 20)
                    << " MB free, about " << estimate / (1u << 20) << " MB needed twice)";
        return;
    }
    cslog() << "SNAPSHOT: building the snapshot of checkpoint " << S;
    const auto started = std::chrono::steady_clock::now();
    const auto r = buildSnapshot(in, root_, key_);
    if (!r.ok) {
        // tried again a few times a minute apart (the block range may have been pruned under it)
        const bool retry = !stop_ && ++failedBuilds_ < kMaxBuildAttempts;
        cswarning() << "SNAPSHOT: no snapshot of checkpoint " << S << ": " << r.error << (retry ? ", trying again in a minute" : "");
        if (retry) {
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        }
        return;
    }
    failedBuilds_ = 0;
    uint64_t bytes = 0;
    for (const auto& f : r.manifest.files) {
        bytes += f.size;
    }
    cslog() << "SNAPSHOT: built block " << S << ", " << r.manifest.files.size() << " files, " << bytes / (1u << 20) << " MB, state digest "
            << r.manifest.stateDigest << " in "
            << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count() << " s";
    loadNewest();
}

void SnapshotService::loadNewest() {
    std::error_code ec;
    cs::Sequence best = 0;
    for (const auto& entry : fs::directory_iterator(root_, ec)) {
        const auto name = entry.path().filename().string();
        if (isDigits(name) && fs::is_regular_file(entry.path() / "snapshot.json", ec)) {
            best = std::max<cs::Sequence>(best, std::stoull(name));
        }
    }
    if (best == 0) {
        return;
    }
    if (provider_.loadedSeq() == best) {
        return;
    }
    std::string error;
    const auto dir = root_ / std::to_string(best);
    if (!provider_.load(dir, &error)) {
        // a snapshot of this node's own builder (it has snapshot.json): removed, so that the next
        // round builds it again while its checkpoint is kept
        const bool retry = ++failedBuilds_ < kMaxBuildAttempts;
        cswarning() << "SNAPSHOT: " << dir.string() << " is not served: " << error << "; removed"
                    << (retry ? ", to be built again in a minute" : "");
        fs::remove_all(dir, ec);
        if (retry) {
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        }
        return;
    }
    uint64_t bytes = 0;
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() != "snapshot.json") {
            bytes += entry.file_size(ec);
            ++files;
        }
    }
    cslog() << "SNAPSHOT: serving block " << best << " (" << files << " files, " << bytes / (1u << 20) << " MB)";
}

}  // namespace cs
