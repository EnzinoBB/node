#ifndef SYNCROPACING_HPP
#define SYNCROPACING_HPP

#include <cstdint>
#include <map>

#include <csnode/nodecore.hpp>
#include <lib/system/common.hpp>

namespace cs {

// Forward sync against responders whose anti-duplicate log is active (special order 35): such a
// responder refuses a request whose last sequence is not above the last one it logged for the same
// requester, while that entry lives (5 s after its reply), and for ever when it accepted a request
// (AwaitAnswer) and then had nothing to send (NoSuchBlocks). A requester that has not applied
// order 35 itself (a node syncing from block 0 before that block) knows it only from the SyncroMsg
// the responder sends: each peer that sent one is active, and requests to it are paced.
//
// Every forward request is recorded, also to a peer not known as active yet, so that the answer to
// it counts. A SyncroMsg that arrives while no forward request to that peer is in flight answers
// another request (the backfill's): it marks the peer active and changes nothing else.
class SyncroPacing {
public:
    static constexpr uint64_t kSentLifeMs = 6'000;        // the responder keeps a Sent entry 5 s
    static constexpr uint64_t kInFlightMs = 30'000;       // a request without an answer this long is over
    static constexpr uint64_t kRefusedForMs = 3'600'000;  // "for ever" on the responder: an hour here

    void onSyncroMessage(const PublicKey& peer, SyncroMessage msg, uint64_t nowMs);
    void onRequest(const PublicKey& peer, Sequence highest, uint64_t nowMs);
    void onReply(const PublicKey& peer, uint64_t nowMs);
    void forget(const PublicKey& peer);

    // the peer sent a SyncroMsg: its anti-duplicate log is active
    bool active(const PublicKey& peer) const;
    bool anyActive() const;
    // a forward request to that peer waits for its answer
    bool inFlight(const PublicKey& peer, uint64_t nowMs) const;
    // a request to that peer ending at `highest` would not be refused as a duplicate
    bool allows(const PublicKey& peer, Sequence highest, uint64_t nowMs) const;

private:
    struct State {
        bool active = false;
        bool requested = false;
        Sequence lastHighest = 0;      // the last sequence of our latest request it logged
        Sequence lastHighestBefore = 0;   // ... and before that request, if it refuses to log it
        uint64_t loggedUntilBefore = 0;
        uint64_t requestedAt = 0;
        bool inFlight = false;
        uint64_t loggedUntil = 0;      // its entry for that request lives until then
        bool awaiting = false;         // AwaitAnswer for the request in flight
        Sequence refusedUpTo = 0;      // not again up to this (NoSuchBlocks after AwaitAnswer) ...
        uint64_t refusedUntil = 0;     // ... until then
    };
    bool inFlight(const State& s, uint64_t nowMs) const;

    std::map<PublicKey, State> peers_;
};

}  // namespace cs

#endif  // SYNCROPACING_HPP
