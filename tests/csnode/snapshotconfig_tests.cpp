#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <config.hpp>

namespace {
SnapshotData parse(const std::string& ini) {
    std::istringstream in(ini);
    boost::property_tree::ptree tree;
    boost::property_tree::read_ini(in, tree);
    return Config::parseSnapshotData(tree);
}
}  // namespace

TEST(SnapshotConfig, DefaultsWithoutTheSection) {
    const auto s = parse("[params]\nipv6=false\n");
    EXPECT_EQ(s, SnapshotData{});
    EXPECT_EQ(s.servePort, 0);
    EXPECT_EQ(s.fetch, "auto");
    EXPECT_EQ(s.minSources, 3u);
    EXPECT_EQ(s.fallback, "github");
    EXPECT_EQ(s.snapshotBlocks, 101000u);
    EXPECT_TRUE(s.probeKnownHosts);
}

TEST(SnapshotConfig, EveryKeyIsRead) {
    const auto s = parse(
        "[snapshot]\n"
        "serve_port=9511\n"
        "serve_bind=127.0.0.1\n"
        "serve_max_connections=3\n"
        "serve_rate_kbps=2000\n"
        "snapshot_blocks=50000\n"
        "fetch=off\n"
        "sources=10.0.0.1:9511,10.0.0.2:9511\n"
        "sources_file=mine.txt\n"
        "probe_known_hosts=false\n"
        "github_manifest=https://example.org/latest.json\n"
        "min_sources=5\n"
        "fallback=none\n"
        "fetch_connections=8\n"
        "fetch_rate_kbps=1000\n");
    EXPECT_EQ(s.servePort, 9511);
    EXPECT_EQ(s.serveBind, "127.0.0.1");
    EXPECT_EQ(s.serveMaxConnections, 3u);
    EXPECT_EQ(s.serveRateKbps, 2000u);
    EXPECT_EQ(s.snapshotBlocks, 50000u);
    EXPECT_EQ(s.fetch, "off");
    EXPECT_EQ(s.sources, "10.0.0.1:9511,10.0.0.2:9511");
    EXPECT_EQ(s.sourcesFile, "mine.txt");
    EXPECT_FALSE(s.probeKnownHosts);
    EXPECT_EQ(s.githubManifest, "https://example.org/latest.json");
    EXPECT_EQ(s.minSources, 5u);
    EXPECT_EQ(s.fallback, "none");
    EXPECT_EQ(s.fetchConnections, 8u);
    EXPECT_EQ(s.fetchRateKbps, 1000u);
}

TEST(SnapshotConfig, BadValuesKeepTheDefaults) {
    const auto s = parse(
        "[snapshot]\n"
        "serve_port=70000\n"
        "serve_max_connections=0\n"
        "snapshot_blocks=10\n"
        "fetch=sometimes\n"
        "probe_known_hosts=maybe\n"
        "min_sources=-1\n"
        "fallback=ask\n"
        "fetch_connections=99\n"
        "fetch_rate_kbps=fast\n");
    EXPECT_EQ(s, SnapshotData{});
}

TEST(SnapshotConfig, EmptyValuesTurnSourcesOff) {
    const auto s = parse("[snapshot]\ngithub_manifest=\nsources_file=\n");
    EXPECT_TRUE(s.githubManifest.empty());
    EXPECT_TRUE(s.sourcesFile.empty());
}

TEST(SnapshotConfig, ADifferentPortIsADifferentConfiguration) {
    const auto a = parse("[snapshot]\nserve_port=9511\n");
    const auto b = parse("[snapshot]\nserve_port=9512\n");
    EXPECT_NE(a, b);
    EXPECT_EQ(a, parse("[snapshot]\nserve_port=9511\n"));
}
