#include <csnode/syncropacing.hpp>

#include <algorithm>

namespace cs {

void SyncroPacing::onSyncroMessage(const PublicKey& peer, SyncroMessage msg, uint64_t nowMs) {
    auto& s = peers_[peer];
    switch (msg) {
        case SyncroMessage::AwaitAnswer:
            s.awaiting = true;
            break;
        case SyncroMessage::NoSuchBlocks:
            if (s.awaiting && s.requested) {
                // the entry stays AwaitAnswer in its log: every request up to it is refused
                s.refusedForever = true;
                s.refusedUpTo = std::max(s.refusedUpTo, s.lastHighest);
            }
            s.awaiting = false;
            break;
        case SyncroMessage::DuplicatedRequest:
            // its entry from an earlier request still lives
            s.loggedUntil = std::max(s.loggedUntil, nowMs + kSentLifeMs);
            s.awaiting = false;
            break;
        default:
            s.awaiting = false;
            break;
    }
}

void SyncroPacing::onRequest(const PublicKey& peer, Sequence highest, uint64_t nowMs) {
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return;
    }
    auto& s = it->second;
    // a request it accepts replaces the last logged sequence, a lower one keeps the old entry
    if (!s.requested || highest > s.lastHighest || nowMs >= s.loggedUntil) {
        s.lastHighest = highest;
    }
    s.requested = true;
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
    it->second.loggedUntil = nowMs + kSentLifeMs;
}

void SyncroPacing::forget(const PublicKey& peer) {
    peers_.erase(peer);
}

bool SyncroPacing::active(const PublicKey& peer) const {
    return peers_.count(peer) != 0;
}

bool SyncroPacing::anyActive() const {
    return !peers_.empty();
}

bool SyncroPacing::allows(const PublicKey& peer, Sequence highest, uint64_t nowMs) const {
    const auto it = peers_.find(peer);
    if (it == peers_.end()) {
        return true;
    }
    const auto& s = it->second;
    if (s.refusedForever && highest <= s.refusedUpTo) {
        return false;
    }
    if (s.requested && nowMs < s.loggedUntil && highest <= s.lastHighest) {
        return false;
    }
    return true;
}

}  // namespace cs
