#include <csnode/snapshot_manifest.hpp>

#include <algorithm>
#include <limits>
#include <regex>
#include <sstream>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>
#include <cscrypto/cscrypto.hpp>
#include <sodium.h>

namespace cs {

namespace {
const std::vector<std::string> kCheckpointFiles = {
    "walletscache.dat", "walletsids.dat", "smartcontracts.dat", "blockchain.dat", "roundstat.dat",
    "tokens.dat", "apihandler.dat", "consensus_pending.dat", "head.bin", "quick_start_hashes.dat",
    "schema_version", "sentinel.bin", "state_digest.bin"};

std::string toHex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 15]);
    }
    return out;
}

bool fromHex(const std::string& hex, cs::Bytes& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    return true;
}

bool isHex(const std::string& s, size_t length) {
    return s.size() == length && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// values the manifest prints into JSON strings: no character that needs escaping
bool isPlain(const std::string& s, size_t maxLength) {
    return s.size() <= maxLength && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= 0x20 && c < 0x7f && c != '"' && c != '\\';
    });
}

bool fail(std::string* error, const std::string& reason) {
    if (error) {
        *error = reason;
    }
    return false;
}
}  // namespace

std::string sha256Hex(const uint8_t* data, size_t size) {
    std::array<uint8_t, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), data, size);
    return toHex(hash.data(), hash.size());
}

const std::vector<std::string>& SnapshotManifest::checkpointFileNames() {
    return kCheckpointFiles;
}

bool SnapshotManifest::isKnownFileName(const std::string& name) {
    if (name == "blocks.bin.lz4" || name == "contracts.bin.lz4") {
        return true;
    }
    for (const auto& f : kCheckpointFiles) {
        if (name == "qs-" + f + ".lz4") {
            return true;
        }
    }
    return false;
}

std::string SnapshotManifest::filesHash() const {
    std::string lines;
    // every byte a client checks is covered: whole file and each chunk
    for (const auto& f : files) {
        lines += f.name + " " + std::to_string(f.size) + " " + std::to_string(f.rawSize) + " " + f.sha256;
        for (const auto& c : f.chunks) {
            lines += " " + c;
        }
        lines += "\n";
    }
    return sha256Hex(reinterpret_cast<const uint8_t*>(lines.data()), lines.size());
}

std::string SnapshotManifest::signedLine() const {
    return "credits-snapshot/2 " + std::to_string(seq) + " " + headHash + " " + stateDigest + " " +
           std::to_string(wallets) + " " + settingsDigest + " " + std::to_string(firstBlock) + " " +
           std::to_string(chunkSize) + " " + filesHash();
}

void SnapshotManifest::sign(const cs::PrivateKey& key) {
    const auto publicKey = cscrypto::getMatchingPublic(key);
    nodeKey = EncodeBase58(cs::Bytes(publicKey.begin(), publicKey.end()));
    const auto line = signedLine();
    const auto sig = cscrypto::generateSignature(key, reinterpret_cast<const cs::Byte*>(line.data()), line.size());
    signature = toHex(sig.data(), sig.size());
}

bool SnapshotManifest::verify() const {
    cs::Bytes key;
    if (!DecodeBase58(nodeKey, key) || key.size() != cscrypto::kPublicKeySize) {
        return false;
    }
    cs::Bytes sig;
    if (!isHex(signature, 2 * cscrypto::kSignatureSize) || !fromHex(signature, sig)) {
        return false;
    }
    const auto line = signedLine();
    return cscrypto::verifySignature(sig.data(), key.data(), reinterpret_cast<const cs::Byte*>(line.data()), line.size());
}

const SnapshotFile* SnapshotManifest::file(const std::string& name) const {
    for (const auto& f : files) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::string SnapshotManifest::toJson() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"format\": " << format << ",\n";
    os << "  \"seq\": " << seq << ",\n";
    os << "  \"head_hash\": \"" << headHash << "\",\n";
    os << "  \"state_digest\": \"" << stateDigest << "\",\n";
    os << "  \"wallets\": " << wallets << ",\n";
    os << "  \"settings_digest\": \"" << settingsDigest << "\",\n";
    os << "  \"first_block\": " << firstBlock << ",\n";
    os << "  \"created\": \"" << created << "\",\n";
    os << "  \"node\": \"" << node << "\",\n";
    os << "  \"node_key\": \"" << nodeKey << "\",\n";
    os << "  \"chunk_size\": " << chunkSize << ",\n";
    os << "  \"compression\": \"" << compression << "\",\n";
    os << "  \"files\": [";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        os << (i ? ",\n" : "\n") << "    { \"name\": \"" << f.name << "\", \"size\": " << f.size
           << ", \"raw_size\": " << f.rawSize << ", \"sha256\": \"" << f.sha256 << "\", \"chunks\": [";
        for (size_t c = 0; c < f.chunks.size(); ++c) {
            os << (c ? ", " : "") << "\"" << f.chunks[c] << "\"";
        }
        os << "] }";
    }
    os << "\n  ],\n";
    os << "  \"signed\": \"" << signedLine() << "\",\n";
    os << "  \"signature\": \"" << signature << "\"\n";
    os << "}\n";
    return os.str();
}

std::optional<SnapshotManifest> SnapshotManifest::fromJson(const std::string& json, std::string* error) {
    if (json.size() > kMaxJsonSize) {
        fail(error, "manifest larger than 1 MB");
        return std::nullopt;
    }
    // read_json recurses: a nesting depth this format never has is refused before parsing
    {
        int depth = 0;
        bool inString = false;
        for (size_t i = 0; i < json.size(); ++i) {
            const char c = json[i];
            if (inString) {
                if (c == '\\') ++i;
                else if (c == '"') inString = false;
            }
            else if (c == '"') inString = true;
            else if (c == '{' || c == '[') {
                if (++depth > 8) {
                    fail(error, "nested too deep");
                    return std::nullopt;
                }
            }
            else if (c == '}' || c == ']') --depth;
        }
    }
    boost::property_tree::ptree tree;
    try {
        std::istringstream in(json);
        boost::property_tree::read_json(in, tree);
    }
    catch (const std::exception& e) {
        fail(error, std::string("not JSON: ") + e.what());
        return std::nullopt;
    }

    SnapshotManifest m;
    try {
        m.format = tree.get<uint32_t>("format");
        m.seq = tree.get<cs::Sequence>("seq");
        m.headHash = tree.get<std::string>("head_hash");
        m.stateDigest = tree.get<std::string>("state_digest");
        m.wallets = tree.get<uint64_t>("wallets");
        m.settingsDigest = tree.get<std::string>("settings_digest");
        m.firstBlock = tree.get<cs::Sequence>("first_block");
        m.created = tree.get<std::string>("created");
        m.node = tree.get<std::string>("node");
        m.nodeKey = tree.get<std::string>("node_key");
        m.chunkSize = tree.get<uint64_t>("chunk_size");
        m.compression = tree.get<std::string>("compression");
        m.signature = tree.get<std::string>("signature");
        for (const auto& item : tree.get_child("files")) {
            SnapshotFile f;
            f.name = item.second.get<std::string>("name");
            f.size = item.second.get<uint64_t>("size");
            f.rawSize = item.second.get<uint64_t>("raw_size");
            f.sha256 = item.second.get<std::string>("sha256");
            for (const auto& chunk : item.second.get_child("chunks")) {
                f.chunks.push_back(chunk.second.get_value<std::string>());
            }
            m.files.push_back(std::move(f));
            if (m.files.size() > kMaxFiles) {
                fail(error, "more than 32 files");
                return std::nullopt;
            }
        }
    }
    catch (const std::exception& e) {
        fail(error, std::string("missing or malformed field: ") + e.what());
        return std::nullopt;
    }

    auto reject = [&](const std::string& reason) {
        fail(error, reason);
        return std::nullopt;
    };
    if (m.format != kFormat) return reject("format is not 2");
    if (m.seq == 0 || m.firstBlock > m.seq || m.seq > std::numeric_limits<uint32_t>::max()) return reject("bad block range");
    if (!isHex(m.headHash, 64) || !isHex(m.stateDigest, 64) || !isHex(m.settingsDigest, 64)) return reject("bad hash or digest");
    if (!isHex(m.signature, 128)) return reject("bad signature");
    if (!isPlain(m.created, 32) || !isPlain(m.node, 64) || !isPlain(m.nodeKey, 64)) return reject("bad text field");
    if (m.compression != "lz4") return reject("unknown compression");
    if (m.chunkSize < (64u << 10) || m.chunkSize > kMaxChunkSize) return reject("chunk size out of range");
    if (m.files.empty()) return reject("no files");
    uint64_t total = 0;
    for (const auto& f : m.files) {
        if (!isKnownFileName(f.name)) return reject("unknown file " + f.name.substr(0, 64));
        if (std::count_if(m.files.begin(), m.files.end(), [&](const SnapshotFile& o) { return o.name == f.name; }) != 1) {
            return reject("file listed twice");
        }
        if (!isHex(f.sha256, 64)) return reject("bad file hash");
        if (f.size > kMaxTotalSize || f.rawSize > kMaxTotalSize) return reject("file too large");
        total += f.size;
        if (total > kMaxTotalSize) return reject("snapshot larger than 8 GB");
        const uint64_t chunks = (f.size + m.chunkSize - 1) / m.chunkSize;
        if (f.chunks.size() != chunks) return reject("chunk count does not match the file size");
        for (const auto& c : f.chunks) {
            if (!isHex(c, 64)) return reject("bad chunk hash");
        }
    }
    if (!m.file("blocks.bin.lz4") || !m.file("qs-state_digest.bin.lz4") || !m.file("qs-head.bin.lz4")) {
        return reject("blocks, head or state digest missing");
    }
    if (tree.get<std::string>("signed", "") != m.signedLine()) return reject("signed line does not match the fields");
    if (!m.verify()) return reject("signature does not verify");
    return m;
}

}  // namespace cs
