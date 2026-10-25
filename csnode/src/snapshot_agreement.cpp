#include <csnode/snapshot_agreement.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include <boost/asio/ip/address.hpp>

#include <base58.h>

namespace cs {

namespace {
std::string hostPart(const std::string& address) {
    if (!address.empty() && address.front() == '[') {
        const auto close = address.find(']');
        return close == std::string::npos ? std::string() : address.substr(1, close - 1);
    }
    // a.b.c.d:port; an IPv6 address has more than one ':'
    const auto colon = address.find(':');
    if (colon != std::string::npos && address.find(':', colon + 1) == std::string::npos) {
        return address.substr(0, colon);
    }
    return address;
}

std::string unitOf(const SourceManifest& s) {
    if (s.trusted) {
        return "key:" + EncodeBase58(s.key.data(), s.key.data() + s.key.size());
    }
    const auto prefix = netPrefix(s.address);
    return prefix.empty() ? std::string("unknown address") : "net:" + prefix;
}

size_t countUnits(const std::vector<size_t>& indices, const std::vector<std::string>& units) {
    std::set<std::string> distinct;
    for (auto i : indices) {
        distinct.insert(units[i]);
    }
    return distinct.size();
}

std::string shortHex(const std::string& hex) {
    return hex.size() > 16 ? hex.substr(0, 16) + "..." : hex;
}
}  // namespace

std::string netPrefix(const std::string& address) {
    boost::system::error_code ec;
    auto ip = boost::asio::ip::make_address(hostPart(address), ec);
    if (ec) {
        return {};
    }
    if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
        ip = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, ip.to_v6());
    }
    if (ip.is_v4()) {
        const auto b = ip.to_v4().to_bytes();
        return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]);
    }
    auto b = ip.to_v6().to_bytes();
    std::fill(b.begin() + 6, b.end(), 0);
    return boost::asio::ip::address_v6(b).to_string() + "/48";
}

AgreementResult agree(const std::vector<SourceManifest>& sources, size_t minSources) {
    minSources = std::max<size_t>(minSources, 1);
    AgreementResult result;

    std::vector<std::string> units(sources.size());
    std::set<cs::PublicKey> seen;
    using BlockKey = std::pair<cs::Sequence, std::string>;
    using DigestKey = std::pair<std::string, std::string>;
    std::map<BlockKey, std::map<DigestKey, std::vector<size_t>>> blocks;
    for (size_t i = 0; i < sources.size(); ++i) {
        units[i] = unitOf(sources[i]);
        if (!seen.insert(sources[i].key).second) {
            continue;
        }
        const auto& m = sources[i].manifest;
        blocks[{m.seq, m.headHash}][{m.stateDigest, m.settingsDigest}].push_back(i);
    }

    struct Block {
        BlockKey key;
        size_t independent = 0;
        std::vector<AgreementGroup> groups;
    };
    std::vector<Block> ordered;
    for (const auto& [blockKey, digests] : blocks) {
        Block b;
        b.key = blockKey;
        std::vector<size_t> all;
        for (const auto& [digestKey, indices] : digests) {
            AgreementGroup g;
            g.seq = blockKey.first;
            g.headHash = blockKey.second;
            g.stateDigest = digestKey.first;
            g.settingsDigest = digestKey.second;
            g.sources = indices;
            for (auto i : indices) {
                g.keys.push_back(sources[i].key);
            }
            g.independent = countUnits(indices, units);
            all.insert(all.end(), indices.begin(), indices.end());
            b.groups.push_back(std::move(g));
        }
        b.independent = countUnits(all, units);
        std::stable_sort(b.groups.begin(), b.groups.end(),
                         [](const AgreementGroup& x, const AgreementGroup& y) { return x.independent > y.independent; });
        ordered.push_back(std::move(b));
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Block& x, const Block& y) {
        return x.key.first != y.key.first ? x.key.first > y.key.first : x.independent > y.independent;
    });

    for (auto& b : ordered) {
        if (b.independent < minSources) {
            continue;
        }
        auto& top = b.groups.front();
        if (top.independent < minSources) {
            continue;
        }
        if (b.groups.size() > 1 && b.groups[1].independent == top.independent) {
            continue;   // two digests with as many sources for one block: a conflict, not an agreement
        }
        top.chosen = true;
        result.chosen = top;
        break;
    }

    size_t best = 0;
    for (auto& b : ordered) {
        for (auto& g : b.groups) {
            best = std::max(best, g.independent);
            result.groups.push_back(std::move(g));
        }
    }
    result.shortfall = result.chosen ? 0 : minSources - std::min(best, minSources);
    return result;
}

std::string describe(const AgreementGroup& group) {
    std::ostringstream out;
    out << "block " << group.seq << " hash " << shortHex(group.headHash) << " digest " << shortHex(group.stateDigest) << " : "
        << group.independent << (group.independent == 1 ? " source" : " sources") << " (keys";
    for (const auto& k : group.keys) {
        out << ' ' << EncodeBase58(k.data(), k.data() + k.size()).substr(0, 8) << "...";
    }
    out << ')';
    return out.str();
}

}  // namespace cs
