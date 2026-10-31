#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <base58.h>
#include <config.hpp>

namespace {
SnapshotData parse(const std::string& ini) {
    std::istringstream in(ini);
    boost::property_tree::ptree tree;
    boost::property_tree::read_ini(in, tree);
    return Config::parseSnapshotData(tree);
}

// a node key that decodes to 32 bytes, and the same bytes for the comparison
const char* const kKeyA = "3rUevsW5xfob6qDxWMDFwwTQCq39SYhzstuyfUGSDvF2";
const char* const kKeyB = "ESWr7ZThCmP2DVNNgrcsCvZZ2rrhDnFRW8oUFEpKpgdD";

cs::PublicKey decoded(const char* key) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(DecodeBase58(key, bytes));
    EXPECT_EQ(bytes.size(), 32u);
    cs::PublicKey result{};
    std::copy(bytes.begin(), bytes.end(), result.begin());
    return result;
}
}  // namespace

TEST(SnapshotConfig, DefaultsWithoutTheSection) {
    const auto s = parse("[params]\nipv6=false\n");
    EXPECT_EQ(s, SnapshotData{});
    EXPECT_FALSE(s.serve);
    EXPECT_EQ(s.serveRateKbps, 2000u);
    EXPECT_EQ(s.serveMaxPeers, 8u);
    EXPECT_EQ(s.snapshotBlocks, 101000u);
    EXPECT_EQ(s.fetch, "auto");
    EXPECT_EQ(s.fetchWait, 600u);
    EXPECT_EQ(s.fetchFallback, "sync");
    EXPECT_EQ(s.minSources, 2u);
    EXPECT_TRUE(s.sources.empty());
    EXPECT_EQ(s.sourcesFile, "snapshot_sources.txt");
    EXPECT_EQ(s.fetchRateKbps, 0u);
}

TEST(SnapshotConfig, EveryKeyIsRead) {
    const auto s = parse(std::string(
        "[snapshot]\n"
        "serve=on\n"
        "serve_rate_kbps=4000\n"
        "serve_max_peers=3\n"
        "snapshot_blocks=50000\n"
        "fetch=off\n"
        "fetch_wait=60\n"
        "fetch_fallback=wait\n"
        "min_sources=3\n"
        "sources=") + kKeyA + "," + kKeyB + "\n"
        "sources_file=mine.txt\n"
        "fetch_rate_kbps=1000\n");
    EXPECT_TRUE(s.serve);
    EXPECT_EQ(s.serveRateKbps, 4000u);
    EXPECT_EQ(s.serveMaxPeers, 3u);
    EXPECT_EQ(s.snapshotBlocks, 50000u);
    EXPECT_EQ(s.fetch, "off");
    EXPECT_EQ(s.fetchWait, 60u);
    EXPECT_EQ(s.fetchFallback, "wait");
    EXPECT_EQ(s.minSources, 3u);
    ASSERT_EQ(s.sources.size(), 2u);
    EXPECT_EQ(s.sources[0], decoded(kKeyA));
    EXPECT_EQ(s.sources[1], decoded(kKeyB));
    EXPECT_EQ(s.sourcesFile, "mine.txt");
    EXPECT_EQ(s.fetchRateKbps, 1000u);
}

TEST(SnapshotConfig, OneBlockSnapshotsAreAccepted) {
    EXPECT_EQ(parse("[snapshot]\nsnapshot_blocks=1\n").snapshotBlocks, 1u);
}

TEST(SnapshotConfig, BadValuesKeepTheDefaults) {
    const auto s = parse(
        "[snapshot]\n"
        "serve=maybe\n"
        "serve_max_peers=0\n"
        "snapshot_blocks=0\n"
        "fetch=sometimes\n"
        "fetch_wait=86401\n"
        "fetch_fallback=github\n"
        "min_sources=-1\n"
        "fetch_rate_kbps=fast\n");
    EXPECT_EQ(s, SnapshotData{});
    EXPECT_EQ(parse("[snapshot]\nserve_max_peers=65\n"), SnapshotData{});
}

TEST(SnapshotConfig, BadSourcesAreDropped) {
    const auto s = parse(std::string("[snapshot]\nsources=not-base58, ") + kKeyA +
                         " ,3rUevsW5xfob6qDx," + kKeyA + "\n");
    ASSERT_EQ(s.sources.size(), 1u);
    EXPECT_EQ(s.sources[0], decoded(kKeyA));
}

TEST(SnapshotConfig, EmptySourcesFileTurnsItOff) {
    EXPECT_TRUE(parse("[snapshot]\nsources_file=\n").sourcesFile.empty());
}

TEST(SnapshotConfig, RemovedKeysAreIgnored) {
    const auto s = parse(
        "[snapshot]\n"
        "serve_port=9511\n"
        "serve_bind=127.0.0.1\n"
        "serve_max_connections=3\n"
        "probe_known_hosts=false\n"
        "github_manifest=https://example.org/latest.json\n"
        "fallback=none\n"
        "fetch_connections=8\n");
    EXPECT_EQ(s, SnapshotData{});
}

TEST(SnapshotConfig, EveryKeyTakesPartInTheComparison) {
    const SnapshotData base;
    for (const char* line : {"serve=on", "serve_rate_kbps=1", "serve_max_peers=1", "snapshot_blocks=1",
                             "fetch=off", "fetch_wait=1", "fetch_fallback=wait", "min_sources=1",
                             "sources_file=x", "fetch_rate_kbps=1"}) {
        EXPECT_NE(parse(std::string("[snapshot]\n") + line + "\n"), base) << line;
    }
    EXPECT_NE(parse(std::string("[snapshot]\nsources=") + kKeyA + "\n"), base);
    EXPECT_EQ(parse("[snapshot]\nserve=on\n"), parse("[snapshot]\nserve=true\n"));
}
