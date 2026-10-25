#include <csnode/snapshot_fetcher.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <base58.h>
#include <sodium.h>

#include <lib/system/logger.hpp>
#include <net/hostdata.hpp>

namespace cs {

namespace {
namespace fs = std::filesystem;

uint64_t steadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool powerOfTwo(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

std::string megabytes(uint64_t bytes) {
    return std::to_string((bytes + 500'000) / 1'000'000);
}

std::string duration(uint64_t seconds) {
    if (seconds >= 60 && seconds % 60 == 0) {
        return std::to_string(seconds / 60) + " min";
    }
    return std::to_string(seconds) + " s";
}

bool sameFile(const SnapshotFile& a, const SnapshotFile& b) {
    return a.name == b.name && a.size == b.size && a.sha256 == b.sha256 && a.chunks == b.chunks;
}

std::string hexOf(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}
}  // namespace

struct SnapshotFetcher::HashState {
    crypto_hash_sha256_state state;
};

SnapshotFetcher::SnapshotFetcher(const SnapshotData& cfg, fs::path tmpDir, Hooks hooks, bool dryRun)
: cfg_(cfg)
, tmpDir_(std::move(tmpDir))
, hooks_(std::move(hooks))
, dryRun_(dryRun) {
    if (!hooks_.clock) {
        hooks_.clock = steadyMs;
    }
    for (const auto& key : cfg_.sources) {
        addConfigured(key)->trusted = true;
    }
    loadSourcesFile();
}

SnapshotFetcher::~SnapshotFetcher() {
    for (auto& f : files_) {
        closeFile(f);
    }
}

void SnapshotFetcher::log(const std::string& line, bool warning) {
    if (warning) {
        cswarning() << line;
    }
    else {
        cslog() << line;
    }
    if (hooks_.onLog) {
        hooks_.onLog(line);
    }
}

uint64_t SnapshotFetcher::now() const {
    return hooks_.clock();
}

std::string SnapshotFetcher::keyText(const cs::PublicKey& key) const {
    return EncodeBase58(key.data(), key.data() + key.size());
}

SnapshotFetcher::Source* SnapshotFetcher::findSource(const cs::PublicKey& key) {
    auto it = sources_.find(key);
    return it == sources_.end() ? nullptr : &it->second;
}

SnapshotFetcher::Source* SnapshotFetcher::addConfigured(const cs::PublicKey& key) {
    // configured keys have their own room: discovered peers never push them out
    auto& s = sources_[key];
    s.configured = true;
    return &s;
}

bool SnapshotFetcher::isProtected(const cs::PublicKey& key, const Source& s) const {
    if (s.configured || s.outstanding != 0) {
        return true;
    }
    return phase_ == Phase::Fetch && std::any_of(agreedSources_.begin(), agreedSources_.end(),
                                                 [&](const SourceManifest& m) { return m.key == key; });
}

bool SnapshotFetcher::roomFor(const cs::PublicKey& key, const std::string& prefix, const std::string& block) const {
    // an address not known yet is not capped here: it is resolved and checked again with its manifest
    size_t samePrefix = 0;
    size_t sameBlock = 0;
    for (const auto& [k, s] : sources_) {
        if (k == key || s.configured) {
            continue;
        }
        samePrefix += !prefix.empty() && s.prefix == prefix;
        sameBlock += !block.empty() && s.block == block;
    }
    return samePrefix < kMaxPerPrefix && sameBlock < kMaxPerBlock;
}

SnapshotFetcher::Source* SnapshotFetcher::addDiscovered(const cs::PublicKey& key, uint64_t t) {
    if (auto* s = findSource(key)) {
        return s;
    }
    if (auto e = evicted_.find(key); e != evicted_.end()) {
        if (t - e->second < kEvictedMs) {
            return nullptr;
        }
        evicted_.erase(e);
    }
    // peer keys cost nothing: discovered sources are limited per /24 (/48), per /16 (/32) and in
    // all, so that no network can fill the table
    const std::string address = hooks_.addressOf ? hooks_.addressOf(key) : std::string();
    const std::string prefix = netPrefix(address);
    const std::string block = netBlock(address);
    if (!roomFor(key, prefix, block)) {
        return nullptr;
    }
    size_t discovered = 0;
    for (const auto& [k, s] : sources_) {
        discovered += !s.configured;
    }
    if (discovered >= kMaxSources && !evictOne(t)) {
        return nullptr;
    }
    auto& s = sources_[key];
    s.address = address;
    s.prefix = prefix;
    s.block = block;
    s.discoveredAt = t;
    return &s;
}

bool SnapshotFetcher::silent(const Source& s, uint64_t t) const {
    // at least two requests without an answer over more than one request interval, counted from
    // the first request, not from the discovery: a peer that became a neighbour late is not silent
    return !s.configured && s.unanswered >= 2 && t - s.firstUnansweredAt > kSilentMs;
}

void SnapshotFetcher::evict(std::map<cs::PublicKey, Source>::iterator it, uint64_t t) {
    // an evicted key is not taken again for a minute, so that keys that come back at every tick
    // cannot churn the table; longer would only hurt honest keys, since keys cost an attacker nothing
    if (evicted_.size() >= kMaxEvicted) {
        auto oldest = std::min_element(evicted_.begin(), evicted_.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        evicted_.erase(oldest);
    }
    evicted_[it->first] = t;
    sources_.erase(it);
    dirty_ = true;
}

bool SnapshotFetcher::evictOne(uint64_t t) {
    // the least useful discovered source makes room, whatever its prefix: one that is not a
    // neighbour now, then a silent one (silence is a reason only here, when room is needed), then
    // one without a manifest after a minute, then one without a manifest in the most represented prefix, then the oldest of the
    // most represented prefix; never a configured source, one with pieces outstanding or one of the
    // agreed group during a fetch
    std::map<std::string, size_t> perPrefix;
    for (const auto& [k, s] : sources_) {
        if (!s.configured) {
            ++perPrefix[s.prefix];
        }
    }
    size_t most = 0;
    for (const auto& [prefix, n] : perPrefix) {
        most = std::max(most, n);
    }
    auto tier = [&](const cs::PublicKey& key, const Source& s) {
        const bool old = t - s.discoveredAt > kMarkMs;
        const bool crowded = perPrefix[s.prefix] == most;
        if (neighbours_.count(key) == 0) return 0;
        if (silent(s, t)) return 1;
        if (old && !s.manifest) return 2;
        if (crowded && !s.manifest) return 3;
        if (crowded) return 4;
        return 5;
    };
    auto victim = sources_.end();
    int victimTier = 5;
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        if (isProtected(it->first, it->second)) {
            continue;
        }
        const int k = tier(it->first, it->second);
        if (k < victimTier || (k == victimTier && victim != sources_.end() && it->second.discoveredAt < victim->second.discoveredAt)) {
            victim = it;
            victimTier = k;
        }
    }
    if (victim == sources_.end()) {
        return false;
    }
    evict(victim, t);
    return true;
}

void SnapshotFetcher::loadSourcesFile() {
    if (cfg_.sourcesFile.empty()) {
        return;
    }
    std::ifstream in(cfg_.sourcesFile);
    if (!in) {
        return;   // a missing file means no listed sources
    }
    std::string line;
    size_t keys = 0;
    size_t lines = 0;
    while (keys < kMaxSourcesFileKeys && lines < 10 * kMaxSourcesFileKeys && std::getline(in, line)) {
        ++lines;
        if (const auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        std::istringstream words(line);
        std::string word;
        std::string last;
        while (words >> word) {
            last = word;
        }
        if (last.empty()) {
            continue;
        }
        std::vector<uint8_t> bytes;
        if (!DecodeBase58(last, bytes) || bytes.size() != cs::PublicKey{}.size()) {
            log("SNAPSHOT: " + cfg_.sourcesFile + ": not a node key: " + last.substr(0, 64), true);
            continue;
        }
        cs::PublicKey key;
        std::copy(bytes.begin(), bytes.end(), key.begin());
        addConfigured(key);
        ++keys;
    }
    if (keys != 0) {
        log("SNAPSHOT: " + std::to_string(keys) + " source keys read from " + cfg_.sourcesFile);
    }
}

// ---------------------------------------------------------------------------------------------
// discovery and agreement

void SnapshotFetcher::tick(const Peers& peers) {
    if (stopped_ || phase_ == Phase::Import || phase_ == Phase::Done || phase_ == Phase::Fallback) {
        return;
    }
    const uint64_t t = now();
    if (!started_) {
        started_ = true;
        startedAt_ = t;
        lastLogAt_ = t;
        lastWaitLogAt_ = t;
    }
    neighbours_ = peers.neighbours;
    discover(peers, t);

    if (phase_ == Phase::Discover || phase_ == Phase::Collect) {
        askManifests(peers, t);
        if (tryAgree(t)) {
            return;
        }
        logWaiting(t);
        checkWait(t);
        return;
    }

    verifyFiles(t);
    if (phase_ != Phase::Fetch) {
        return;
    }
    expire(t);
    if (phase_ != Phase::Fetch) {
        return;
    }
    fill(t);
    // a fetch always ends: nothing to ask for 2 min, or no new byte for 5 min (sources busy,
    // silent or slow), and it counts as a failed attempt
    if (phase_ == Phase::Fetch && outstanding() == 0 && t - lastSendAt_ >= kStallMs) {
        backToCollect("no piece could be asked for " + duration(kStallMs / 1000));
        return;
    }
    if (phase_ == Phase::Fetch && t - lastByteAt_ >= kNoProgressMs) {
        backToCollect("no data for " + duration(kNoProgressMs / 1000));
        return;
    }
    logProgress(t, false);
}

void SnapshotFetcher::discover(const Peers& peers, uint64_t t) {
    for (const auto& [key, capabilities] : peers.known) {
        if ((capabilities & cs::hostdata::kSnapshotProtocolV1) == 0) {
            continue;
        }
        if (auto* s = addDiscovered(key, t)) {
            s->serving = true;
        }
    }
    std::set<cs::PublicKey> add;
    for (auto& [key, s] : sources_) {
        if (s.banned || neighbours_.count(key) != 0) {
            continue;
        }
        if (!s.added || t - s.addedAt >= kNeighbourRetryMs) {
            s.added = true;
            s.addedAt = t;
            add.insert(key);
        }
    }
    if (!add.empty() && hooks_.addNeighbours) {
        hooks_.addNeighbours(add);
    }
}

void SnapshotFetcher::askManifests(const Peers&, uint64_t t) {
    for (auto& [key, s] : sources_) {
        if (s.banned || neighbours_.count(key) == 0) {
            continue;
        }
        // a reply: ask again after 30 s; silence (a serving node may not answer when its share for
        // idle keys is spent, design 5.3a): again after 10 s, twice, then every 30 s
        const uint64_t interval = (s.awaiting && s.unanswered <= kManifestRetries) ? kManifestRetryMs : kManifestIntervalMs;
        if (s.everAsked && t - s.askedAt < interval) {
            continue;
        }
        if (s.awaiting) {
            ++s.unanswered;
        }
        else {
            s.unanswered = 1;
            s.firstUnansweredAt = t;
        }
        s.everAsked = true;
        s.awaiting = true;
        s.askedAt = t;
        if (!s.loggedAsk) {
            s.loggedAsk = true;
            log("SNAPSHOT: asking " + keyText(key) + " for its manifest");
        }
        if (hooks_.sendManifestRequest) {
            hooks_.sendManifestRequest(key, snapshot_p2p::ManifestRequest{});
        }
        if (phase_ == Phase::Discover) {
            phase_ = Phase::Collect;
        }
    }
}

void SnapshotFetcher::onManifestReply(const cs::PublicKey& sender, const snapshot_p2p::ManifestReply& reply) {
    if (stopped_ || phase_ == Phase::Import || phase_ == Phase::Done || phase_ == Phase::Fallback) {
        ++dropped_;
        return;
    }
    auto* s = findSource(sender);
    if (s == nullptr || !s->awaiting) {
        ++dropped_;
        return;
    }
    s->awaiting = false;
    s->everAnswered = true;
    s->unanswered = 0;
    const uint64_t t = now();
    if (reply.status != snapshot_p2p::ManifestStatus::Ready) {
        // not serving, not synced or still building: whatever it declared before is not served now
        dirty_ = dirty_ || s->manifest.has_value();
        s->manifest.reset();
        return;
    }

    std::string why;
    auto manifest = SnapshotManifest::fromJson(reply.manifestJson, &why);
    if (manifest) {
        if (!manifest->verify()) {
            why = "bad signature";
        }
        else if (manifest->nodeKey != keyText(sender)) {
            why = "signed by another node";
        }
        else if (manifest->chunkSize > snapshot_p2p::kMaxPieceBytes || !powerOfTwo(manifest->chunkSize)) {
            why = "chunk size " + std::to_string(manifest->chunkSize);
        }
    }
    if (!manifest || !why.empty()) {
        ++dropped_;
        dirty_ = dirty_ || s->manifest.has_value();
        s->manifest.reset();
        log("SNAPSHOT: manifest from " + keyText(sender) + " dropped: " + why, true);
        return;
    }
    // the address the table limits and the agreement count are taken from, resolved again now
    const std::string address = hooks_.addressOf ? hooks_.addressOf(sender) : std::string();
    const std::string prefix = netPrefix(address);
    const std::string block = netBlock(address);
    if (!s->configured && !roomFor(sender, prefix, block)) {
        ++dropped_;
        log("SNAPSHOT: manifest from " + keyText(sender) + " dropped: too many sources at " + prefix);
        if (!isProtected(sender, *s)) {
            sources_.erase(sender);
            dirty_ = true;
        }
        // a source in use keeps its entry, its address and the manifest it had
        return;
    }
    s->address = address;
    s->prefix = prefix;
    s->block = block;
    s->filesHash = manifest->filesHash();
    s->manifest = std::move(manifest);
    s->manifestAt = t;
    dirty_ = true;
    // agreement is tried at the next tick, so that the replies to one round of requests count together
}

bool SnapshotFetcher::tryAgree(uint64_t t) {
    if (!dirty_) {
        return false;   // nothing changed since the last agreement that did not lead to a fetch
    }
    dirty_ = false;
    std::vector<SourceManifest> input;
    for (const auto& [key, s] : sources_) {
        if (s.banned || !s.manifest) {
            continue;
        }
        SourceManifest m;
        m.key = key;
        m.address = s.address;
        m.trusted = s.trusted;
        m.manifest = *s.manifest;
        input.push_back(std::move(m));
    }
    agreement_ = agree(input, cfg_.minSources);
    if (!agreement_.chosen) {
        return false;
    }
    if (dryRun_) {
        logGroups(false);
        phase_ = Phase::Done;
        return true;
    }
    agreedSources_.clear();
    for (auto i : agreement_.chosen->sources) {
        agreedSources_.push_back(input[i]);
    }
    if (!startFetch(t)) {
        agreedSources_.clear();
        return phase_ == Phase::Fallback;
    }
    return true;
}

void SnapshotFetcher::logGroups(bool chosenOnly) {
    for (const auto& g : agreement_.groups) {
        if (g.chosen) {
            log("SNAPSHOT: " + describe(g));
        }
        else if (!chosenOnly) {
            log("SNAPSHOT: " + std::string(agreement_.chosen ? "minority: " : "") + describe(g));
        }
    }
}

std::string SnapshotFetcher::seenText() const {
    if (agreement_.groups.empty()) {
        return "no manifest";
    }
    std::string out;
    for (const auto& g : agreement_.groups) {
        if (!out.empty()) {
            out += "; ";
        }
        out += std::to_string(g.independent) + (g.independent == 1 ? " source" : " sources") + " for block " + std::to_string(g.seq);
    }
    return out;
}

void SnapshotFetcher::logWaiting(uint64_t t) {
    const uint64_t waitMs = uint64_t(cfg_.fetchWait) * 1000;
    if (t - startedAt_ >= waitMs || t - lastLogAt_ < kLogIntervalMs) {
        return;
    }
    lastLogAt_ = t;
    size_t serving = 0;
    size_t neighbours = 0;
    for (const auto& [key, s] : sources_) {
        if (s.serving || s.configured) {
            ++serving;
            if (neighbours_.count(key) != 0) {
                ++neighbours;
            }
        }
    }
    size_t unknown = 0;
    for (const auto& [key, s] : sources_) {
        unknown += !s.banned && s.manifest && !s.trusted && netPrefix(s.address).empty();
    }
    log("SNAPSHOT: " + std::to_string(serving) + " peers serve snapshots (" + std::to_string(neighbours) + " neighbours), " +
        std::to_string(manifests()) + " manifests collected, " + std::to_string(unknown) + " not counted: address unknown, " +
        std::to_string(dropped_) + " replies dropped");
    const std::string left = duration((waitMs - (t - startedAt_) + 59'999) / 60'000 * 60) + " left";
    if (agreement_.chosen) {
        log("SNAPSHOT: block " + std::to_string(agreement_.chosen->seq) +
            " is agreed, but no file group has a source left: waiting, " + left);
        return;
    }
    // groups come strongest first
    const AgreementGroup* best = agreement_.groups.empty() ? nullptr : &agreement_.groups.front();
    if (best != nullptr && best->independent >= cfg_.minSources) {
        // as strong as another group: a conflict, not a shortage
        log("SNAPSHOT: conflict: two groups with " + std::to_string(best->independent) + " sources at block " +
            std::to_string(best->seq) + ": waiting, " + left);
        return;
    }
    if (best != nullptr) {
        log("SNAPSHOT: " + std::to_string(best->independent) + (best->independent == 1 ? " source declares" : " sources declare") +
            " block " + std::to_string(best->seq) + " (need " + std::to_string(cfg_.minSources) + "): waiting, " + left);
    }
}

void SnapshotFetcher::checkWait(uint64_t t) {
    if (t - startedAt_ < uint64_t(cfg_.fetchWait) * 1000) {
        return;
    }
    if (dryRun_) {
        logGroups(false);
        phase_ = Phase::Done;
        return;
    }
    const std::string head = "SNAPSHOT: no agreement of " + std::to_string(cfg_.minSources) + " community sources after " +
                             duration((t - startedAt_) / 1000) + " (seen: " + seenText() + ").";
    if (cfg_.fetchFallback == "wait") {
        if (!fallbackLogged_ || t - lastWaitLogAt_ >= kWaitLogIntervalMs) {
            fallbackLogged_ = true;
            lastWaitLogAt_ = t;
            log(head + " Waiting for more sources (fetch_fallback=wait). To start from the published snapshot instead: stop the node "
                       "and run  ./node --download-snapshot  in this folder (Docker: CREDITS_SNAPSHOT=github), then start it again.",
                true);
        }
        return;
    }
    log(head + " Syncing from block 0 (about 35 h on a fast machine). To start from the published snapshot instead: stop the node "
               "and run  ./node --download-snapshot  in this folder (Docker: CREDITS_SNAPSHOT=github), then start it again.",
        true);
    clearInFlight();
    phase_ = Phase::Fallback;
}

// ---------------------------------------------------------------------------------------------
// download

bool SnapshotFetcher::everUsable(const Source& s) const {
    return !s.banned;
}

bool SnapshotFetcher::usable(const Source& s, uint64_t t) const {
    return !s.banned && s.markedUntil <= t && s.busyUntil <= t && s.outstanding < s.budget;
}

bool SnapshotFetcher::startFetch(uint64_t t) {
    if (!chooseTarget(t)) {
        return false;
    }
    phase_ = Phase::Fetch;
    misbehaviour_ = false;
    fetchStartedAt_ = t;
    lastSendAt_ = t;
    lastByteAt_ = t;
    lastProgressAt_ = t;
    lastProgressBytes_ = bytesDone();
    lastProgressDecile_ = bytesTotal() == 0 ? 0 : static_cast<int>(bytesDone() * 10 / bytesTotal());
    logGroups(false);
    log("SNAPSHOT: fetching block " + std::to_string(target_->seq) + ": " + megabytes(bytesTotal()) + " MB in " +
        std::to_string(files_.size()) + " files from " + std::to_string(agreedSources_.size()) + " sources");
    fill(t);
    return true;
}

bool SnapshotFetcher::chooseTarget(uint64_t t) {
    // the files must be those of one signed manifest (the importer checks its signature): among the
    // agreed sources' file lists, the one shared by most usable sources whose every file still has
    // a usable source; each file then comes from every agreed source declaring the same bytes
    const SnapshotManifest* best = nullptr;
    std::vector<std::vector<cs::PublicKey>> bestSources;
    size_t bestOwners = 0;
    size_t bestTotal = 0;
    std::vector<std::string> hashes;   // filesHash of each agreed source, computed once
    for (const auto& a : agreedSources_) {
        hashes.push_back(a.manifest.filesHash());
    }
    std::set<std::string> tried;
    for (size_t c = 0; c < agreedSources_.size(); ++c) {
        const auto& rep = agreedSources_[c].manifest;
        const auto& hash = hashes[c];
        if (!tried.insert(hash).second) {
            continue;
        }
        bool refused = false;
        for (const auto& f : rep.files) {
            refused = refused || badFiles_.count({f.name, f.sha256}) != 0;
        }
        if (refused) {
            continue;
        }
        size_t owners = 0;
        for (size_t o = 0; o < agreedSources_.size(); ++o) {
            const auto* s = findSource(agreedSources_[o].key);
            if (s != nullptr && everUsable(*s) && hashes[o] == hash) {
                ++owners;
            }
        }
        std::vector<std::vector<cs::PublicKey>> perFile;
        size_t total = 0;
        bool dry = false;
        for (const auto& f : rep.files) {
            std::vector<cs::PublicKey> keys;
            for (const auto& other : agreedSources_) {
                const auto* s = findSource(other.key);
                const auto* of = other.manifest.file(f.name);
                if (s != nullptr && everUsable(*s) && of != nullptr && other.manifest.chunkSize == rep.chunkSize && sameFile(*of, f)) {
                    keys.push_back(other.key);
                }
            }
            dry = dry || keys.empty();
            total += keys.size();
            perFile.push_back(std::move(keys));
        }
        if (dry) {
            continue;
        }
        if (best == nullptr || owners > bestOwners || (owners == bestOwners && total > bestTotal)) {
            best = &rep;
            bestSources = std::move(perFile);
            bestOwners = owners;
            bestTotal = total;
        }
    }
    if (best == nullptr) {
        return false;
    }

    clearInFlight();
    std::vector<FileState> next;
    for (size_t i = 0; i < best->files.size(); ++i) {
        const auto& f = best->files[i];
        // equal chunk hashes over equal sizes: the same bytes cut the same way
        auto old = std::find_if(files_.begin(), files_.end(), [&](const FileState& s) { return sameFile(s.file, f); });
        FileState state;
        bool reuse = old != files_.end();
        if (reuse) {
            state = std::move(*old);
            old->fd = -1;
            state.hashing.reset();   // a check in progress starts over
            // what is on disk is checked again before it is handed over
            if (state.verified) {
                state.verified = false;
                state.verifyPending = true;
            }
        }
        else {
            state.file = f;
            state.have.assign(f.chunks.size(), false);
            state.missing = f.chunks.size();
            state.verifyPending = f.chunks.empty();
        }
        state.sources = bestSources[i];
        if (!openFile(state, !reuse)) {
            log("SNAPSHOT: cannot write " + (tmpDir_ / f.name).string() + ": syncing from block 0", true);
            closeFile(state);
            for (auto& n : next) {
                closeFile(n);
            }
            phase_ = Phase::Fallback;
            return false;
        }
        next.push_back(std::move(state));
    }
    for (auto& f : files_) {
        closeFile(f);
    }
    files_ = std::move(next);
    target_ = *best;
    (void)t;
    return true;
}

bool SnapshotFetcher::openFile(FileState& f, bool truncate) {
    if (f.fd >= 0) {
        return true;
    }
    // the name comes from a manifest fromJson accepted, so it is one of the snapshot's fixed file
    // names; checked again here, where it becomes a path
    if (!SnapshotManifest::isKnownFileName(f.file.name) || fs::path(f.file.name).filename() != f.file.name) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(tmpDir_, ec);
    const auto path = tmpDir_ / f.file.name;
    f.fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (truncate ? O_TRUNC : 0), 0600);
    if (f.fd < 0) {
        return false;
    }
    if (::ftruncate(f.fd, static_cast<off_t>(f.file.size)) != 0) {
        closeFile(f);
        return false;
    }
    return true;
}

void SnapshotFetcher::closeFile(FileState& f) {
    if (f.fd >= 0) {
        ::close(f.fd);
        f.fd = -1;
    }
}

uint64_t SnapshotFetcher::pieceLength(const FileState& f, uint32_t chunk) const {
    const uint64_t chunkSize = target_ ? target_->chunkSize : 0;
    const uint64_t offset = uint64_t(chunk) * chunkSize;
    return offset >= f.file.size ? 0 : std::min<uint64_t>(chunkSize, f.file.size - offset);
}

bool SnapshotFetcher::takeTokens(uint64_t cost, uint64_t t) {
    if (cfg_.fetchRateKbps == 0) {
        return true;
    }
    const double rate = double(cfg_.fetchRateKbps) * 1000.0 / 1000.0;   // bytes per ms
    owed_ = std::max(0.0, owed_ - double(t - owedAt_) * rate);
    owedAt_ = t;
    if (owed_ > 0) {
        return false;
    }
    owed_ += double(cost);
    return true;
}

void SnapshotFetcher::fill(uint64_t t) {
    if (stopped_ || phase_ != Phase::Fetch || !target_) {
        return;
    }
    // round-robin over the sources: each usable source gets one piece per pass, from the file it
    // can serve that has the fewest sources, so that a source shared by every file does not take
    // the pieces that only it and few others could serve
    std::vector<cs::PublicKey> keys;
    for (const auto& f : files_) {
        for (const auto& k : f.sources) {
            if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
                keys.push_back(k);
            }
        }
    }
    if (keys.empty()) {
        return;
    }
    const size_t first = rr_++ % keys.size();
    size_t total = outstanding();
    bool sent = true;
    while (sent && total < kPiecesTotal) {
        sent = false;
        for (size_t n = 0; n < keys.size() && total < kPiecesTotal; ++n) {
            const auto& key = keys[(first + n) % keys.size()];
            auto* s = findSource(key);
            if (s == nullptr || !usable(*s, t) || neighbours_.count(key) == 0) {
                continue;
            }
            FileState* pick = nullptr;
            uint32_t pickChunk = 0;
            for (auto& f : files_) {
                if (f.missing == f.inFlight.size() || (pick != nullptr && f.sources.size() >= pick->sources.size()) ||
                    std::find(f.sources.begin(), f.sources.end(), key) == f.sources.end()) {
                    continue;
                }
                uint32_t chunk = 0;
                while (chunk < f.have.size() && (f.have[chunk] || f.inFlight.count(chunk) != 0)) {
                    ++chunk;
                }
                if (chunk < f.have.size()) {
                    pick = &f;
                    pickChunk = chunk;
                }
            }
            if (pick == nullptr) {
                continue;
            }
            if (!takeTokens(pieceLength(*pick, pickChunk), t)) {
                return;
            }
            snapshot_p2p::PieceRequest request;
            request.seq = target_->seq;
            request.name = pick->file.name;
            request.chunk = pickChunk;
            pick->inFlight[pickChunk] = InFlight{key, t};
            ++s->outstanding;
            ++total;
            lastSendAt_ = t;
            sent = true;
            if (hooks_.sendPieceRequest) {
                hooks_.sendPieceRequest(key, request);
            }
        }
    }
}

void SnapshotFetcher::onPieceReply(const cs::PublicKey& sender, snapshot_p2p::PieceReply&& reply) {
    if (stopped_ || phase_ != Phase::Fetch || !target_ || reply.seq != target_->seq) {
        ++dropped_;
        return;
    }
    auto file = std::find_if(files_.begin(), files_.end(), [&](const FileState& f) { return f.file.name == reply.name; });
    if (file == files_.end()) {
        ++dropped_;
        return;
    }
    auto& f = *file;
    auto* s = findSource(sender);
    auto it = f.inFlight.find(reply.chunk);
    bool lateReply = false;
    if (it != f.inFlight.end() && it->second.source == sender) {
        f.inFlight.erase(it);
        if (s != nullptr && s->outstanding != 0) {
            --s->outstanding;
        }
    }
    else {
        // the reply to a piece that timed out on a slow link: still taken while the piece is missing,
        // so that the bytes already on their way are not asked for twice
        auto l = late_.find({reply.name, reply.chunk});
        if (l == late_.end() || l->second != sender || reply.chunk >= f.have.size() || f.have[reply.chunk] ||
            reply.status != snapshot_p2p::PieceStatus::Ok) {
            ++dropped_;
            return;
        }
        late_.erase(l);
        lateReply = true;
    }
    const uint64_t t = now();

    switch (reply.status) {
        case snapshot_p2p::PieceStatus::Busy:
            if (s != nullptr) {
                // at least 500 ms, so that a source answering busy with no delay is not asked at every round trip
                s->busyUntil = t + std::clamp<uint64_t>(reply.retryMs, kMinBusyMs, kMaxBusyMs);
                if (s->busySince == 0) {
                    s->busySince = t;
                }
                else if (t - s->busySince >= kBusyMarkMs) {
                    // busy for a minute without a piece: left alone for a minute, without counting
                    // towards the ban (a loaded honest node is not a faulty one)
                    s->busySince = 0;
                    s->markedUntil = t + kMarkMs;
                    log("SNAPSHOT: source " + keyText(sender) + " busy for " + duration(kBusyMarkMs / 1000) +
                        " without a piece: not asked for 1 min");
                }
            }
            break;
        case snapshot_p2p::PieceStatus::Unknown:
            mark(sender, t, false, "does not have the piece");
            break;
        case snapshot_p2p::PieceStatus::NotServing:
            mark(sender, t, false, "does not serve");
            break;
        case snapshot_p2p::PieceStatus::Ok: {
            const uint64_t length = pieceLength(f, reply.chunk);
            if (reply.data.size() != length || sha256Hex(reply.data.data(), reply.data.size()) != f.file.chunks[reply.chunk]) {
                log("SNAPSHOT: rejected piece " + f.file.name + ":" + std::to_string(reply.chunk) + " from " + keyText(sender), true);
                // mark() may switch to another file group and replace files_: f is not used after it
                mark(sender, t, true, "sent a piece that does not match the manifest");
                break;
            }
            if (f.have[reply.chunk]) {
                break;
            }
            const uint64_t offset = uint64_t(reply.chunk) * target_->chunkSize;
            size_t done = 0;
            while (done < reply.data.size()) {
                const auto n = ::pwrite(f.fd, reply.data.data() + done, reply.data.size() - done, static_cast<off_t>(offset + done));
                if (n <= 0) {
                    log("SNAPSHOT: cannot write " + (tmpDir_ / f.file.name).string() + ": syncing from block 0", true);
                    clearInFlight();
                    phase_ = Phase::Fallback;
                    return;
                }
                done += static_cast<size_t>(n);
            }
            f.have[reply.chunk] = true;
            --f.missing;
            f.doneBytes += length;
            lastByteAt_ = t;
            if (s != nullptr) {
                s->busySince = 0;
                s->delivered = true;
                s->lastPieceAt = t;
                // two pieces in a row within the timeout: back to two pieces at a time
                s->streak = lateReply ? 0 : s->streak + 1;
                if (s->budget < kPiecesPerSource && s->streak >= 2) {
                    s->budget = kPiecesPerSource;
                }
            }
            if (f.missing == 0) {
                f.verifyPending = true;
            }
            break;
        }
    }
    if (phase_ != Phase::Fetch) {
        return;
    }
    fill(t);
    logProgress(t, false);
}

void SnapshotFetcher::releaseSource(const cs::PublicKey& key) {
    for (auto& f : files_) {
        for (auto it = f.inFlight.begin(); it != f.inFlight.end();) {
            if (it->second.source == key) {
                it = f.inFlight.erase(it);
            }
            else {
                ++it;
            }
        }
    }
    if (auto* s = findSource(key)) {
        s->outstanding = 0;
    }
    forgetLate(key);
}

void SnapshotFetcher::forgetLate(const cs::PublicKey& key) {
    for (auto it = late_.begin(); it != late_.end();) {
        it = it->second == key ? late_.erase(it) : std::next(it);
    }
}

void SnapshotFetcher::clearInFlight() {
    // late replies too: after a change of files a chunk index may name other bytes
    late_.clear();
    for (auto& f : files_) {
        f.inFlight.clear();
    }
    for (auto& [key, s] : sources_) {
        s.outstanding = 0;
    }
}

// Keys are not authenticated by the p2p layer (design section 2): a host that spoofs a source's key
// can get that key marked or banned here. The cost is bounded: a source lost for this bootstrap,
// never a wrong snapshot, since every piece is checked against the signed manifest.
void SnapshotFetcher::mark(const cs::PublicKey& key, uint64_t t, bool forever, const std::string& why) {
    auto* s = findSource(key);
    if (s == nullptr) {
        return;
    }
    const bool wasBanned = s->banned;
    misbehaviour_ = misbehaviour_ || phase_ == Phase::Fetch;
    if (forever) {
        s->banned = true;
    }
    else {
        ++s->marks;
        s->markedUntil = t + kMarkMs;
        s->banned = s->marks >= 2;
    }
    dirty_ = dirty_ || (s->banned && !wasBanned);
    log("SNAPSHOT: source " + keyText(key) + " " + why + (s->banned ? ": not asked again" : ": not asked for 1 min"));
    if (!s->banned || phase_ != Phase::Fetch) {
        return;
    }
    // its other pieces go to other sources; a file left without a source restarts from another group
    releaseSource(key);
    for (const auto& f : files_) {
        if (f.missing == 0) {
            continue;
        }
        const bool dry = std::none_of(f.sources.begin(), f.sources.end(), [this](const cs::PublicKey& k) {
            const auto* other = findSource(k);
            return other != nullptr && everUsable(*other);
        });
        if (dry) {
            const auto name = f.file.name;
            if (chooseTarget(t)) {
                log("SNAPSHOT: no source left for " + name + ": switching to the files of another source group");
            }
            else if (phase_ != Phase::Fallback) {
                backToCollect("no source left for " + name);
            }
            return;
        }
    }
}

void SnapshotFetcher::expire(uint64_t t) {
    // the timeout grows with what the source still has to send us: 30 s plus its outstanding bytes
    // at 64 KB/s, so that two large pieces on a slow link are not taken for a stall
    std::map<cs::PublicKey, uint64_t> bytesTo;
    for (const auto& f : files_) {
        for (const auto& [chunk, piece] : f.inFlight) {
            bytesTo[piece.source] += pieceLength(f, chunk);
        }
    }
    std::set<cs::PublicKey> lateSources;
    for (auto& f : files_) {
        for (auto it = f.inFlight.begin(); it != f.inFlight.end();) {
            const uint64_t timeout = kPieceTimeoutMs + bytesTo[it->second.source] * 1000 / kTimeoutBytesPerSecond;
            if (t - it->second.sentAt >= timeout) {
                lateSources.insert(it->second.source);
                if (auto* s = findSource(it->second.source); s != nullptr && s->outstanding != 0) {
                    --s->outstanding;
                }
                if (late_.size() >= kMaxLate) {
                    late_.erase(late_.begin());
                }
                late_[{f.file.name, it->first}] = it->second.source;
                it = f.inFlight.erase(it);
            }
            else {
                ++it;
            }
        }
    }
    for (const auto& key : lateSources) {
        if (phase_ != Phase::Fetch) {
            break;
        }
        auto* s = findSource(key);
        if (s != nullptr && s->delivered && t - s->lastPieceAt < kRecentPieceMs) {
            // it delivers, slowly: one piece at a time until it delivers two in time
            s->streak = 0;
            if (s->budget != 1) {
                s->budget = 1;
                log("SNAPSHOT: source " + keyText(key) + " is slow: one piece at a time");
            }
            continue;
        }
        mark(key, t, false, "did not answer within " + duration((kPieceTimeoutMs + bytesTo[key] * 1000 / kTimeoutBytesPerSecond) / 1000));
    }
}

void SnapshotFetcher::verifyFiles(uint64_t t) {
    // the whole-file SHA-256, at most kHashBytesPerTick per tick so that a large file does not hold
    // the processor thread; a file that does not match is downloaded again
    uint64_t budget = kHashBytesPerTick;
    std::vector<uint8_t> buffer;
    bool bad = false;
    for (auto& f : files_) {
        if (!f.verifyPending) {
            continue;
        }
        if (!f.hashing) {
            f.hashing = std::make_shared<HashState>();
            crypto_hash_sha256_init(&f.hashing->state);
            f.hashed = 0;
        }
        bool ok = f.fd >= 0;
        while (ok && f.hashed < f.file.size && budget != 0) {
            if (buffer.empty()) {
                buffer.resize(1u << 20);
            }
            const auto want = static_cast<size_t>(std::min<uint64_t>({buffer.size(), f.file.size - f.hashed, budget}));
            const auto n = ::pread(f.fd, buffer.data(), want, static_cast<off_t>(f.hashed));
            if (n <= 0) {
                ok = false;
                break;
            }
            crypto_hash_sha256_update(&f.hashing->state, buffer.data(), static_cast<unsigned long long>(n));
            f.hashed += static_cast<uint64_t>(n);
            budget -= static_cast<uint64_t>(n);
        }
        if (ok && f.hashed < f.file.size) {
            return;   // the rest at the next tick
        }
        std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
        crypto_hash_sha256_final(&f.hashing->state, digest.data());
        f.hashing.reset();
        f.verifyPending = false;
        if (ok && hexOf(digest.data(), digest.size()) == f.file.sha256) {
            f.verified = true;
            continue;
        }
        // every piece matched its chunk hash: the file changed on disk after it was written
        log("SNAPSHOT: " + f.file.name + " does not have the SHA-256 of the manifest after its last piece", true);
        f.have.assign(f.file.chunks.size(), false);
        f.missing = f.file.chunks.size();
        f.doneBytes = 0;
        f.verified = false;
        f.verifyPending = f.file.chunks.empty();
        bad = true;
    }
    if (bad) {
        backToCollect("a downloaded file did not verify");
        return;
    }
    const bool complete = std::all_of(files_.begin(), files_.end(), [](const FileState& f) { return f.verified; });
    if (complete && target_) {
        logProgress(t, true);
        std::set<cs::PublicKey> used;
        for (const auto& f : files_) {
            used.insert(f.sources.begin(), f.sources.end());
        }
        log("SNAPSHOT: fetched block " + std::to_string(target_->seq) + ": " + megabytes(bytesTotal()) + " MB from " +
            std::to_string(used.size()) + " sources in " + std::to_string((t - fetchStartedAt_) / 1000) + " s");
        for (auto& f : files_) {
            closeFile(f);
        }
        clearInFlight();
        phase_ = Phase::Import;
    }
}

void SnapshotFetcher::backToCollect(const std::string& why) {
    clearInFlight();
    target_.reset();
    agreedSources_.clear();
    agreement_.chosen.reset();
    dirty_ = true;
    phase_ = Phase::Collect;
    if (misbehaviour_) {
        // sources were banned or marked: remote peers must not be able to push the node to a sync
        // from block 0, so this is not counted
        misbehaviour_ = false;
        log("SNAPSHOT: " + why + " (sources were banned or marked, not counted): looking at the sources again", true);
        return;
    }
    ++fetchFailures_;
    if (cfg_.fetchFallback == "wait") {
        log("SNAPSHOT: " + why + " (failed fetch " + std::to_string(fetchFailures_) +
                ", fetch_fallback=wait): looking at the sources again",
            true);
        return;
    }
    if (fetchFailures_ >= kMaxAttempts) {
        log("SNAPSHOT: " + why + " (failed fetch " + std::to_string(fetchFailures_) + " of " + std::to_string(kMaxAttempts) +
                "): syncing from block 0",
            true);
        phase_ = Phase::Fallback;
        return;
    }
    log("SNAPSHOT: " + why + " (failed fetch " + std::to_string(fetchFailures_) + " of " + std::to_string(kMaxAttempts) +
            "): looking at the sources again",
        true);
}

void SnapshotFetcher::banDeclaring(const std::string& name, const std::string& sha256, const std::string& why) {
    for (const auto& a : agreedSources_) {
        const auto* f = a.manifest.file(name);
        auto* s = findSource(a.key);
        if (f != nullptr && f->sha256 == sha256 && s != nullptr && !s->banned) {
            s->banned = true;
            dirty_ = true;
            log("SNAPSHOT: source " + keyText(a.key) + " " + why + ": not asked again");
        }
    }
}

void SnapshotFetcher::logProgress(uint64_t t, bool force) {
    const uint64_t total = bytesTotal();
    if (!target_ || total == 0) {
        return;
    }
    const uint64_t done = bytesDone();
    const int decile = static_cast<int>(done * 10 / total);
    if (!force && decile <= lastProgressDecile_ && t - lastProgressAt_ < kLogIntervalMs) {
        return;
    }
    const double seconds = double(t - lastProgressAt_) / 1000.0;
    const double rate = seconds > 0 ? double(done - std::min(done, lastProgressBytes_)) / seconds / 1e6 : 0.0;
    std::set<cs::PublicKey> active;
    for (const auto& f : files_) {
        for (const auto& k : f.sources) {
            const auto* s = findSource(k);
            if (s != nullptr && everUsable(*s)) {
                active.insert(k);
            }
        }
    }
    char rateText[32];
    std::snprintf(rateText, sizeof(rateText), "%.1f", rate);
    log("SNAPSHOT: fetching block " + std::to_string(target_->seq) + ": " + std::to_string(done * 100 / total) + " % (" +
        megabytes(done) + " of " + megabytes(total) + " MB), " + std::to_string(active.size()) + " sources, " + rateText + " MB/s");
    lastProgressAt_ = t;
    lastProgressBytes_ = done;
    lastProgressDecile_ = std::max(lastProgressDecile_, decile);
}

// ---------------------------------------------------------------------------------------------

std::optional<std::pair<SnapshotManifest, std::vector<SourceManifest>>> SnapshotFetcher::result() const {
    if (phase_ != Phase::Import || !target_) {
        return std::nullopt;
    }
    return std::make_pair(*target_, agreedSources_);
}

void SnapshotFetcher::onImportFailed(const std::string& fileAtFault) {
    if (stopped_ || phase_ != Phase::Import) {
        return;
    }
    ++importFailures_;
    if (target_ && fileAtFault.empty()) {
        // not a file's fault: the source whose signed manifest this was
        std::vector<uint8_t> bytes;
        if (DecodeBase58(target_->nodeKey, bytes) && bytes.size() == cs::PublicKey{}.size()) {
            cs::PublicKey key;
            std::copy(bytes.begin(), bytes.end(), key.begin());
            if (auto* s = findSource(key); s != nullptr && !s->banned) {
                s->banned = true;
                log("SNAPSHOT: source " + keyText(key) + " signed a snapshot the import refused: not asked again");
            }
        }
    }
    if (target_) {
        if (const auto* f = target_->file(fileAtFault)) {
            badFiles_.insert({f->name, f->sha256});
            // every agreed source that declared these bytes (design 6.7)
            banDeclaring(f->name, f->sha256, "declared " + f->name + " that the import refused");
            files_.erase(std::remove_if(files_.begin(), files_.end(), [&](FileState& s) {
                             if (s.file.name != fileAtFault) {
                                 return false;
                             }
                             closeFile(s);
                             return true;
                         }),
                         files_.end());
        }
    }
    log("SNAPSHOT: the import refused " + (fileAtFault.empty() ? std::string("the snapshot") : fileAtFault) + " (attempt " +
            std::to_string(importFailures_) + " of " + std::to_string(kMaxAttempts) + ")",
        true);
    if (importFailures_ >= kMaxAttempts) {
        log("SNAPSHOT: " + std::to_string(kMaxAttempts) + " imports failed: syncing from block 0", true);
        phase_ = Phase::Fallback;
        return;
    }
    target_.reset();
    agreedSources_.clear();
    agreement_.chosen.reset();
    dirty_ = true;
    phase_ = Phase::Collect;
}

std::vector<GroupReport> SnapshotFetcher::groups() const {
    return agreement_.groups;
}

void SnapshotFetcher::stop() {
    stopped_ = true;
    clearInFlight();
    for (auto& f : files_) {
        closeFile(f);
    }
}

size_t SnapshotFetcher::manifests() const {
    return static_cast<size_t>(
        std::count_if(sources_.begin(), sources_.end(), [](const auto& entry) { return !entry.second.banned && entry.second.manifest; }));
}

size_t SnapshotFetcher::outstanding() const {
    size_t n = 0;
    for (const auto& f : files_) {
        n += f.inFlight.size();
    }
    return n;
}

uint64_t SnapshotFetcher::bytesDone() const {
    uint64_t n = 0;
    for (const auto& f : files_) {
        n += f.doneBytes;
    }
    return n;
}

uint64_t SnapshotFetcher::bytesTotal() const {
    uint64_t n = 0;
    for (const auto& f : files_) {
        n += f.file.size;
    }
    return n;
}

}  // namespace cs
