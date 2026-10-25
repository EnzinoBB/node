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

    for (const auto& [blockKey, digests] : blocks) {
        for (const auto& [digestKey, indices] : digests) {
            AgreementGroup g;
            g.seq = blockKey.first;
            g.headHash = blockKey.second;
            g.stateDigest = digestKey.first;
            g.settingsDigest = digestKey.second;
            g.sources = indices;
            for (auto i : indices) {
                g.keys.push_back(sources[i].key);
                g.configured = g.configured || sources[i].trusted;
            }
            g.independent = countUnits(indices, units);
            result.groups.push_back(std::move(g));
        }
    }

    // The rule (see the header): among the groups that reach minSources, a group with a configured
    // key first, then the most independent sources, then the highest block. A higher block alone
    // never wins over more support: min_sources made-up sources at a forged later block lose to a
    // larger honest group at an earlier one. Equal on all three: a conflict, nothing is chosen.
    auto stronger = [](const AgreementGroup& x, const AgreementGroup& y) {
        if (x.configured != y.configured) return x.configured;
        if (x.independent != y.independent) return x.independent > y.independent;
        return x.seq > y.seq;
    };
    auto equal = [](const AgreementGroup& x, const AgreementGroup& y) {
        return x.configured == y.configured && x.independent == y.independent && x.seq == y.seq;
    };
    std::stable_sort(result.groups.begin(), result.groups.end(), stronger);

    const AgreementGroup* best = nullptr;
    for (const auto& g : result.groups) {
        if (g.independent >= minSources) {
            best = &g;
            break;
        }
    }
    const bool conflict = best != nullptr && std::any_of(result.groups.begin(), result.groups.end(), [&](const AgreementGroup& g) {
        return &g != best && g.independent >= minSources && equal(g, *best);
    });
    for (auto& g : result.groups) {
        if (g.independent < minSources) {
            g.reason = "fewer than " + std::to_string(minSources) + " independent sources";
        }
        else if (conflict && equal(g, *best)) {
            g.reason = "as much support as another group: conflict";
        }
        else if (&g == best) {
            g.chosen = true;
        }
        else if (best->configured && !g.configured) {
            g.reason = "no configured source, the strongest group has one";
        }
        else if (g.independent < best->independent) {
            g.reason = "fewer independent sources than the strongest group";
        }
        else {
            g.reason = "lower block than the strongest group, with as much support";
        }
    }
    size_t most = 0;
    for (const auto& g : result.groups) {
        most = std::max(most, g.independent);
        if (g.chosen) {
            result.chosen = g;
        }
    }
    result.shortfall = (result.chosen || conflict) ? 0 : minSources - std::min(most, minSources);
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
    if (!group.reason.empty()) {
        out << ": " << group.reason;
    }
    return out.str();
}

}  // namespace cs
