#include <csnode/snapshot_builder.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <limits>
#include <ctime>
#include <fstream>
#include <vector>

#include <lz4.h>
#include <lz4hc.h>
#include <sodium.h>

#include <csdb/pool.hpp>
#include <csnode/caches_serialization_manager.hpp>
#include <csnode/statedigest.hpp>
#include <lib/system/logger.hpp>

namespace cs {

namespace {
constexpr int kLz4Level = 9;   // LZ4HC: 10-30 % above zstd -3 on the mainnet files, measured 2026-10-24

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::string hexOf(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

// writes the served (compressed) bytes of one snapshot file and hashes them: the whole file and
// every chunkSize piece
class HashingWriter {
public:
    HashingWriter(const std::filesystem::path& path, uint64_t chunkSize)
    : out_(path, std::ios::binary | std::ios::trunc), chunkSize_(chunkSize) {
        crypto_hash_sha256_init(&file_);
        crypto_hash_sha256_init(&chunk_);
    }

    bool good() const {
        return static_cast<bool>(out_);
    }

    void write(const uint8_t* data, size_t size) {
        out_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        crypto_hash_sha256_update(&file_, data, size);
        size_ += size;
        while (size > 0) {
            const size_t room = static_cast<size_t>(chunkSize_ - inChunk_);
            const size_t n = std::min(room, size);
            crypto_hash_sha256_update(&chunk_, data, n);
            inChunk_ += n;
            data += n;
            size -= n;
            if (inChunk_ == chunkSize_) {
                closeChunk();
            }
        }
    }

    bool finish(SnapshotFile& file) {
        if (inChunk_ > 0) {
            closeChunk();
        }
        std::array<uint8_t, crypto_hash_sha256_BYTES> h{};
        crypto_hash_sha256_final(&file_, h.data());
        file.sha256 = hexOf(h.data(), h.size());
        file.size = size_;
        file.chunks = std::move(chunks_);
        out_.flush();
        out_.close();
        return !out_.fail();
    }

private:
    void closeChunk() {
        std::array<uint8_t, crypto_hash_sha256_BYTES> h{};
        crypto_hash_sha256_final(&chunk_, h.data());
        chunks_.push_back(hexOf(h.data(), h.size()));
        crypto_hash_sha256_init(&chunk_);
        inChunk_ = 0;
    }

    std::ofstream out_;
    uint64_t chunkSize_;
    uint64_t size_ = 0;
    uint64_t inChunk_ = 0;
    crypto_hash_sha256_state file_;
    crypto_hash_sha256_state chunk_;
    std::vector<std::string> chunks_;
};

// raw data in, lz4 blocks out through a HashingWriter
class Lz4Writer {
public:
    Lz4Writer(const std::filesystem::path& path, uint64_t chunkSize) : out_(path, chunkSize) {
        raw_.reserve(snapshot_lz4::kBlockSize);
        packed_.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(snapshot_lz4::kBlockSize))));
    }

    bool good() const {
        return out_.good();
    }

    void write(const uint8_t* data, size_t size) {
        rawSize_ += size;
        while (size > 0) {
            const size_t n = std::min(snapshot_lz4::kBlockSize - raw_.size(), size);
            raw_.insert(raw_.end(), data, data + n);
            data += n;
            size -= n;
            if (raw_.size() == snapshot_lz4::kBlockSize) {
                flushBlock();
            }
        }
    }

    void write(const std::vector<uint8_t>& data) {
        write(data.data(), data.size());
    }

    bool finish(SnapshotFile& file) {
        if (!raw_.empty()) {
            flushBlock();
        }
        file.rawSize = rawSize_;
        return out_.finish(file) && ok_;
    }

private:
    void flushBlock() {
        const int n = LZ4_compress_HC(reinterpret_cast<const char*>(raw_.data()), reinterpret_cast<char*>(packed_.data()),
                                      static_cast<int>(raw_.size()), static_cast<int>(packed_.size()), kLz4Level);
        if (n <= 0) {
            ok_ = false;
        }
        std::vector<uint8_t> header;
        putU32(header, static_cast<uint32_t>(raw_.size()));
        putU32(header, static_cast<uint32_t>(std::max(n, 0)));
        out_.write(header.data(), header.size());
        out_.write(packed_.data(), static_cast<size_t>(std::max(n, 0)));
        raw_.clear();
    }

    HashingWriter out_;
    std::vector<uint8_t> raw_;
    std::vector<uint8_t> packed_;
    uint64_t rawSize_ = 0;
    bool ok_ = true;
};

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool stopped(const SnapshotBuildInput& in) {
    return in.stop && in.stop->load();
}
}  // namespace

namespace snapshot_lz4 {
bool decompressFile(const std::filesystem::path& file, const std::function<bool(const uint8_t*, size_t)>& sink) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<uint8_t> packed;
    std::vector<uint8_t> raw(kBlockSize);
    const int bound = LZ4_compressBound(static_cast<int>(kBlockSize));
    for (;;) {
        uint8_t header[8];
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (in.gcount() == 0 && in.eof()) {
            return true;
        }
        if (in.gcount() != sizeof(header)) {
            return false;
        }
        const uint32_t rawLen = getU32(header);
        const uint32_t packedLen = getU32(header + 4);
        if (rawLen == 0 || rawLen > kBlockSize || packedLen == 0 || packedLen > static_cast<uint32_t>(bound)) {
            return false;
        }
        packed.resize(packedLen);
        in.read(reinterpret_cast<char*>(packed.data()), packedLen);
        if (static_cast<uint32_t>(in.gcount()) != packedLen) {
            return false;
        }
        const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data()), reinterpret_cast<char*>(raw.data()),
                                          static_cast<int>(packedLen), static_cast<int>(rawLen));
        if (n != static_cast<int>(rawLen) || !sink(raw.data(), rawLen)) {
            return false;
        }
    }
}
}  // namespace snapshot_lz4

bool forEachSnapshotBlock(const std::filesystem::path& blocksFile, const std::function<bool(cs::Sequence, cs::Bytes&&)>& visit,
                          uint64_t maxRawBytes) {
    // records may span lz4 blocks: keep the unparsed tail
    std::vector<uint8_t> pending;
    uint64_t total = 0;
    bool ok = snapshot_lz4::decompressFile(blocksFile, [&](const uint8_t* data, size_t size) {
        total += size;
        if (total > maxRawBytes) {
            return false;
        }
        pending.insert(pending.end(), data, data + size);
        size_t pos = 0;
        while (pending.size() - pos >= 8) {
            const uint32_t seq = getU32(pending.data() + pos);
            const uint32_t len = getU32(pending.data() + pos + 4);
            if (len == 0 || len > (64u << 20)) {
                return false;
            }
            if (pending.size() - pos - 8 < len) {
                break;
            }
            cs::Bytes bytes(pending.begin() + static_cast<ptrdiff_t>(pos + 8), pending.begin() + static_cast<ptrdiff_t>(pos + 8 + len));
            if (!visit(seq, std::move(bytes))) {
                return false;
            }
            pos += 8 + len;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(pos));
        return true;
    });
    return ok && pending.empty();
}

SnapshotBuildResult buildSnapshot(const SnapshotBuildInput& input, const std::filesystem::path& outRoot, const cs::PrivateKey& key) {
    SnapshotBuildResult result;
    const cs::Sequence S = input.seq;
    const auto tmp = outRoot / (std::to_string(S) + ".tmp");
    const auto final = outRoot / std::to_string(S);
    std::error_code ec;

    auto failWith = [&](const std::string& reason) {
        std::filesystem::remove_all(tmp, ec);
        result.ok = false;
        result.error = reason;
        return result;
    };

    if (S == 0 || !input.blockBytes) {
        return failWith("no checkpoint");
    }
    const auto digest = CheckpointDigest::read(input.checkpointDir / CheckpointDigest::kFileName);
    if (!digest) {
        return failWith("checkpoint " + std::to_string(S) + " has no state_digest.bin (saved by an earlier build)");
    }
    const auto head = readCheckpointHead(input.checkpointDir);
    if (!head || head->sequence != S || head->head_hash.empty()) {
        return failWith("checkpoint " + std::to_string(S) + " has no head for that block");
    }
    if (S > std::numeric_limits<uint32_t>::max()) {
        return failWith("block number above 32 bits");
    }
    const cs::Sequence wanted = S > input.snapshotBlocks ? S - input.snapshotBlocks : 0;
    const cs::Sequence F = std::max(wanted, input.firstStored);
    if (F > S) {
        return failWith("the checkpoint's block is not stored");
    }

    // only folders this builder made are replaced or removed: a misdirected outRoot (qs, db, ...)
    // must lose nothing
    auto isBuiltSnapshot = [&](const std::filesystem::path& dir) {
        return std::filesystem::is_regular_file(dir / "snapshot.json", ec);
    };
    if (std::filesystem::exists(final, ec) && !isBuiltSnapshot(final)) {
        return failWith(final.string() + " exists and is not a snapshot");
    }
    // a build interrupted earlier left only snapshot files in <S>.tmp
    for (const auto& entry : std::filesystem::directory_iterator(tmp, ec)) {
        const auto name = entry.path().filename().string();
        if (name != "snapshot.json" && !SnapshotManifest::isKnownFileName(name)) {
            result.error = tmp.string() + " holds " + name + ", which is not a snapshot file";
            return result;
        }
    }
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    if (ec) {
        return failWith("cannot create " + tmp.string() + ": " + ec.message());
    }

    SnapshotManifest& m = result.manifest;
    m.seq = S;
    m.headHash = hexOf(head->head_hash.data(), head->head_hash.size());
    m.stateDigest = hexOf(digest->digest.data(), digest->digest.size());
    m.wallets = digest->wallets;
    m.settingsDigest = hexOf(digest->settings.data(), digest->settings.size());
    m.firstBlock = F;
    m.created = utcNow();
    m.node = input.nodeVersion;
    m.chunkSize = input.chunkSize;

    // 1. the checkpoint's files
    for (const auto& name : SnapshotManifest::checkpointFileNames()) {
        const auto src = input.checkpointDir / name;
        if (!std::filesystem::exists(src, ec)) {
            continue;   // tokens.dat and apihandler.dat exist only in builds with the API
        }
        std::vector<uint8_t> data;
        if (!readFile(src, data)) {
            return failWith("cannot read " + src.string());
        }
        SnapshotFile f;
        f.name = "qs-" + name + ".lz4";
        Lz4Writer w(tmp / f.name, m.chunkSize);
        w.write(data);
        if (!w.finish(f)) {
            return failWith("cannot write " + f.name);
        }
        m.files.push_back(std::move(f));
    }

    // 2. the blocks F..S, a hash chain ending at the checkpoint's head
    {
        SnapshotFile f;
        f.name = "blocks.bin.lz4";
        Lz4Writer w(tmp / f.name, m.chunkSize);
        if (!w.good()) {
            return failWith("cannot write blocks");
        }
        cs::Bytes previousHash;
        std::vector<uint8_t> record;
        for (cs::Sequence seq = F; seq <= S; ++seq) {
            if (stopped(input)) {
                return failWith("stopped");
            }
            cs::Bytes bytes;
            if (!input.blockBytes(seq, bytes) || bytes.empty()) {
                return failWith("block " + std::to_string(seq) + " is not stored");
            }
            const auto pool = csdb::Pool::from_binary(cs::Bytes(bytes));
            if (!pool.is_valid() || pool.sequence() != seq) {
                return failWith("block " + std::to_string(seq) + " does not parse");
            }
            const auto hash = pool.hash().to_binary();
            if (seq > F && pool.previous_hash().to_binary() != previousHash) {
                return failWith("block " + std::to_string(seq) + " does not follow block " + std::to_string(seq - 1));
            }
            if (seq == S && hash != head->head_hash) {
                return failWith("block " + std::to_string(S) + " is not the checkpoint's head");
            }
            previousHash = hash;
            record.clear();
            putU32(record, static_cast<uint32_t>(seq));
            putU32(record, static_cast<uint32_t>(bytes.size()));
            record.insert(record.end(), bytes.begin(), bytes.end());
            w.write(record);
        }
        if (!w.finish(f)) {
            return failWith("cannot write blocks");
        }
        m.files.push_back(std::move(f));
    }

    // 3. contract data, sorted by key
    if (input.contracts) {
        std::vector<std::pair<cs::Bytes, cs::Bytes>> records;
        if (!input.contracts([&](const cs::Bytes& k, const cs::Bytes& v) {
                records.emplace_back(k, v);
                return !stopped(input);
            })) {
            return failWith(stopped(input) ? "stopped" : "cannot read the contract data");
        }
        std::sort(records.begin(), records.end());
        SnapshotFile f;
        f.name = "contracts.bin.lz4";
        Lz4Writer w(tmp / f.name, m.chunkSize);
        std::vector<uint8_t> record;
        for (const auto& [k, v] : records) {
            record.clear();
            putU32(record, static_cast<uint32_t>(k.size()));
            record.insert(record.end(), k.begin(), k.end());
            putU32(record, static_cast<uint32_t>(v.size()));
            record.insert(record.end(), v.begin(), v.end());
            w.write(record);
        }
        if (!w.finish(f)) {
            return failWith("cannot write contracts");
        }
        m.files.push_back(std::move(f));
    }

    // 4. manifest, signed; publish
    m.sign(key);
    {
        std::ofstream out(tmp / "snapshot.json", std::ios::binary | std::ios::trunc);
        out << m.toJson();
        out.flush();
        if (!out) {
            return failWith("cannot write snapshot.json");
        }
    }
    std::filesystem::remove_all(final, ec);
    std::filesystem::rename(tmp, final, ec);
    if (ec) {
        return failWith("cannot rename the snapshot: " + ec.message());
    }
    for (const auto& entry : std::filesystem::directory_iterator(outRoot, ec)) {
        const auto name = entry.path().filename().string();
        if (name != std::to_string(S) && !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) &&
            isBuiltSnapshot(entry.path())) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }
    result.ok = true;
    result.dir = final;
    return result;
}

}  // namespace cs
