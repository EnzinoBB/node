#include <csnode/snapshot_provider.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cs {

namespace {
namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

bool fail(std::string* error, const std::string& text) {
    if (error) {
        *error = text;
    }
    return false;
}

bool powerOfTwo(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

uint32_t msUntil(SteadyClock::time_point now, SteadyClock::time_point when) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
    return static_cast<uint32_t>(std::clamp<int64_t>(ms, 1, SnapshotProvider::kMaxRetryMs));
}
}  // namespace

struct SnapshotProvider::Loaded {
    struct File {
        int fd = -1;
        uint64_t size = 0;
        uint64_t chunks = 0;
        uint64_t firstChunk = 0;   // index of its first chunk among all files, in manifest order
    };

    SnapshotManifest manifest;
    std::string json;
    std::map<std::string, File> files;
    uint64_t totalBytes = 0;
    uint64_t totalChunks = 0;

    ~Loaded() {
        for (auto& [name, f] : files) {
            if (f.fd >= 0) {
                ::close(f.fd);
            }
        }
    }
};

bool SnapshotProvider::Bucket::take(double cost, double rate, double burst, SteadyClock::time_point now, uint32_t& retryMs) {
    const double elapsed = std::chrono::duration<double>(now - at).count();
    tokens = std::min(burst, tokens + std::max(0.0, elapsed) * rate);
    at = now;
    if (tokens >= cost) {
        tokens -= cost;
        return true;
    }
    const double ms = std::ceil((cost - tokens) / rate * 1000.0);
    retryMs = static_cast<uint32_t>(std::clamp(ms, 1.0, static_cast<double>(kMaxRetryMs)));
    return false;
}

SnapshotProvider::SnapshotProvider(Clock clock)
: clock_(std::move(clock)) {
}

SnapshotProvider::~SnapshotProvider() = default;

bool SnapshotProvider::load(const fs::path& dir, std::string* error) {
    cs::Sequence seq = 0;
    try {
        size_t used = 0;
        const auto name = dir.filename().string();
        seq = std::stoull(name, &used);
        if (used != name.size()) {
            return fail(error, "not a snapshot folder: " + dir.string());
        }
    }
    catch (const std::exception&) {
        return fail(error, "not a snapshot folder: " + dir.string());
    }

    std::error_code ec;
    const auto jsonPath = dir / "snapshot.json";
    const auto jsonSize = fs::file_size(jsonPath, ec);
    if (ec || jsonSize > snapshot_p2p::kMaxManifestJson) {
        return fail(error, "no snapshot.json of at most 256 KB in " + dir.string());
    }
    std::ifstream in(jsonPath, std::ios::binary);
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
        return fail(error, "cannot read " + jsonPath.string());
    }

    std::string why;
    auto manifest = SnapshotManifest::fromJson(json, &why);
    if (!manifest) {
        return fail(error, jsonPath.string() + ": " + why);
    }
    if (manifest->seq != seq) {
        return fail(error, jsonPath.string() + " is the snapshot of block " + std::to_string(manifest->seq));
    }
    if (!manifest->verify()) {
        return fail(error, jsonPath.string() + ": bad signature");
    }
    if (manifest->chunkSize > snapshot_p2p::kMaxPieceBytes || !powerOfTwo(manifest->chunkSize)) {
        return fail(error, jsonPath.string() + ": chunk size " + std::to_string(manifest->chunkSize) + " cannot be served");
    }

    auto loaded = std::make_shared<Loaded>();
    for (const auto& f : manifest->files) {
        // the path comes from the manifest this node built and verified, never from a request
        const auto path = dir / f.name;
        Loaded::File entry;
        entry.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (entry.fd < 0) {
            return fail(error, "cannot open " + path.string());
        }
        loaded->files[f.name] = entry;   // closed with loaded from here on
        struct stat st {};
        if (::fstat(entry.fd, &st) != 0 || static_cast<uint64_t>(st.st_size) != f.size) {
            return fail(error, path.string() + " does not have the size of the manifest");
        }
        // pieces are read on the thread that handles consensus messages: keep the files in the page cache
        ::posix_fadvise(entry.fd, 0, 0, POSIX_FADV_WILLNEED);
        loaded->files[f.name].size = f.size;
        loaded->files[f.name].chunks = f.chunks.size();
        loaded->files[f.name].firstChunk = loaded->totalChunks;
        loaded->totalChunks += f.chunks.size();
        loaded->totalBytes += f.size;
    }
    loaded->manifest = std::move(*manifest);
    loaded->json = std::move(json);

    std::lock_guard<std::mutex> lock(loadedLock_);
    loaded_ = std::move(loaded);
    return true;
}

void SnapshotProvider::unload() {
    std::lock_guard<std::mutex> lock(loadedLock_);
    loaded_.reset();
}

std::optional<cs::Sequence> SnapshotProvider::loadedSeq() const {
    std::lock_guard<std::mutex> lock(loadedLock_);
    if (!loaded_) {
        return std::nullopt;
    }
    return loaded_->manifest.seq;
}

SnapshotProvider::Peer& SnapshotProvider::peer(const cs::PublicKey& key, SteadyClock::time_point now) {
    auto it = peers_.find(key);
    if (it == peers_.end()) {
        if (peers_.size() >= kMaxPeers) {
            // a peer downloading now goes last: new keys (free to make up) must not push it out
            auto downloading = [&](const Peer& p) { return p.lastPiece != SteadyClock::time_point{} && now - p.lastPiece < kPeerWindow; };
            auto oldest = std::min_element(peers_.begin(), peers_.end(), [&](const auto& a, const auto& b) {
                const bool da = downloading(a.second);
                const bool db = downloading(b.second);
                return da != db ? !da : a.second.lastSeen < b.second.lastSeen;
            });
            peers_.erase(oldest);
        }
        it = peers_.emplace(key, Peer{}).first;
    }
    it->second.lastSeen = now;
    return it->second;
}

size_t SnapshotProvider::activePeers(SteadyClock::time_point now, const cs::PublicKey& except) const {
    size_t n = 0;
    for (const auto& [key, p] : peers_) {
        if (key != except && p.lastPiece != SteadyClock::time_point{} && now - p.lastPiece < kPeerWindow) {
            ++n;
        }
    }
    return n;
}

std::optional<snapshot_p2p::ManifestReply> SnapshotProvider::onManifestRequest(const cs::PublicKey& sender, const Settings& settings,
                                                                                const NodeState& state) {
    using snapshot_p2p::ManifestStatus;
    snapshot_p2p::ManifestReply reply;
    reply.lastSeq = state.lastSeq;
    reply.round = state.round;
    reply.servingTrusted = state.trusted;

    std::shared_ptr<const Loaded> loaded;
    {
        std::lock_guard<std::mutex> lock(loadedLock_);
        loaded = loaded_;
    }
    if (!settings.serve) {
        reply.status = ManifestStatus::NotServing;
        return reply;
    }
    if (!loaded) {
        reply.status = ManifestStatus::Building;
        return reply;
    }
    if (!state.synced) {
        reply.status = ManifestStatus::NotSynced;
        return reply;
    }

    // the manifest is the large reply to a small request: once per peer every 5 s, paid for, and
    // to keys that are not downloading at most 20 a second (bursts of 8)
    const auto now = clock_();
    const auto known = peers_.find(sender);
    const bool downloading = known != peers_.end() && known->second.lastPiece != SteadyClock::time_point{} &&
                             now - known->second.lastPiece < kPeerWindow;
    if (known != peers_.end() && known->second.manifestSent && now - known->second.lastManifest < kManifestInterval) {
        return std::nullopt;
    }
    if (!downloading) {
        if (!manifestsStarted_) {
            manifests_ = Bucket{kManifestBurst, now};
            manifestsStarted_ = true;
        }
        uint32_t retry = 0;
        if (!manifests_.take(1, kManifestsPerSecond, kManifestBurst, now, retry)) {
            ++busyAnswers_;
            return std::nullopt;
        }
    }
    auto& p = peer(sender, now);
    if (settings.rateKBps != 0) {
        const double rate = settings.rateKBps * 1000.0 / (state.trusted ? 2 : 1);
        const double burst = static_cast<double>(kBurstBytes) / (state.trusted ? 2 : 1);
        if (!globalStarted_) {
            global_ = Bucket{burst, now};
            globalStarted_ = true;
        }
        uint32_t retry = 0;
        if (!global_.take(static_cast<double>(loaded->json.size()), rate, burst, now, retry)) {
            ++busyAnswers_;
            return std::nullopt;
        }
    }
    p.manifestSent = true;
    p.lastManifest = now;
    ++manifestsServed_;
    reply.status = ManifestStatus::Ready;
    reply.manifestJson = loaded->json;
    return reply;
}

snapshot_p2p::PieceReply SnapshotProvider::onPieceRequest(const cs::PublicKey& sender, const snapshot_p2p::PieceRequest& request,
                                                          const Settings& settings, const NodeState& state) {
    using snapshot_p2p::PieceStatus;
    snapshot_p2p::PieceReply reply;
    reply.seq = request.seq;
    reply.name = request.name;
    reply.chunk = request.chunk;

    std::shared_ptr<const Loaded> loaded;
    {
        std::lock_guard<std::mutex> lock(loadedLock_);
        loaded = loaded_;
    }
    if (!settings.serve || !loaded) {
        reply.status = PieceStatus::NotServing;
        return reply;
    }
    const auto now = clock_();
    auto busy = [&](uint32_t retryMs) {
        ++busyAnswers_;
        reply.status = PieceStatus::Busy;
        reply.retryMs = retryMs;
        return reply;
    };
    if (!state.synced) {
        return busy(kMaxRetryMs);
    }
    const auto file = loaded->files.find(request.name);
    if (request.seq != loaded->manifest.seq || file == loaded->files.end() || request.chunk >= file->second.chunks) {
        reply.status = PieceStatus::Unknown;
        return reply;
    }
    const uint64_t chunkSize = loaded->manifest.chunkSize;
    const uint64_t offset = uint64_t(request.chunk) * chunkSize;
    const auto length = static_cast<size_t>(std::min<uint64_t>(chunkSize, file->second.size - offset));

    auto& p = peer(sender, now);
    const bool active = p.lastPiece != SteadyClock::time_point{} && now - p.lastPiece < kPeerWindow;
    if (!active && activePeers(now, sender) >= std::max<size_t>(settings.maxPeers, 1)) {
        return busy(kMaxRetryMs);
    }
    if (now - p.windowStart >= kPieceWindow) {
        p.windowStart = now;
        p.piecesInWindow = 0;
    }
    if (p.piecesInWindow >= kPiecesPerWindow) {
        return busy(msUntil(now, p.windowStart + kPieceWindow));
    }
    if (settings.rateKBps != 0) {
        const double rate = settings.rateKBps * 1000.0 / (state.trusted ? 2 : 1);
        const double burst = static_cast<double>(kBurstBytes) / (state.trusted ? 2 : 1);
        if (!globalStarted_) {
            global_ = Bucket{burst, now};
            globalStarted_ = true;
        }
        if (!p.bucketStarted) {
            p.bucket = Bucket{burst / 2, now};
            p.bucketStarted = true;
        }
        // both buckets are checked before either is charged
        Bucket peerTry = p.bucket;
        Bucket globalTry = global_;
        uint32_t retryPeer = 0;
        uint32_t retryGlobal = 0;
        const bool peerOk = peerTry.take(static_cast<double>(length), rate / 2, burst / 2, now, retryPeer);
        const bool globalOk = globalTry.take(static_cast<double>(length), rate, burst, now, retryGlobal);
        if (!peerOk || !globalOk) {
            return busy(std::max(peerOk ? 0 : retryPeer, globalOk ? 0 : retryGlobal));
        }
        p.bucket = peerTry;
        global_ = globalTry;
    }

    reply.data.resize(length);
    size_t done = 0;
    while (done < length) {
        const auto n = ::pread(file->second.fd, reply.data.data() + done, length - done, static_cast<off_t>(offset + done));
        if (n <= 0) {
            reply.data.clear();
            reply.status = PieceStatus::NotServing;
            return reply;
        }
        done += static_cast<size_t>(n);
    }

    ++p.piecesInWindow;
    p.lastPiece = now;
    if (p.seq != loaded->manifest.seq) {
        p.seq = loaded->manifest.seq;
        p.bytes = 0;
        p.chunksSeen.assign(static_cast<size_t>(loaded->totalChunks), false);
        p.completed = false;
        p.firstPiece = now;
    }
    const size_t index = static_cast<size_t>(file->second.firstChunk + request.chunk);
    if (index < p.chunksSeen.size() && !p.chunksSeen[index]) {
        p.chunksSeen[index] = true;
        p.bytes += length;
    }
    ++piecesServed_;
    bytesServed_ += length;
    if (!p.completed && p.bytes >= loaded->totalBytes) {
        p.completed = true;
        if (onPeerCompleted) {
            onPeerCompleted(sender, p.seq, p.bytes, std::chrono::duration_cast<std::chrono::seconds>(now - p.firstPiece));
        }
    }
    reply.status = PieceStatus::Ok;
    return reply;
}

std::string SnapshotProvider::takeSummary() {
    if (piecesServed_ == 0 && manifestsServed_ == 0 && busyAnswers_ == 0) {
        return {};
    }
    const auto now = clock_();
    size_t recent = 0;
    for (const auto& [key, p] : peers_) {
        if (now - p.lastSeen < std::chrono::hours(1)) {
            ++recent;
        }
    }
    std::ostringstream out;
    out << recent << " peers, " << manifestsServed_ << " manifests, " << piecesServed_ << " pieces, " << bytesServed_ / (1u << 20)
        << " MiB, " << busyAnswers_ << " busy answers";
    piecesServed_ = bytesServed_ = busyAnswers_ = manifestsServed_ = 0;
    return out.str();
}

}  // namespace cs
