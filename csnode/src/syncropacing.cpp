#include <csnode/syncropacing.hpp>

#include <algorithm>

namespace cs {

bool SyncroPacing::inFlight(const State& s, uint64_t nowMs) const {
    return s.inFlight && nowMs < s.requestedAt + kInFlightMs;
}

void SyncroPacing::onSyncroMessage(const PublicKey& peer, SyncroMessage msg, uint64_t nowMs) {
    auto& s = peers_[peer];
    s.active = true;
    if (!inFlight(s, nowMs)) {
        // not an answer to a forward request (the backfill's, or a late one): nothing else changes
        s.inFlight = false;
        s.awaiting = false;
        return;
    }
    switch (msg) {
        case SyncroMessage::AwaitAnswer:
            s.awaiting = true;
            break;
        case SyncroMessage::NoSuchBlocks:
            if (s.awaiting) {
                // the entry stays AwaitAnswer in its log: every request up to it is refused
                s.refusedUpTo = std::max(s.refusedUpTo, s.lastHighest);
                s.refusedUntil = nowMs + kRefusedForMs;
            }
            else {
                // refused before it logged the request: its log holds what it held before
                s.lastHighest = s.lastHighestBefore;
                s.loggedUntil = s.loggedUntilBefore;
            }
            s.awaiting = false;
            s.inFlight = false;
            break;
        case SyncroMessage::DuplicatedRequest:
            // not logged: its entry from an earlier request still lives
            s.lastHighest = std::max(s.lastHighestBefore, s.lastHighest);
            s.loggedUntil = std::max(s.loggedUntilBefore, nowMs + kSentLifeMs);
            s.awaiting = false;
            s.inFlight = false;
            break;
        case SyncroMessage::IncorrectRequest:
            s.lastHighest = s.lastHighestBefore;
            s.loggedUntil = s.loggedUntilBefore;
            s.awaiting = false;
            s.inFlight = false;
            break;
        default:
            break;
    }
}

void SyncroPacing::onRequest(const PublicKey& peer, Sequence highest, uint64_t nowMs) {
    auto& s = peers_[peer];
    s.lastHighestBefore = s.lastHighest;
    s.loggedUntilBefore = s.loggedUntil;
    // a request it accepts replaces the last logged sequence, a lower one keeps the old entry
    if (!s.requested || highest > s.lastHighest || nowMs >= s.loggedUntil) {
        s.lastHighest = highest;
    }
    s.requested = true;
    s.requestedAt = nowMs;
    s.inFlight = true;
    s.awaiting = false;
    // until the reply comes the entry lives anyway; the reply sets the 5 s
    s.loggedUntil = std::max(s.loggedUntil, nowMs + kSentLifeMs);
}

void SyncroPacing::onReply(const PublicKey& peer, uint64_t nowMs) {
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return;
    }
    it->second.awaiting = false;
    it->second.inFlight = false;
    it->second.loggedUntil = nowMs + kSentLifeMs;
}

void SyncroPacing::forget(const PublicKey& peer) {
    peers_.erase(peer);
}

bool SyncroPacing::active(const PublicKey& peer) const {
    const auto it = peers_.find(peer);
    return it != peers_.end() && it->second.active;
}

bool SyncroPacing::anyActive() const {
    return std::any_of(peers_.begin(), peers_.end(), [](const auto& p) { return p.second.active; });
}

bool SyncroPacing::inFlight(const PublicKey& peer, uint64_t nowMs) const {
    const auto it = peers_.find(peer);
    return it != peers_.end() && inFlight(it->second, nowMs);
}

bool SyncroPacing::allows(const PublicKey& peer, Sequence highest, uint64_t nowMs) const {
    const auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.active) {
        return true;
    }
    const auto& s = it->second;
    if (nowMs < s.refusedUntil && highest <= s.refusedUpTo) {
        return false;
    }
    if (s.requested && nowMs < s.loggedUntil && highest <= s.lastHighest) {
        return false;
    }
    return true;
}

}  // namespace cs
