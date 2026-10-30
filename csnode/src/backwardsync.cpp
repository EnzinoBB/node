#include <csnode/backwardsync.hpp>

#include <algorithm>
#include <chrono>
#include <limits>

#include <base58.h>

#include <csnode/blockverifier.hpp>
#include <lib/system/logger.hpp>

namespace cs {

namespace {
uint64_t steadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string keyText(const PublicKey& key) {
    return EncodeBase58(key.data(), key.data() + key.size());
}

constexpr uint64_t kForever = std::numeric_limits<uint64_t>::max();
}  // namespace

BackwardSync::BackwardSync(Hooks hooks, Settings settings)
: hooks_(std::move(hooks))
, settings_(settings) {
    if (!hooks_.clock) {
        hooks_.clock = steadyMs;
    }
    settings_.batch = std::clamp<size_t>(settings_.batch, 1, 1000);
    settings_.storePerTurn = std::clamp<size_t>(settings_.storePerTurn, 1, 1000);
}

uint64_t BackwardSync::now() const {
    return hooks_.clock();
}

void BackwardSync::log(const std::string& text, bool error) const {
    if (error) {
        cserror() << text;
    }
    else {
        cslog() << text;
    }
    if (hooks_.onLog) {
        hooks_.onLog(text);
    }
}

bool BackwardSync::hasRequestTo(const PublicKey& peer) const {
    return inFlight_.count(peer) != 0;
}

bool BackwardSync::isActive() const {
    return started_ && !done_ && !stopped_;
}

void BackwardSync::stop() {
    stopped_ = true;
}

BackwardSync::Stats BackwardSync::stats() const {
    Stats s;
    s.first = first_;
    s.stored = stored_;
    s.rejected = rejected_;
    s.dropped = dropped_;
    s.confirmationErrors = confirmationErrors_;
    s.inFlight = inFlightCount_;
    s.buffered = bufferedCount_;
    s.busyUs = busyUs_;
    s.longestTurnUs = longestTurnUs_;
    s.started = started_;
    s.done = done_;
    return s;
}

// Takes the chain's bottom when nothing is in flight or buffered: at the first tick, after a
// restart (no watermark but the storage's first block), and after pruning moved it.
bool BackwardSync::refreshAnchor(const Context& ctx) {
    const Anchor anchor = hooks_.anchor ? hooks_.anchor() : Anchor{};
    floor_ = hooks_.floor ? hooks_.floor() : 0;
    if (anchor.first == 0 || anchor.first <= floor_ || anchor.prevHash.is_empty()) {
        return false;
    }
    if (anchor.first - 1 != next_ || expectedHash_ != anchor.prevHash || !started_) {
        next_ = anchor.first - 1;
        nextToRequest_ = next_;
        requestedAll_ = false;
        expectedHash_ = anchor.prevHash;
        lastStored_.reset();
        retry_.clear();
    }
    first_ = anchor.first;
    if (!started_) {
        started_ = true;
        head_ = ctx.lastSeq;
        lastFlushAt_ = now();
        progressAt_ = lastFlushAt_;
        log("BACKFILL: " + std::string(floor_ == 0 ? "full node" : "pruned node") + " with blocks " + std::to_string(anchor.first) +
            ".." + std::to_string(ctx.lastSeq) + ": fetching " + std::to_string(floor_) + ".." + std::to_string(next_) +
            " from peers in the background");
    }
    return true;
}

void BackwardSync::tick(const Context& ctx) {
    if (stopped_ || done_) {
        return;
    }
    if (inFlight_.empty() && buffer_.empty() && !refreshAnchor(ctx)) {
        return;
    }
    if (!started_) {
        return;
    }
    // a pruned node's floor follows its pruning: nothing below it is fetched any more
    if (hooks_.floor) {
        floor_ = std::max(floor_, hooks_.floor());
        if (next_ < floor_) {
            finish();
            return;
        }
    }
    drain();
    if (done_) {
        return;
    }
    expire();
    flushIfDue(false);

    std::string why;
    if (ctx.bootstrapping) {
        why = "the node fetches a snapshot";
    }
    else if (ctx.forwardSyncRunning) {
        why = "the node syncs forward";
    }
    else if (ctx.bootstrapRound) {
        why = "bootstrap round";
    }
    else if (ctx.round == 0 || ctx.lastSeq + 3 < ctx.round) {
        why = "the node is behind the round";
    }
    if (!why.empty()) {
        if (!paused_) {
            paused_ = true;
            pauseReason_ = why;
            log("BACKFILL: paused (" + why + "), at block " + std::to_string(next_));
        }
        return;
    }
    if (paused_) {
        paused_ = false;
        log("BACKFILL: resumed at block " + std::to_string(next_));
    }
    request(ctx);

    if (stored_ >= progressStored_ + kProgressBlocks) {
        const uint64_t t = now();
        const double seconds = std::max<double>(1.0, static_cast<double>(t - progressAt_) / 1000.0);
        const double rate = static_cast<double>(stored_ - progressStored_) / seconds;
        const double hoursLeft = rate > 0 ? static_cast<double>(next_ - floor_) / rate / 3600.0 : 0;
        log("BACKFILL: stored down to #" + std::to_string(next_ + 1) + ", " + std::to_string(static_cast<size_t>(rate)) +
            " blocks/s over the last " + std::to_string(stored_ - progressStored_) + ", " + std::to_string(inFlight_.size()) +
            " peers in use, about " + std::to_string(static_cast<size_t>(hoursLeft + 0.5)) + " h left, " +
            std::to_string(busyUs_ / 1000) + " ms of processor thread so far (longest turn " + std::to_string(longestTurnUs_ / 1000) + " ms)");
        progressAt_ = t;
        progressStored_ = stored_;
    }
}

void BackwardSync::request(const Context& ctx) {
    const uint64_t t = now();
    if (settings_.rate > 0) {
        const double capacity = static_cast<double>(kMaxInFlight * settings_.batch);
        if (tokensAt_ == 0) {
            tokens_ = capacity;
        }
        else {
            tokens_ = std::min(capacity, tokens_ + static_cast<double>(settings_.rate) * static_cast<double>(t - tokensAt_) / 1000.0);
        }
        tokensAt_ = t;
    }

    while (inFlight_.size() < kMaxInFlight) {
        // the range: blocks asked for again first (the highest, which the storage waits for), then
        // the next ones downwards, no further than the buffer allows below the next block to store
        std::set<Sequence> sequences;
        bool fromRetry = false;
        if (!retry_.empty()) {
            sequences = std::prev(retry_.end())->second;
            fromRetry = true;
        }
        else if (!requestedAll_) {
            const Sequence window = static_cast<Sequence>(kBufferBatches * settings_.batch);
            if (nextToRequest_ < floor_) {
                requestedAll_ = true;   // the floor rose (pruning) past what is left to ask for
                break;
            }
            if (next_ - nextToRequest_ >= window) {
                break;
            }
            const Sequence hi = nextToRequest_;
            const Sequence lo = hi - floor_ >= settings_.batch ? hi - settings_.batch + 1 : floor_;
            for (Sequence s = lo; s <= hi; ++s) {
                sequences.insert(s);
            }
        }
        if (sequences.empty()) {
            break;
        }
        if (settings_.rate > 0 && tokens_ < static_cast<double>(sequences.size())) {
            break;
        }
        const auto peer = pickPeer(ctx, *sequences.rbegin());
        if (!peer) {
            if (!noPeerLogged_) {
                noPeerLogged_ = true;
                log("BACKFILL: no neighbour can serve blocks up to " + std::to_string(*sequences.rbegin()) + " now: waiting");
            }
            break;
        }
        noPeerLogged_ = false;
        if (fromRetry) {
            retry_.erase(std::prev(retry_.end()));
        }
        else {
            const Sequence lo = *sequences.begin();
            if (lo == floor_) {
                requestedAll_ = true;
            }
            else {
                nextToRequest_ = lo - 1;
            }
        }
        if (settings_.rate > 0) {
            tokens_ -= static_cast<double>(sequences.size());
        }
        PoolsRequestedSequences request(sequences.begin(), sequences.end());   // ascending, as forward sync asks
        Batch& batch = inFlight_[*peer];
        batch.missing = std::move(sequences);
        batch.sentAt = t;
        auto& state = peers_[*peer];
        state.awaitingAnswer = false;
        state.nextRequestAt = t + (state.paced ? kPacedMs : kUnpacedMs);
        csdebug() << "BACKFILL: asking " << keyText(*peer) << " for " << request.size() << " blocks " << request.front() << ".."
                  << request.back();
        if (hooks_.sendRequest) {
            hooks_.sendRequest(*peer, request);
        }
    }
    inFlightCount_ = inFlight_.size();
}

// A neighbour that holds the blocks, is not left out, may be asked now and has nothing in flight;
// the next one after the last chosen, in key order, so that the batches spread over the peers.
std::optional<PublicKey> BackwardSync::pickPeer(const Context& ctx, Sequence highest) {
    const uint64_t t = now();
    std::vector<PublicKey> candidates;
    for (const auto& p : ctx.peers) {
        if (p.lastSeq < highest || inFlight_.count(p.key) != 0) {
            continue;
        }
        const auto it = peers_.find(p.key);
        if (it != peers_.end() && (it->second.markedUntil > t || it->second.nextRequestAt > t)) {
            continue;
        }
        candidates.push_back(p.key);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end());
    auto it = candidates.begin();
    if (lastPicked_) {
        it = std::upper_bound(candidates.begin(), candidates.end(), *lastPicked_);
        if (it == candidates.end()) {
            it = candidates.begin();
        }
    }
    lastPicked_ = *it;
    return *it;
}

void BackwardSync::onBlocks(const PublicKey& sender, PoolsBlock&& blocks) {
    const auto it = inFlight_.find(sender);
    if (it == inFlight_.end()) {
        dropped_ += blocks.size();
        return;
    }
    Batch& batch = it->second;
    const size_t limit = kBufferBatches * settings_.batch;
    for (auto& block : blocks) {
        const Sequence seq = block.sequence();
        if (!block.is_valid() || batch.missing.count(seq) == 0) {
            ++dropped_;
            continue;
        }
        if (buffer_.size() >= limit && buffer_.count(seq) == 0) {
            // left missing: asked for again when the batch times out
            ++dropped_;
            if (!bufferFullLogged_) {
                bufferFullLogged_ = true;
                log("BACKFILL: " + std::to_string(buffer_.size()) + " blocks wait to be stored: the others of " + keyText(sender) +
                    " are dropped");
            }
            continue;
        }
        buffer_[seq] = Buffered{std::move(block), sender};
        batch.missing.erase(seq);
    }
    if (batch.missing.empty()) {
        inFlight_.erase(it);
        auto& state = peers_[sender];
        if (state.paced && !state.awaitingAnswer) {
            // served without AwaitAnswer: its anti-duplicate log is not active (any more)
            state.paced = false;
            csdebug() << "BACKFILL: " << keyText(sender) << " served a batch without AwaitAnswer: no pacing";
        }
        state.awaitingAnswer = false;
        state.failures = 0;
        state.nextRequestAt = now() + (state.paced ? kPacedMs : kUnpacedMs);
    }
    inFlightCount_ = inFlight_.size();
    bufferedCount_ = buffer_.size();
    drain();
}

void BackwardSync::onSyncroMessage(const PublicKey& sender, SyncroMessage msg) {
    if (!started_ || done_) {
        return;
    }
    const bool ours = inFlight_.count(sender) != 0;
    // AwaitAnswer tells whether a peer's anti-duplicate log is active, whatever request it answers;
    // the others concern the backfill only when it has a batch with that peer
    if (!ours && msg != SyncroMessage::AwaitAnswer) {
        return;
    }
    auto& state = peers_[sender];
    const Sequence highest = ours && !inFlight_[sender].missing.empty() ? *inFlight_[sender].missing.rbegin() : next_;
    switch (msg) {
        case SyncroMessage::AwaitAnswer:
            if (!state.paced) {
                state.paced = true;
                log("BACKFILL: peer " + keyText(sender) + " answers with AwaitAnswer: one batch every 6 s");
            }
            if (ours) {
                state.awaitingAnswer = true;
            }
            break;
        case SyncroMessage::NoSuchBlocks:
            if (!ours) {
                break;
            }
            if (state.awaitingAnswer) {
                // its anti-duplicate log keeps our request as AwaitAnswer for ever: every lower one is refused
                mark(sender, kForever, "has no blocks up to " + std::to_string(highest) + " and keeps our request in its log: not used again");
            }
            else {
                mark(sender, now() + kMarkMs, "has no blocks up to " + std::to_string(highest));
            }
            reassign(sender);
            break;
        case SyncroMessage::DuplicatedRequest:
            if (!ours) {
                break;
            }
            state.paced = true;
            state.nextRequestAt = now() + kDuplicatedDelayMs;
            log("BACKFILL: peer " + keyText(sender) + " refused a request as a duplicate: one batch every 6 s, the next in 10 s");
            reassign(sender);
            break;
        case SyncroMessage::IncorrectRequest:
            if (!ours) {
                break;
            }
            mark(sender, now() + kMarkMs, "refused a request as incorrect");
            reassign(sender);
            break;
        default:
            break;
    }
}

void BackwardSync::mark(const PublicKey& peer, uint64_t until, const std::string& why) {
    auto& state = peers_[peer];
    state.markedUntil = std::max(state.markedUntil, until);
    log("BACKFILL: peer " + keyText(peer) + " " + why + (until == kForever ? "" : ": marked for 1 h"));
}

// the peer's batch goes back to the blocks asked for again
void BackwardSync::reassign(const PublicKey& peer) {
    const auto it = inFlight_.find(peer);
    if (it == inFlight_.end()) {
        return;
    }
    if (!it->second.missing.empty()) {
        auto& again = retry_[*it->second.missing.rbegin()];
        again.insert(it->second.missing.begin(), it->second.missing.end());
    }
    inFlight_.erase(it);
    inFlightCount_ = inFlight_.size();
}

void BackwardSync::expire() {
    const uint64_t t = now();
    std::vector<PublicKey> late;
    for (const auto& [peer, batch] : inFlight_) {
        if (t >= batch.sentAt + kBatchTimeoutMs) {
            late.push_back(peer);
        }
    }
    for (const auto& peer : late) {
        auto& state = peers_[peer];
        state.nextRequestAt = t + kSlowPeerDelayMs;
        csdebug() << "BACKFILL: " << keyText(peer) << " did not send " << inFlight_[peer].missing.size() << " blocks in 30 s";
        if (++state.failures >= kFailuresToMark) {
            state.failures = 0;
            mark(peer, t + kMarkMs, "did not answer twice");
        }
        reassign(peer);
    }
}

std::string BackwardSync::verify(const csdb::Pool& block) const {
    if (!block.is_valid() || block.sequence() != next_) {
        return "sequence";
    }
    if (expectedHash_.is_empty() || block.hash() != expectedHash_) {
        return "hash";
    }
    std::string why;
    if (!BlockVerifier::verifySignatures(block, why)) {
        return "signatures: " + why;
    }
    return {};
}

// A block that fails its checks: every block that peer sent and that waits here goes, the peer is
// left out for an hour, its blocks are asked for again from others.
void BackwardSync::reject(const PublicKey& peer, Sequence seq, const std::string& why) {
    ++rejected_;
    std::set<Sequence> again;
    for (auto it = buffer_.begin(); it != buffer_.end();) {
        if (it->second.peer == peer) {
            again.insert(it->first);
            it = buffer_.erase(it);
        }
        else {
            ++it;
        }
    }
    if (!again.empty()) {
        retry_[*again.rbegin()].insert(again.begin(), again.end());
    }
    reassign(peer);
    log("BACKFILL: block " + std::to_string(seq) + " from peer " + keyText(peer) + " rejected (" + why + "): batch requested again", true);
    auto& state = peers_[peer];
    ++state.failures;
    mark(peer, now() + kMarkMs, "sent a block that does not verify");
    bufferedCount_ = buffer_.size();
}

// Stores the buffered blocks strictly downwards, from the next one, as long as they verify.
void BackwardSync::drain() {
    const uint64_t t = now();
    if (storeFailing_ && t < retryStoreAt_) {
        return;
    }
    if (buffer_.count(next_) == 0) {
        return;
    }
    // pruning may have moved the bottom of a pruned node's window above the next block
    if (hooks_.floor) {
        floor_ = std::max(floor_, hooks_.floor());
        if (next_ < floor_) {
            finish();
            return;
        }
    }
    const auto startedAt = std::chrono::steady_clock::now();
    size_t storedNow = 0;
    while (!done_ && storedNow < settings_.storePerTurn) {
        const auto it = buffer_.find(next_);
        if (it == buffer_.end()) {
            break;
        }
        const std::string why = verify(it->second.pool);
        if (!why.empty()) {
            reject(it->second.peer, next_, why);
            continue;
        }
        if (!hooks_.store || !hooks_.store(it->second.pool)) {
            if (!storeFailing_) {
                log("BACKFILL: block " + std::to_string(next_) + " could not be stored: trying again in 60 s", true);
            }
            storeFailing_ = true;
            retryStoreAt_ = t + kStoreRetryMs;
            break;
        }
        if (storeFailing_) {
            storeFailing_ = false;
            log("BACKFILL: storing again from block " + std::to_string(next_));
        }
        // the round confirmations of the block stored before are signed by the confidants of this
        // one: inside the hash, so a mismatch is the chain's, not the peer's. Counted, never refused
        if (lastStored_) {
            std::string bad;
            if (!BlockVerifier::verifyConfirmations(*lastStored_, it->second.pool.confidants(), bad)) {
                const size_t n = ++confirmationErrors_;
                if (n <= 10 || n % 10'000 == 0) {
                    log("BACKFILL: the round confirmations of block " + std::to_string(lastStored_->sequence()) +
                        " do not verify against the confidants of block " + std::to_string(next_) + " (" + bad + "), " +
                        std::to_string(n) + " so far", true);
                }
            }
        }
        lastStored_ = std::move(it->second.pool);
        expectedHash_ = lastStored_->previous_hash();
        buffer_.erase(it);
        first_ = next_;
        ++stored_;
        ++storedNow;
        ++storedSinceFlush_;
        if (next_ <= floor_) {
            finish();
            break;
        }
        --next_;
    }
    bufferedCount_ = buffer_.size();
    const auto us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startedAt).count());
    busyUs_ += us;
    if (us > longestTurnUs_) {
        longestTurnUs_ = us;
    }
    flushIfDue(false);
}

void BackwardSync::flushIfDue(bool force) {
    if (storedSinceFlush_ == 0) {
        return;
    }
    const uint64_t t = now();
    if (!force && storedSinceFlush_ < kFlushBlocks && t < lastFlushAt_ + kFlushMs) {
        return;
    }
    const bool ok = !hooks_.flush || hooks_.flush();
    lastFlushAt_ = t;
    log("BACKFILL: flushed, " + std::to_string(storedSinceFlush_) + " blocks since the last flush, down to #" +
            std::to_string(first_.load()) + (ok ? "" : ": the storage flush failed"),
        !ok);
    storedSinceFlush_ = 0;
}

void BackwardSync::finish() {
    done_ = true;
    flushIfDue(true);
    const std::string cost = ", " + std::to_string(stored_.load()) + " blocks stored in " + std::to_string(busyUs_ / 1000) +
                             " ms of processor thread, longest turn " + std::to_string(longestTurnUs_ / 1000) + " ms";
    if (floor_ == 0) {
        log("BACKFILL: full history stored, blocks 0.." + std::to_string(head_) + " and later" + cost);
    }
    else {
        log("BACKFILL: stored down to the floor " + std::to_string(floor_) + " (pruned node)" + cost);
    }
    inFlight_.clear();
    buffer_.clear();
    retry_.clear();
    inFlightCount_ = 0;
    bufferedCount_ = 0;
}

}  // namespace cs
