#include <csnode/snapshot_service.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

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

SnapshotService::SnapshotService(BlockChain& blockchain, const cs::PrivateKey& key, std::string nodeVersion)
: blockchain_(blockchain), key_(key), nodeVersion_(std::move(nodeVersion)) {
    server_ = std::make_unique<SnapshotServer>(root_, [this]() {
        return std::make_pair(blockchain_.getLastSeq(), static_cast<cs::Sequence>(cs::Conveyer::instance().currentRoundNumber()));
    });
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
    server_->stop();
}

void SnapshotService::onCheckpointSaved(cs::Sequence sequence) {
    (void)sequence;
    buildPending_ = true;
    cv_.notify_all();
}

void SnapshotService::run() {
    // the builder works on this thread: below the node's other work
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
    server_->reload();
    while (!stop_) {
        const auto& cfg = cs::ConfigHolder::instance().config()->getSnapshotSettings();
        SnapshotServer::Settings settings;
        settings.bind = cfg.serveBind;
        settings.port = cfg.servePort;
        settings.maxConnections = cfg.serveMaxConnections;
        settings.rateKBps = cfg.serveRateKbps;
        server_->apply(settings);
        if (settings.port != 0 && buildPending_.exchange(false)) {
            buildLatest();
        }
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait_for(lock, kPeriod, [this]() { return stop_.load(); });
    }
}

void SnapshotService::buildLatest() {
    const cs::Sequence S = latestCheckpoint();
    std::error_code ec;
    if (S == 0 || fs::exists(root_ / std::to_string(S) / "snapshot.json", ec)) {
        return;   // nothing new: no checkpoint with a digest yet, or already built
    }
    const auto& cfg = cs::ConfigHolder::instance().config()->getSnapshotSettings();
    SnapshotBuildInput in;
    in.seq = S;
    in.checkpointDir = fs::path("qs") / std::to_string(S);
    in.firstStored = blockchain_.getFirstStoredSequence();
    in.snapshotBlocks = cfg.snapshotBlocks;
    in.nodeVersion = nodeVersion_;
    in.stop = &stop_;
    in.blockBytes = [this](cs::Sequence seq, cs::Bytes& out) {
        const csdb::Pool pool = blockchain_.loadBlock(seq);
        if (!pool.is_valid()) {
            return false;
        }
        out = pool.to_binary();
        return !out.empty();
    };
    // no contract records: the contracts database holds the latest state of each contract, later than
    // S, while the signed snapshot is the state at S; the checkpoint carries the states at S
    // (smartcontracts.dat) and the node fetches any it lacks from the confidants
    fs::create_directories(root_, ec);
    cslog() << "SNAPSHOT: building the snapshot of checkpoint " << S;
    const auto started = std::chrono::steady_clock::now();
    const auto r = buildSnapshot(in, root_, key_);
    if (!r.ok) {
        cswarning() << "SNAPSHOT: no snapshot of checkpoint " << S << ": " << r.error;
        return;
    }
    uint64_t bytes = 0;
    for (const auto& f : r.manifest.files) {
        bytes += f.size;
    }
    cslog() << "SNAPSHOT: built block " << S << ", " << r.manifest.files.size() << " files, " << bytes / (1u << 20) << " MB, state digest "
            << r.manifest.stateDigest << " in "
            << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count() << " s";
    server_->reload();
}

}  // namespace cs
