#include <csnode/snapshot_server.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <lib/system/logger.hpp>

namespace cs {

namespace {
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;
namespace fs = std::filesystem;

// the unit the per-address cap counts: an IPv4 address (also when written as IPv4-mapped IPv6), or
// the /64 network of an IPv6 address, which one host usually holds whole
std::string peerGroup(const asio::ip::address& address) {
    if (address.is_v6()) {
        const auto v6 = address.to_v6();
        if (v6.is_v4_mapped()) {
            return asio::ip::make_address_v4(asio::ip::v4_mapped, v6).to_string();
        }
        auto bytes = v6.to_bytes();
        std::fill(bytes.begin() + 8, bytes.end(), 0);
        return asio::ip::address_v6(bytes).to_string() + "/64";
    }
    return address.to_string();
}

void setTimeout(tcp::socket& socket, int option, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, option, &tv, sizeof(tv));
}

bool isDigits(const std::string& s) {
    return !s.empty() && s.size() <= 12 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

// "bytes=a-b", "bytes=a-" or "bytes=-n" for a file of size; false for anything else (two ranges too)
bool parseRange(const std::string& header, uint64_t size, uint64_t& first, uint64_t& last) {
    const std::string prefix = "bytes=";
    if (header.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::string spec = header.substr(prefix.size());
    if (spec.find(',') != std::string::npos) {
        return false;
    }
    const auto dash = spec.find('-');
    if (dash == std::string::npos) {
        return false;
    }
    const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
    if (size == 0) {
        return false;
    }
    if (a.empty()) {
        if (!isDigits(b)) return false;
        const uint64_t n = std::stoull(b);
        if (n == 0) return false;
        first = n >= size ? 0 : size - n;
        last = size - 1;
        return true;
    }
    if (!isDigits(a) || (!b.empty() && !isDigits(b))) {
        return false;
    }
    first = std::stoull(a);
    last = b.empty() ? size - 1 : std::min<uint64_t>(std::stoull(b), size - 1);
    return first < size && first <= last;
}

// the bandwidth cap of all connections together
class TokenBucket {
public:
    void setRate(size_t kBps) {
        std::lock_guard<std::mutex> lock(m_);
        rate_ = static_cast<double>(kBps) * 1000.0;
        tokens_ = std::min(tokens_, rate_);
    }
    // waits until n bytes may be sent; false when the server stops meanwhile
    bool take(size_t n, const std::atomic<bool>& stopping) {
        for (;;) {
            if (stopping) {
                return false;
            }
            std::unique_lock<std::mutex> lock(m_);
            if (rate_ <= 0) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            tokens_ = std::min(rate_, tokens_ + rate_ * std::chrono::duration<double>(now - last_).count());
            last_ = now;
            if (tokens_ >= static_cast<double>(n)) {
                tokens_ -= static_cast<double>(n);
                return true;
            }
            const double wait = (static_cast<double>(n) - tokens_) / rate_;
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.2)));
        }
    }

private:
    std::mutex m_;
    double rate_ = 0;
    double tokens_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

struct Loaded {
    SnapshotManifest manifest;
    std::string json;      // as built, with synced_to inserted per request
    fs::path dir;
};
}  // namespace

struct SnapshotServer::Impl : std::enable_shared_from_this<SnapshotServer::Impl> {
    static constexpr int kHeaderDeadlineSec = 10;          // a whole request line and headers
    static constexpr size_t kMaxPerAddress = 2;
    // a reader slower than this on average, over the time spent writing to it (the bandwidth cap's
    // waits not counted), is dropped once it has been written to for kMinRateAfterSec
    static constexpr double kMinBytesPerSec = 32 * 1024;
    static constexpr double kMinRateAfterSec = 10;
    static constexpr int kMaxConnectionSec = 30 * 60;

    fs::path root;
    SyncedTo syncedTo;
    Settings settings;
    std::mutex settingsLock;

    std::shared_ptr<const Loaded> loaded;
    std::mutex loadedLock;

    TokenBucket bucket;
    std::atomic<size_t> maxConnections{8};
    std::atomic<size_t> active{0};
    // every open connection, its peer address and the time its current step must end by: the accept
    // thread closes the ones past it (a client that sends its headers byte by byte holds no slot)
    struct Conn {
        std::string address;
        std::chrono::steady_clock::time_point deadline;
    };
    std::map<std::shared_ptr<tcp::socket>, Conn> sockets;
    std::mutex socketsLock;

    void setDeadline(const std::shared_ptr<tcp::socket>& socket, std::chrono::seconds in) {
        std::lock_guard<std::mutex> lock(socketsLock);
        const auto it = sockets.find(socket);
        if (it != sockets.end()) {
            it->second.deadline = std::chrono::steady_clock::now() + in;
        }
    }

    void closeExpired() {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(socketsLock);
        for (auto& [socket, conn] : sockets) {
            if (now > conn.deadline) {
                boost::system::error_code ec;
                socket->shutdown(tcp::socket::shutdown_both, ec);
            }
        }
    }

    asio::io_context io;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::thread acceptThread;
    std::atomic<bool> stopping{false};

    std::shared_ptr<const Loaded> current() {
        std::lock_guard<std::mutex> lock(loadedLock);
        return loaded;
    }

    bool synced() const {
        if (!syncedTo) {
            return true;
        }
        const auto [lastBlock, round] = syncedTo();
        return lastBlock > 0 && round <= lastBlock + kSyncedRounds;
    }

    template <class Body>
    void finish(http::response<Body>& res, bool keepAlive) {
        res.set(http::field::server, "credits-node");
        res.keep_alive(keepAlive);
        res.prepare_payload();
    }

    // returns false when the connection must end
    // what one connection has used, across its requests: the minimum rate and the time limit apply to
    // the connection, not to each request
    struct Usage {
        uint64_t served = 0;
        double writeSeconds = 0;
        std::chrono::steady_clock::time_point end;
    };

    bool handle(const std::shared_ptr<tcp::socket>& socketPtr, const http::request<http::empty_body>& req, Usage& usage) {
        uint64_t& served = usage.served;
        tcp::socket& socket = *socketPtr;
        const bool keepAlive = req.keep_alive();
        auto simple = [&](http::status status, const std::string& text, bool retry = false) {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::content_type, "text/plain");
            if (retry) res.set(http::field::retry_after, "30");
            res.body() = text + "\n";
            finish(res, keepAlive);
            boost::system::error_code ec;
            http::write(socket, res, ec);
            return !ec && keepAlive;
        };
        if (req.method() != http::verb::get && req.method() != http::verb::head) {
            return simple(http::status::method_not_allowed, "GET or HEAD only");
        }
        const std::string target(req.target());
        const std::string prefix = "/snapshot/";
        if (target.compare(0, prefix.size(), prefix) != 0) {
            return simple(http::status::not_found, "not found");
        }
        const auto state = current();
        if (!synced()) {
            return simple(http::status::service_unavailable, "this node is not synced with the network", true);
        }
        if (!state) {
            return simple(http::status::service_unavailable, "no snapshot built yet", true);
        }
        const std::string rest = target.substr(prefix.size());
        const bool head = req.method() == http::verb::head;
        if (rest == "latest.json") {
            const auto [lastBlock, round] = syncedTo ? syncedTo() : std::pair<cs::Sequence, cs::Sequence>{0, 0};
            (void)round;
            std::string body = state->json;
            // informative, outside the signed line: how far this node is
            const auto brace = body.find('{');
            body.insert(brace + 1, "\n  \"synced_to\": " + std::to_string(lastBlock) + ",");
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = head ? std::string{} : body;
            finish(res, keepAlive);
            if (head) res.content_length(body.size());
            boost::system::error_code ec;
            http::write(socket, res, ec);
            return !ec && keepAlive;
        }
        const auto slash = rest.find('/');
        if (slash == std::string::npos || rest.substr(0, slash) != std::to_string(state->manifest.seq)) {
            return simple(http::status::not_found, "not found");
        }
        const auto* file = state->manifest.file(rest.substr(slash + 1));
        if (!file) {
            return simple(http::status::not_found, "not found");
        }
        const fs::path path = state->dir / file->name;
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return simple(http::status::not_found, "not found");
        }
        const uint64_t size = file->size;
        uint64_t first = 0, last = size == 0 ? 0 : size - 1;
        bool partial = false;
        const auto range = req.find(http::field::range);
        if (range != req.end()) {
            if (!parseRange(std::string(range->value()), size, first, last) || last - first + 1 > kMaxRange) {
                http::response<http::string_body> res{http::status::range_not_satisfiable, req.version()};
                res.set(http::field::content_range, "bytes */" + std::to_string(size));
                res.body() = "one range of at most 16 MiB\n";
                finish(res, keepAlive);
                boost::system::error_code ec;
                http::write(socket, res, ec);
                return !ec && keepAlive;
            }
            partial = true;
        }
        const uint64_t length = size == 0 ? 0 : last - first + 1;
        http::response<http::empty_body> res{partial ? http::status::partial_content : http::status::ok, req.version()};
        res.set(http::field::server, "credits-node");
        res.set(http::field::content_type, "application/octet-stream");
        res.set(http::field::accept_ranges, "bytes");
        if (partial) {
            res.set(http::field::content_range, "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size));
        }
        res.content_length(length);
        res.keep_alive(keepAlive);
        boost::system::error_code ec;
        http::response_serializer<http::empty_body> sr{res};
        http::write_header(socket, sr, ec);
        if (ec) {
            return false;
        }
        if (head || length == 0) {
            return keepAlive;
        }
        // a stalled reader holds no slot: every piece written renews a short deadline
        setDeadline(socketPtr, std::chrono::seconds(kWriteTimeoutSec));
        in.seekg(static_cast<std::streamoff>(first));
        std::vector<char> buffer(64 << 10);
        uint64_t left = length;
        while (left > 0 && !stopping) {
            if (std::chrono::steady_clock::now() >= usage.end) {
                return false;   // the connection's time is up, even in the middle of a body
            }
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
            in.read(buffer.data(), static_cast<std::streamsize>(n));
            if (static_cast<size_t>(in.gcount()) != n) {
                return false;
            }
            if (!bucket.take(n, stopping)) {
                return false;
            }
            const auto t0 = std::chrono::steady_clock::now();
            asio::write(socket, asio::buffer(buffer.data(), n), ec);
            if (ec) {
                return false;
            }
            usage.writeSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (usage.writeSeconds >= kMinRateAfterSec && static_cast<double>(usage.served + n) / usage.writeSeconds < kMinBytesPerSec) {
                return false;   // the reader is too slow to keep a slot
            }
            setDeadline(socketPtr, std::chrono::seconds(kWriteTimeoutSec));
            left -= n;
            served += n;
        }
        return left == 0 && keepAlive;
    }

    void session(std::shared_ptr<tcp::socket> socket) {
        std::string peer;
        {
            boost::system::error_code ec;
            peer = socket->remote_endpoint(ec).address().to_string();
        }
        setTimeout(*socket, SO_RCVTIMEO, kIdleTimeoutSec);
        setTimeout(*socket, SO_SNDTIMEO, kWriteTimeoutSec);
        Usage usage;
        usage.end = std::chrono::steady_clock::now() + std::chrono::seconds(kMaxConnectionSec);
        uint64_t& served = usage.served;
        boost::beast::flat_buffer buffer;
        for (size_t i = 0; i < kMaxRequestsPerConnection && !stopping && std::chrono::steady_clock::now() < usage.end; ++i) {
            http::request_parser<http::empty_body> parser;
            parser.header_limit(8 * 1024);
            setDeadline(socket, std::chrono::seconds(i == 0 ? kHeaderDeadlineSec : kIdleTimeoutSec));
            boost::system::error_code ec;
            http::read(*socket, buffer, parser, ec);
            if (ec) {
                break;
            }
            if (!handle(socket, parser.get(), usage)) {
                break;
            }
        }
        boost::system::error_code ec;
        socket->shutdown(tcp::socket::shutdown_both, ec);
        socket->close(ec);
        if (served >= (1u << 20)) {
            const auto state = current();
            cslog() << "SNAPSHOT: served " << served / (1u << 20) << " MB of block " << (state ? state->manifest.seq : 0) << " to " << peer;
        }
        {
            std::lock_guard<std::mutex> lock(socketsLock);
            sockets.erase(socket);
        }
        --active;
    }

    void refuse(tcp::socket& socket) {
        setTimeout(socket, SO_SNDTIMEO, 5);
        http::response<http::string_body> res{http::status::service_unavailable, 11};
        res.set(http::field::retry_after, "5");
        res.body() = "too many connections\n";
        finish(res, false);
        boost::system::error_code ec;
        http::write(socket, res, ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
        socket.close(ec);
    }

    void acceptLoop() {
        while (!stopping) {
            closeExpired();   // on every pass, whether connections are waiting or not
            auto socket = std::make_shared<tcp::socket>(io);
            boost::system::error_code ec;
            acceptor->accept(*socket, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (ec) {
                if (stopping) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            socket->non_blocking(false, ec);
            if (active.load() >= maxConnections.load()) {
                refuse(*socket);
                continue;
            }
            std::string address;
            {
                boost::system::error_code aec;
                address = peerGroup(socket->remote_endpoint(aec).address());
            }
            bool tooMany = false;
            {
                std::lock_guard<std::mutex> lock(socketsLock);
                const auto same = std::count_if(sockets.begin(), sockets.end(), [&](const auto& kv) { return kv.second.address == address; });
                tooMany = static_cast<size_t>(same) >= kMaxPerAddress;
                if (!tooMany) {
                    sockets.emplace(socket, Conn{address, std::chrono::steady_clock::now() + std::chrono::seconds(kHeaderDeadlineSec)});
                }
            }
            if (tooMany) {
                refuse(*socket);   // outside the lock: it writes, up to its own timeout
                continue;
            }
            ++active;
            // the session keeps the server's state alive until it ends, whatever happens to the server
            std::thread([self = shared_from_this(), socket]() { self->session(socket); }).detach();
        }
    }

    void stopListening() {
        stopping = true;
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        if (acceptor) {
            boost::system::error_code ec;
            acceptor->close(ec);
            acceptor.reset();
        }
        {
            std::lock_guard<std::mutex> lock(socketsLock);
            for (const auto& s : sockets) {
                boost::system::error_code ec;
                s->shutdown(tcp::socket::shutdown_both, ec);
            }
        }
        // the sessions see their sockets shut and stopping set: they end within their current write
        while (active.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        stopping = false;
    }

    // returns the bound port, 0 when the address cannot be used
    uint16_t listen(const Settings& s) {
        boost::system::error_code ec;
        const auto address = s.bind.empty() ? asio::ip::address(asio::ip::address_v6::any()) : asio::ip::make_address(s.bind, ec);
        if (ec) {
            cserror() << "SNAPSHOT: serve_bind " << s.bind << " is not an address";
            return 0;
        }
        acceptor = std::make_unique<tcp::acceptor>(io);
        const tcp::endpoint endpoint(address, s.port);
        acceptor->open(endpoint.protocol(), ec);
        if (!ec && address.is_v6()) {
            acceptor->set_option(asio::ip::v6_only(false), ec);
            ec.clear();
        }
        if (!ec) acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
        if (!ec) acceptor->bind(endpoint, ec);
        if (!ec) acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (!ec) acceptor->non_blocking(true, ec);
        if (ec) {
            cserror() << "SNAPSHOT: cannot listen on " << (s.bind.empty() ? "port " : s.bind + ":") << s.port << ": " << ec.message();
            acceptor.reset();
            return 0;
        }
        const uint16_t bound = acceptor->local_endpoint(ec).port();
        acceptThread = std::thread([this]() { acceptLoop(); });
        return bound;
    }
};

SnapshotServer::SnapshotServer(fs::path root, SyncedTo syncedTo) : impl_(std::make_shared<Impl>()) {
    impl_->root = std::move(root);
    impl_->syncedTo = std::move(syncedTo);
}

SnapshotServer::~SnapshotServer() {
    stop();
}

void SnapshotServer::apply(const Settings& settings) {
    std::lock_guard<std::mutex> lock(impl_->settingsLock);
    impl_->bucket.setRate(settings.rateKBps);
    impl_->maxConnections = std::max<size_t>(1, settings.maxConnections);
    const bool listening = boundPort_.load() != 0;
    const bool sameAddress = listening && settings.bind == impl_->settings.bind && settings.port == impl_->settings.port;
    impl_->settings = settings;
    if (sameAddress) {
        return;
    }
    if (listening) {
        impl_->stopListening();
        cslog() << "SNAPSHOT: service stopped";
        boundPort_ = 0;
    }
    if (settings.port != 0) {
        const uint16_t bound = impl_->listen(settings);
        boundPort_ = bound;
        if (bound != 0) {
            cslog() << "SNAPSHOT: serving on " << (settings.bind.empty() ? std::string("port ") : settings.bind + ":") << bound;
        }
    }
}

void SnapshotServer::reload() {
    std::error_code ec;
    cs::Sequence best = 0;
    for (const auto& entry : fs::directory_iterator(impl_->root, ec)) {
        const auto name = entry.path().filename().string();
        if (isDigits(name) && fs::is_regular_file(entry.path() / "snapshot.json", ec)) {
            best = std::max<cs::Sequence>(best, std::stoull(name));
        }
    }
    std::shared_ptr<const Loaded> next;
    if (best != 0) {
        const fs::path dir = impl_->root / std::to_string(best);
        std::ifstream in(dir / "snapshot.json", std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        auto manifest = SnapshotManifest::fromJson(json, &error);
        if (manifest && manifest->seq == best) {
            auto l = std::make_shared<Loaded>();
            l->manifest = std::move(*manifest);
            l->json = std::move(json);
            l->dir = dir;
            next = l;
        }
        else {
            cswarning() << "SNAPSHOT: " << (dir / "snapshot.json").string() << " is not served: " << error;
        }
    }
    std::lock_guard<std::mutex> lock(impl_->loadedLock);
    impl_->loaded = next;
}

void SnapshotServer::stop() {
    std::lock_guard<std::mutex> lock(impl_->settingsLock);
    if (boundPort_.load() != 0) {
        impl_->stopListening();
        boundPort_ = 0;
        cslog() << "SNAPSHOT: service stopped";
    }
}

}  // namespace cs
