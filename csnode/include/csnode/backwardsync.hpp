#ifndef BACKWARDSYNC_HPP
#define BACKWARDSYNC_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <csdb/pool.hpp>
#include <csnode/nodecore.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Backward sync (docs/specs/2026-10-17-backward-sync-design.md): a node whose storage starts above
// its floor (block 0 for a full node) fetches the blocks below its lowest one from its neighbours
// with BlockRequest, newest first, checks each against the hash chain and its signatures, and
// stores it strictly downwards, never applying it to the caches. Requests go out only while the
// node is synced; replies already asked for are stored in any case. Single-threaded: every call
// comes from the node's processor thread, but stop() and stats(); the periodic flush runs on a
// thread of its own (Settings::asyncFlush). The chain, the network and the clock are reached only
// through Hooks.
class BackwardSync {
public:
    struct Peer {
        PublicKey key;
        Sequence lastSeq = 0;
    };

    // what the node knows at this tick
    struct Context {
        std::vector<Peer> peers;            // the neighbours and their last block
        bool forwardSyncRunning = false;    // PoolSynchronizer::isSyncroStarted()
        bool bootstrapRound = false;
        bool bootstrapping = false;         // the node fetches a snapshot: its chain is not open
        RoundNumber round = 0;
        Sequence lastSeq = 0;
        bool trusted = false;               // the node is a confidant (or the writer) of this round
    };

    // the first stored block and the hash of the block below it (empty: unknown)
    struct Anchor {
        Sequence first = 0;
        csdb::PoolHash prevHash;
    };

    struct Hooks {
        std::function<Anchor()> anchor;
        std::function<bool(const csdb::Pool&)> store;   // false: not stored (e.g. a full disk)
        std::function<bool()> flush;
        std::function<Sequence()> floor;                // the lowest block to store; none: 0
        std::function<void(const PublicKey&, const PoolsRequestedSequences&)> sendRequest;
        std::function<uint64_t()> clock;                // ms, steady; default: steady_clock
        std::function<void(const std::string&)> onLog;  // optional: every BACKFILL line, for tests
    };

    struct Settings {
        size_t batch = 100;    // blocks per request, 1..1000
        size_t rate = 1000;    // blocks requested per second over all peers, 0: unlimited
        // blocks verified and stored per call (a tick or a reply), so that the processor thread,
        // which also runs consensus, is never held long; the rest waits for the next call
        size_t storePerTurn = 50;
        // and for at most this long (ms) per call while the node is a confidant, 0: no limit, at most
        // 1000. A turn ends after the block that passes it: a mainnet turn of 50 blocks took up to 1.7 s,
        // more than a round
        uint64_t turnBudgetMs = 50;
        // the same while the node is not a confidant, with little consensus work: about 450 blocks/s
        // on mainnet instead of about 250
        uint64_t idleTurnBudgetMs = 90;
        // the periodic flush (up to several seconds on mainnet) runs on a thread of its own instead of
        // the processor thread; the flushes at the end and at a stop stay where they are
        bool asyncFlush = true;
    };

    struct Stats {
        Sequence first = 0;          // the lowest block stored so far
        size_t stored = 0;
        size_t rejected = 0;         // blocks that failed verification
        size_t dropped = 0;          // blocks nobody asked for, or beyond the buffer
        size_t confirmationErrors = 0;
        size_t inFlight = 0;
        size_t buffered = 0;
        uint64_t busyUs = 0;         // processor-thread time spent verifying and storing
        uint64_t longestTurnUs = 0;  // the longest single call
        size_t slowTurns = 0;        // calls longer than kSlowTurnMs
        uint64_t longestFlushUs = 0; // the longest storage flush (every kFlushBlocks or kFlushMs)
        bool started = false;
        bool done = false;
    };

    static constexpr size_t kMaxInFlight = 4;
    static constexpr size_t kBufferBatches = 8;               // blocks waiting to be stored, in batches
    static constexpr uint64_t kBatchTimeoutMs = 30'000;
    static constexpr uint64_t kSlowPeerDelayMs = 60'000;      // a peer whose batch timed out
    static constexpr unsigned kFailuresToMark = 2;
    static constexpr uint64_t kMarkMs = 3'600'000;            // a peer left out for an hour
    static constexpr uint64_t kPacedMs = 6'000;               // after AwaitAnswer: the responder's log keeps a request 5 s
    static constexpr uint64_t kUnpacedMs = 200;
    static constexpr uint64_t kDuplicatedDelayMs = 10'000;
    static constexpr uint64_t kStoreRetryMs = 60'000;
    static constexpr size_t kFlushBlocks = 10'000;
    static constexpr uint64_t kFlushMs = 60'000;
    // a flush (up to 2.4 s on mainnet) waits while the node is a confidant, at most this long: blocks
    // not flushed yet are fetched again after a crash, nothing else is lost
    static constexpr uint64_t kTrustedFlushDeferMs = 600'000;
    static constexpr size_t kProgressBlocks = 100'000;
    static constexpr uint64_t kSlowTurnMs = 200;              // a longer turn is logged, with where its time went

    BackwardSync(Hooks hooks, Settings settings);
    ~BackwardSync();
    BackwardSync(const BackwardSync&) = delete;
    BackwardSync& operator=(const BackwardSync&) = delete;

    // every 500 ms
    void tick(const Context& ctx);
    // a RequestedBlock reply of a peer this asked
    void onBlocks(const PublicKey& sender, PoolsBlock&& blocks);
    void onSyncroMessage(const PublicKey& sender, SyncroMessage msg);
    // a batch is in flight to this peer: its RequestedBlock replies below the first stored block are ours
    bool hasRequestTo(const PublicKey& peer) const;
    // fetching: started, not done, not stopped
    bool isActive() const;
    // any thread
    void stop();
    Stats stats() const;

private:
    struct Batch {
        std::set<Sequence> missing;   // asked for, not received yet
        uint64_t sentAt = 0;
    };
    struct PeerState {
        uint64_t nextRequestAt = 0;
        uint64_t markedUntil = 0;
        unsigned failures = 0;
        bool paced = false;            // AwaitAnswer seen: its anti-duplicate log is active (order 35)
        bool awaitingAnswer = false;   // AwaitAnswer for the batch in flight
    };
    struct Buffered {
        csdb::Pool pool;
        PublicKey peer;
    };

    uint64_t now() const;
    void log(const std::string& text, bool error = false) const;
    bool refreshAnchor(const Context& ctx);
    void drain();
    // why the block below the last stored one is refused, empty if it is accepted
    std::string verify(const csdb::Pool& block) const;
    void reject(const PublicKey& peer, Sequence seq, const std::string& why);
    void reassign(const PublicKey& peer);
    void mark(const PublicKey& peer, uint64_t until, const std::string& why);
    void expire();
    void flushIfDue(bool force);
    void flushWorker();
    void stopFlusher();
    void request(const Context& ctx);
    std::optional<PublicKey> pickPeer(const Context& ctx, Sequence highest);
    void finish();

    Hooks hooks_;
    Settings settings_;

    std::map<PublicKey, Batch> inFlight_;
    std::map<PublicKey, PeerState> peers_;
    std::map<Sequence, Buffered> buffer_;
    std::map<Sequence, std::set<Sequence>> retry_;   // by highest sequence: asked for again first
    std::optional<PublicKey> lastPicked_;

    Sequence floor_ = 0;
    Sequence next_ = 0;              // the next block to store
    Sequence nextToRequest_ = 0;     // the highest block not asked for yet
    bool requestedAll_ = false;      // every block down to the floor was asked for once
    csdb::PoolHash expectedHash_;    // the hash of block next_
    std::optional<csdb::Pool> lastStored_;
    Sequence head_ = 0;

    double tokens_ = 0;
    uint64_t tokensAt_ = 0;
    uint64_t retryStoreAt_ = 0;
    bool storeFailing_ = false;
    bool paused_ = false;
    std::string pauseReason_;
    bool noPeerLogged_ = false;
    bool bufferFullLogged_ = false;
    uint64_t lastFlushAt_ = 0;
    size_t storedSinceFlush_ = 0;
    uint64_t progressAt_ = 0;
    size_t progressStored_ = 0;
    // the phases of storing since the last progress line, for its averages per block
    uint64_t phaseVerifyUs_ = 0;
    uint64_t phaseStoreUs_ = 0;
    uint64_t phaseConfirmUs_ = 0;
    uint64_t phaseTransactions_ = 0;
    uint64_t progressBusyUs_ = 0;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> done_{false};
    std::atomic<Sequence> first_{0};
    std::atomic<size_t> stored_{0};
    std::atomic<size_t> rejected_{0};
    std::atomic<size_t> dropped_{0};
    std::atomic<size_t> confirmationErrors_{0};
    std::atomic<size_t> inFlightCount_{0};
    std::atomic<size_t> bufferedCount_{0};
    std::atomic<uint64_t> busyUs_{0};
    std::atomic<uint64_t> longestTurnUs_{0};
    std::atomic<size_t> slowTurns_{0};
    bool trusted_ = false;   // from the last tick: the budget of the turns until the next one
    std::atomic<uint64_t> longestFlushUs_{0};

    // the flush thread: started at the first periodic flush, joined by stop() and the destructor
    std::mutex flushMutex_;
    std::condition_variable flushCv_;
    std::thread flushThread_;
    bool flushRequested_ = false;   // the fields below: under flushMutex_
    bool flushRunning_ = false;
    bool flushStopping_ = false;
    size_t flushBlocks_ = 0;        // what the requested flush covers, for its log line
    Sequence flushDown_ = 0;
};

}  // namespace cs

#endif  // BACKWARDSYNC_HPP
