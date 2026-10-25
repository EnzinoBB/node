#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cscrypto/cscrypto.hpp>
#include <csdb/pool.hpp>
#include <csnode/snapshot_builder.hpp>
#include <csnode/snapshot_server.hpp>
#include <csnode/statedigest.hpp>

namespace {
namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

uint16_t freePort() {
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return a.local_endpoint().port();
}

struct Reply {
    unsigned status = 0;
    std::string body;
    std::string contentRange;
    std::string retryAfter;
};

Reply request(uint16_t port, http::verb verb, const std::string& target, const std::string& range = {}) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
    http::request<http::empty_body> req{verb, target, 11};
    req.set(http::field::host, "localhost");
    req.keep_alive(false);
    if (!range.empty()) req.set(http::field::range, range);
    http::write(socket, req);
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(64u << 20);
    if (verb == http::verb::head) parser.skip(true);
    http::read(socket, buffer, parser);
    Reply r;
    r.status = parser.get().result_int();
    r.body = parser.get().body();
    r.contentRange = std::string(parser.get()[http::field::content_range]);
    r.retryAfter = std::string(parser.get()[http::field::retry_after]);
    return r;
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// a snapshot built by the builder from a small chain, served from root/
struct Fixture {
    fs::path root;
    cs::SnapshotBuildResult built;
    std::atomic<bool> synced{true};
    std::unique_ptr<cs::SnapshotServer> server;
    uint16_t port = 0;

    explicit Fixture(const char* name) : root(fs::temp_directory_path() / name) {
        fs::remove_all(root);
        EXPECT_TRUE(cscrypto::cryptoInit());
        cs::PublicKey pub;
        const auto key = cscrypto::generateKeyPair(pub);
        std::map<cs::Sequence, cs::Bytes> blocks;
        csdb::PoolHash prev;
        cs::Bytes head;
        std::mt19937 random(42);
        for (cs::Sequence s = 0; s <= 120; ++s) {
            csdb::Pool p(prev, s);
            std::string payload(3000, ' ');
            for (auto& c : payload) c = static_cast<char>('a' + random() % 26);   // about 360 KB that lz4 cannot shrink much
            p.add_user_field(1, payload);
            EXPECT_TRUE(p.compose());
            blocks[s] = p.to_binary();
            prev = p.hash();
            head = p.hash().to_binary();
        }
        const fs::path qs = root / "qs" / "120";
        fs::create_directories(qs);
        {
            std::ofstream f(qs / "head.bin", std::ios::binary);
            const uint32_t magic = 0x44414548u;
            const uint64_t seq = 120;
            const uint16_t hl = static_cast<uint16_t>(head.size()), pl = 0;
            f.write(reinterpret_cast<const char*>(&magic), 4);
            f.write(reinterpret_cast<const char*>(&seq), 8);
            f.write(reinterpret_cast<const char*>(&hl), 2);
            f.write(reinterpret_cast<const char*>(head.data()), hl);
            f.write(reinterpret_cast<const char*>(&pl), 2);
        }
        cs::CheckpointDigest d;
        d.wallets = 1;
        EXPECT_TRUE(d.write(qs / cs::CheckpointDigest::kFileName));
        cs::SnapshotBuildInput in;
        in.seq = 120;
        in.checkpointDir = qs;
        in.snapshotBlocks = 120;
        in.chunkSize = 64 << 10;
        in.blockBytes = [blocks](cs::Sequence s, cs::Bytes& out) { out = blocks.at(s); return true; };
        built = cs::buildSnapshot(in, root / "snapshot", key);
        EXPECT_TRUE(built.ok) << built.error;
        server = std::make_unique<cs::SnapshotServer>(root / "snapshot", [this]() {
            return synced ? std::make_pair(cs::Sequence(1000), cs::Sequence(1001)) : std::make_pair(cs::Sequence(10), cs::Sequence(1000));
        });
        server->reload();
        port = freePort();
        cs::SnapshotServer::Settings s;
        s.bind = "127.0.0.1";
        s.port = port;
        server->apply(s);
        EXPECT_TRUE(server->running());
    }
    ~Fixture() {
        server->stop();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string blocksPath() const {
        return "/snapshot/120/blocks.bin.lz4";
    }
};
}  // namespace

TEST(SnapshotServer, ServesTheManifestAndFiles) {
    Fixture fx("snapshot_server_files");
    auto r = request(fx.port, http::verb::get, "/snapshot/latest.json");
    ASSERT_EQ(r.status, 200u);
    EXPECT_NE(r.body.find("\"synced_to\": 1000"), std::string::npos);
    const auto manifest = cs::SnapshotManifest::fromJson(r.body);
    ASSERT_TRUE(manifest.has_value());   // synced_to is outside the signed line
    EXPECT_EQ(manifest->seq, 120u);

    const auto whole = readAll(fx.built.dir / "blocks.bin.lz4");
    r = request(fx.port, http::verb::get, fx.blocksPath());
    ASSERT_EQ(r.status, 200u);
    EXPECT_EQ(r.body, whole);

    r = request(fx.port, http::verb::get, fx.blocksPath(), "bytes=0-99");
    ASSERT_EQ(r.status, 206u);
    EXPECT_EQ(r.body, whole.substr(0, 100));
    EXPECT_EQ(r.contentRange, "bytes 0-99/" + std::to_string(whole.size()));
    r = request(fx.port, http::verb::get, fx.blocksPath(), "bytes=-10");
    EXPECT_EQ(r.body, whole.substr(whole.size() - 10));
    r = request(fx.port, http::verb::get, fx.blocksPath(), "bytes=100-");
    EXPECT_EQ(r.body, whole.substr(100));

    EXPECT_EQ(request(fx.port, http::verb::head, fx.blocksPath()).status, 200u);
}

TEST(SnapshotServer, RefusesWhatItDoesNotServe) {
    Fixture fx("snapshot_server_refuse");
    const auto size = fs::file_size(fx.built.dir / "blocks.bin.lz4");
    EXPECT_EQ(request(fx.port, http::verb::get, fx.blocksPath(), "bytes=" + std::to_string(size) + "-").status, 416u);
    EXPECT_EQ(request(fx.port, http::verb::get, fx.blocksPath(), "bytes=0-1,5-6").status, 416u);
    EXPECT_EQ(request(fx.port, http::verb::get, fx.blocksPath(), "bytes=0-" + std::to_string((16u << 20) + 1)).status, 206u);   // clipped to the file
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/119/blocks.bin.lz4").status, 404u);
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/120/../../qs/120/head.bin").status, 404u);
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/120/NodePrivate.txt").status, 404u);
    EXPECT_EQ(request(fx.port, http::verb::get, "/").status, 404u);
    EXPECT_EQ(request(fx.port, http::verb::post, "/snapshot/latest.json").status, 405u);
}

TEST(SnapshotServer, OnlyASyncedNodeServes) {
    Fixture fx("snapshot_server_synced");
    fx.synced = false;
    const auto r = request(fx.port, http::verb::get, "/snapshot/latest.json");
    EXPECT_EQ(r.status, 503u);
    EXPECT_EQ(r.retryAfter, "30");
    fx.synced = true;
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/latest.json").status, 200u);
}

TEST(SnapshotServer, NothingBuiltIsUnavailable) {
    Fixture fx("snapshot_server_empty");
    fs::remove_all(fx.root / "snapshot");
    fx.server->reload();
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/latest.json").status, 503u);
}

TEST(SnapshotServer, ConnectionCapAndRateAndMove) {
    Fixture fx("snapshot_server_limits");
    cs::SnapshotServer::Settings s;
    s.bind = "127.0.0.1";
    s.port = fx.port;
    s.maxConnections = 1;
    fx.server->apply(s);
    // one connection held open: the next one is refused with 503
    asio::io_context io;
    tcp::socket held(io);
    held.connect(tcp::endpoint(asio::ip::address_v4::loopback(), fx.port));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(request(fx.port, http::verb::get, "/snapshot/latest.json").status, 503u);
    held.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 100 KB/s: 300 KB take about 3 s (the bucket starts empty)
    s.maxConnections = 8;
    s.rateKBps = 100;
    fx.server->apply(s);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = request(fx.port, http::verb::get, fx.blocksPath(), "bytes=0-299999");
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(r.status, 206u);
    EXPECT_GT(secs, 2.0);
    EXPECT_LT(secs, 6.0);

    // moved to another port while running, then stopped by port 0
    s.rateKBps = 0;
    s.port = freePort();
    fx.server->apply(s);
    EXPECT_EQ(fx.server->port(), s.port);
    EXPECT_EQ(request(s.port, http::verb::get, "/snapshot/latest.json").status, 200u);
    s.port = 0;
    fx.server->apply(s);
    EXPECT_FALSE(fx.server->running());
}
