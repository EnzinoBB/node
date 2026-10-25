#ifndef SNAPSHOT_FETCHER_HPP
#define SNAPSHOT_FETCHER_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <config.hpp>
#include <csnode/snapshot_agreement.hpp>
#include <csnode/snapshot_manifest.hpp>
#include <csnode/snapshot_messages.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Gets a snapshot from the p2p neighbours of a node that starts on an empty database
// (docs/specs/2026-10-25-snapshot-p2p-design.md, sections 6.2-6.5): brings the peers that serve
// snapshots into the neighbour table, collects their signed manifests, waits for the agreement of
// [snapshot] min_sources independent sources, then downloads the files of the agreed snapshot
// piece by piece into tmpDir, each piece checked against the manifest's chunk hash and each file
// against its SHA-256. Single-threaded: every call comes from the node's processor thread. The
// network, the routing table and the clock are reached only through Hooks.
class SnapshotFetcher {
public:
    struct Peers {
        std::vector<std::pair<cs::PublicKey, uint8_t>> known;   // key, capabilities (net/hostdata.hpp)
        std::set<cs::PublicKey> neighbours;
    };

    struct Hooks {
        std::function<void(const cs::PublicKey&, const snapshot_p2p::ManifestRequest&)> sendManifestRequest;
        std::function<void(const cs::PublicKey&, const snapshot_p2p::PieceRequest&)> sendPieceRequest;
        std::function<void(const std::set<cs::PublicKey>&)> addNeighbours;
        std::function<std::string(const cs::PublicKey&)> addressOf;   // "" when unknown
        std::function<uint64_t()> clock;                               // ms, steady; default: steady_clock
        std::function<void(const std::string&)> onLog;                 // optional: every SNAPSHOT line, for tests
    };

    enum class Phase {
        Discover,   // no manifest asked yet
        Collect,    // manifests asked, waiting for agreement
        Fetch,      // downloading the agreed snapshot
        Import,     // every file downloaded and verified: result() is set
        Done,       // dry run: agreement reached or fetch_wait over, groups() has what was seen
        Fallback    // no snapshot: the node syncs from block 0
    };

    static constexpr uint64_t kManifestIntervalMs = 30'000;
    static constexpr uint64_t kManifestRetryMs = 10'000;   // a request without an answer: asked again sooner
    static constexpr size_t kManifestRetries = 2;           // ... this many times, then every 30 s
    static constexpr uint64_t kSilentMs = kManifestIntervalMs + 5'000;   // no answer this long: silent
    static constexpr uint64_t kNeighbourRetryMs = 60'000;
    static constexpr uint64_t kPieceTimeoutMs = 30'000;
    static constexpr uint64_t kMarkMs = 60'000;
    static constexpr uint64_t kMinBusyMs = 500;
    static constexpr uint64_t kMaxBusyMs = 10'000;
    static constexpr uint64_t kBusyMarkMs = 60'000;          // busy this long without a piece: marked
    static constexpr uint64_t kNoProgressMs = 300'000;       // a fetch without a new byte this long ends
    static constexpr uint64_t kLogIntervalMs = 30'000;
    static constexpr uint64_t kWaitLogIntervalMs = 300'000;
    static constexpr uint64_t kStallMs = 120'000;
    static constexpr size_t kPiecesPerSource = 2;
    static constexpr size_t kPiecesTotal = 8;
    static constexpr size_t kMaxSources = 64;        // discovered sources; configured keys come on top
    static constexpr size_t kMaxPerPrefix = 4;       // discovered sources per /24 (IPv4) or /48 (IPv6)
    static constexpr size_t kMaxPerBlock = 16;       // discovered sources per /16 (IPv4) or /32 (IPv6)
    static constexpr uint64_t kEvictedMs = 60'000;   // an evicted key is not taken again before
    static constexpr size_t kMaxEvicted = 1024;
    static constexpr size_t kMaxAttempts = 3;        // fetches that end without a snapshot, then Fallback
    static constexpr uint64_t kHashBytesPerTick = 64ull << 20;   // whole-file SHA-256 per tick
    static constexpr size_t kMaxSourcesFileKeys = 64;

    // cfg.fetchWait = 0: no wait, Fallback (or Done) at the first tick without agreement
    SnapshotFetcher(const SnapshotData& cfg, std::filesystem::path tmpDir, Hooks hooks, bool dryRun = false);
    ~SnapshotFetcher();
    SnapshotFetcher(const SnapshotFetcher&) = delete;
    SnapshotFetcher& operator=(const SnapshotFetcher&) = delete;

    void tick(const Peers& peers);   // every 500 ms
    void onManifestReply(const cs::PublicKey& sender, const snapshot_p2p::ManifestReply& reply);
    void onPieceReply(const cs::PublicKey& sender, snapshot_p2p::PieceReply&& reply);

    Phase phase() const {
        return phase_;
    }
    // the manifest of the downloaded files (in tmpDir) and the agreed sources; set in Import
    std::optional<std::pair<SnapshotManifest, std::vector<SourceManifest>>> result() const;
    // the importer refused fileAtFault: that file's SHA-256 is not fetched again; back to Collect,
    // or Fallback after kMaxAttempts failed imports
    void onImportFailed(const std::string& fileAtFault);
    std::vector<GroupReport> groups() const;
    // no request is sent and no reply is taken after this
    void stop();

    // for the logs and the tests
    size_t manifests() const;
    size_t outstanding() const;
    uint64_t dropped() const {
        return dropped_;
    }
    uint64_t bytesDone() const;
    uint64_t bytesTotal() const;

private:
    struct Source {
        bool configured = false;      // [snapshot] sources or sources_file
        bool trusted = false;         // [snapshot] sources
        bool serving = false;         // announced capability bit 24
        std::string prefix;           // netPrefix and netBlock of the address, empty while unknown;
        std::string block;            // resolved again when a manifest arrives
        bool everAnswered = false;
        size_t unanswered = 0;        // manifest requests since the last answer
        uint64_t firstUnansweredAt = 0;
        uint64_t discoveredAt = 0;
        uint64_t addedAt = 0;         // last addNeighbours
        bool added = false;
        uint64_t askedAt = 0;
        bool everAsked = false;
        bool awaiting = false;        // a manifest request without a reply yet
        bool loggedAsk = false;
        std::optional<SnapshotManifest> manifest;
        uint64_t manifestAt = 0;
        std::string filesHash;
        std::string address;
        size_t marks = 0;
        uint64_t markedUntil = 0;
        bool banned = false;          // for the rest of the bootstrap
        uint64_t busyUntil = 0;
        uint64_t busySince = 0;       // first busy answer since its last piece; 0: none
        size_t outstanding = 0;
    };

    struct InFlight {
        cs::PublicKey source{};
        uint64_t sentAt = 0;
    };

    struct HashState;

    struct FileState {
        SnapshotFile file;
        std::vector<cs::PublicKey> sources;   // agreed sources declaring this file with this SHA-256
        std::vector<bool> have;
        size_t missing = 0;           // chunks not on disk yet
        uint64_t doneBytes = 0;
        std::map<uint32_t, InFlight> inFlight;
        int fd = -1;
        bool verifyPending = false;
        bool verified = false;
        std::shared_ptr<HashState> hashing;   // whole-file SHA-256 in progress, kHashBytesPerTick per tick
        uint64_t hashed = 0;
    };

    void log(const std::string& line, bool warning = false);
    uint64_t now() const;
    std::string keyText(const cs::PublicKey& key) const;
    Source* findSource(const cs::PublicKey& key);
    Source* addConfigured(const cs::PublicKey& key);
    Source* addDiscovered(const cs::PublicKey& key, uint64_t t);
    bool roomFor(const cs::PublicKey& key, const std::string& prefix, const std::string& block) const;
    bool evictOne(uint64_t t);
    bool silent(const Source& s, uint64_t t) const;
    void evict(std::map<cs::PublicKey, Source>::iterator it, uint64_t t);
    bool isProtected(const cs::PublicKey& key, const Source& s) const;
    void loadSourcesFile();

    void discover(const Peers& peers, uint64_t t);
    void askManifests(const Peers& peers, uint64_t t);
    bool tryAgree(uint64_t t);
    void logWaiting(uint64_t t);
    void logGroups(bool chosenOnly);
    std::string seenText() const;
    void checkWait(uint64_t t);

    bool usable(const Source& s, uint64_t t) const;
    bool everUsable(const Source& s) const;
    bool startFetch(uint64_t t);
    bool chooseTarget(uint64_t t);
    void fill(uint64_t t);
    void expire(uint64_t t);
    void verifyFiles(uint64_t t);
    void clearInFlight();
    void releaseSource(const cs::PublicKey& key);
    void mark(const cs::PublicKey& key, uint64_t t, bool forever, const std::string& why);
    void backToCollect(const std::string& why);
    void banDeclaring(const std::string& name, const std::string& sha256, const std::string& why);
    void closeFile(FileState& f);
    bool openFile(FileState& f, bool truncate);
    uint64_t pieceLength(const FileState& f, uint32_t chunk) const;
    void logProgress(uint64_t t, bool force);
    bool takeTokens(uint64_t cost, uint64_t t);

    SnapshotData cfg_;
    std::filesystem::path tmpDir_;
    Hooks hooks_;
    bool dryRun_;

    Phase phase_ = Phase::Discover;
    bool stopped_ = false;
    bool started_ = false;
    uint64_t startedAt_ = 0;
    std::map<cs::PublicKey, Source> sources_;
    std::set<cs::PublicKey> neighbours_;   // as of the last tick
    std::map<cs::PublicKey, uint64_t> evicted_;   // discovered keys let go, and when

    AgreementResult agreement_;
    std::vector<SourceManifest> agreedSources_;
    std::optional<SnapshotManifest> target_;
    std::vector<FileState> files_;
    std::set<std::pair<std::string, std::string>> badFiles_;   // (name, sha256) refused by the importer
    size_t failures_ = 0;          // fetches that ended without a snapshot, refused imports included
    bool dirty_ = true;            // the input of the agreement changed since it was computed
    uint64_t lastByteAt_ = 0;
    uint64_t lastSendAt_ = 0;
    size_t rr_ = 0;   // first source of the next fill

    // fetch_rate_kbps on requested bytes: a request goes out when nothing is owed, then owes its size
    double owed_ = 0;
    uint64_t owedAt_ = 0;

    uint64_t dropped_ = 0;
    uint64_t lastLogAt_ = 0;
    uint64_t lastWaitLogAt_ = 0;
    bool fallbackLogged_ = false;
    uint64_t lastProgressAt_ = 0;
    uint64_t lastProgressBytes_ = 0;
    int lastProgressDecile_ = -1;
    uint64_t fetchStartedAt_ = 0;
};

}  // namespace cs

#endif  // SNAPSHOT_FETCHER_HPP
