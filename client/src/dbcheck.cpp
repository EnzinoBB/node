#include "dbcheck.hpp"

#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>

#include <csdb/storage_backend.hpp>
#include <csdb/storage_convert.hpp>

#ifdef _WIN32
#include <io.h>
#define CS_ISATTY(fd) _isatty(fd)
#define CS_STDIN_FD 0
#else
#include <cerrno>
#include <fcntl.h>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#define CS_ISATTY(fd) isatty(fd)
#define CS_STDIN_FD STDIN_FILENO
#endif

namespace dbcheck {

namespace {

std::atomic<bool> stopFlag{false};

extern "C" void onStopSignal(int) {
    stopFlag = true;
}

std::ostream& say() {
    return std::cout << "block database: ";
}

std::string mb(uint64_t bytes) {
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

enum class Lock { Held, Busy, None };

// An exclusive lock on <db>/.lock, held until the process exits: the descriptor is never closed, so
// a second process sees the database in use for the node's whole run. A missing db/ is left
// untouched (first start), and then nothing is locked.
Lock lockDatabase(const std::string& dbPath) {
#ifdef _WIN32
    (void)dbPath;
    return Lock::None;
#else
    namespace fs = std::filesystem;
    static std::map<std::string, int> held;
    std::error_code ec;
    if (!fs::is_directory(dbPath, ec)) {
        return Lock::None;
    }
    const std::string path = (fs::path(dbPath) / ".lock").string();
    auto it = held.find(path);
    if (it != held.end()) {
        struct stat mine {}, now {};
        if (::fstat(it->second, &mine) == 0 && ::stat(path.c_str(), &now) == 0 && mine.st_dev == now.st_dev && mine.st_ino == now.st_ino) {
            return Lock::Held;   // checked again in this process
        }
        ::close(it->second);     // the folder was replaced (tests reuse folder names)
        held.erase(it);
    }
    // O_CLOEXEC: the contract executor started by the node must not keep the lock after the node exits
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return Lock::None;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        return err == EWOULDBLOCK ? Lock::Busy : Lock::None;
    }
    held[path] = fd;
    return Lock::Held;
#endif
}

bool inUse(Lock lock, const std::string& dbPath) {
    if (lock != Lock::Busy) {
        return false;
    }
    say() << "another node process is using " << dbPath << ": stop it first" << std::endl;
    return true;
}

uint64_t freeSpace(const std::string& dbPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(dbPath);
    if (!fs::exists(p, ec)) {
        p = p.has_parent_path() ? p.parent_path() : fs::path(".");
    }
    const auto info = fs::space(p, ec);
    return ec ? UINT64_MAX : static_cast<uint64_t>(info.available);
}

uint64_t sizeOf(const std::string& name, const std::string& dbPath) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& f : csdb::backend::files(name, dbPath)) {
        const auto s = std::filesystem::file_size(f, ec);
        if (!ec) {
            total += s;
        }
    }
    return total;
}

int convertNow(const std::string& dbPath, const std::string& from, const std::string& to, bool convertOnly, uint32_t batch) {
    const uint64_t need = csdb::convert::estimatePeakBytes(dbPath, from, batch);
    const uint64_t have = freeSpace(dbPath);
    if (have < need) {
        say() << "not enough free space: the conversion needs about " << mb(need) << ", " << mb(have) << " available" << std::endl;
        return 3;
    }

    csdb::convert::Options opt;
    opt.dbPath = dbPath;
    opt.from = from;
    opt.to = to;
    opt.batch = batch;
    opt.log = [](const std::string& line) { say() << line << std::endl; };
    opt.stopRequested = [] { return stopFlag.load(); };

    stopFlag = false;
    auto prevInt = std::signal(SIGINT, onStopSignal);
    auto prevTerm = std::signal(SIGTERM, onStopSignal);
    csdb::convert::Result res;
    std::string failure;
    try {
        res = csdb::convert::run(opt);
    }
    catch (const std::exception& e) {
        failure = e.what();
    }
    std::signal(SIGINT, prevInt);
    std::signal(SIGTERM, prevTerm);

    if (!failure.empty()) {
        say() << "conversion failed: " << failure << std::endl;
        return 3;
    }
    if (res.stopped) {
        say() << "conversion paused, it resumes at the next start" << std::endl;
        return 0;
    }
    if (!res.ok) {
        say() << "conversion failed: " << res.error << std::endl;
        return 3;
    }
    say() << "converted " << res.blocks << " blocks from " << from << " to " << to << std::endl;
    return convertOnly ? 0 : -1;
}

}  // namespace

Action decide(const Facts& f) {
    if (f.convertStatePresent) {
        return Action::Resume;
    }
    if (f.found.size() > 1) {
        return Action::StopBoth;
    }
    if (f.found.empty() || f.found[0] == f.configured) {
        return Action::Open;
    }
    if (f.convertFlag) {
        return Action::Convert;
    }
    if (f.terminal) {
        return Action::Ask;
    }
    return Action::StopMismatch;
}

namespace {

int checkImpl(const std::string& dbPath, const std::string& configured, bool convertFlag, bool convertOnly, uint32_t batch) {
    Facts f;
    f.configured = configured;
    f.convertFlag = convertFlag;
    f.terminal = CS_ISATTY(CS_STDIN_FD) != 0;

    // a busy lock stops a conversion only; a normal start goes on as before
    const Lock lock = lockDatabase(dbPath);

    csdb::convert::State state;
    {
        std::error_code ec;
        f.convertStatePresent = std::filesystem::exists(std::filesystem::path(dbPath) / ".convert", ec);
    }
    if (f.convertStatePresent) {
        if (!csdb::convert::readState(dbPath, state)) {
            say() << "db/.convert exists but cannot be read, fix or remove it" << std::endl;
            return 3;
        }
    }
    else {
        try {
            // with another process on the folder no engine is opened (BerkeleyDB would run recovery
            // on its live environment)
            f.found = csdb::backend::detect(dbPath, lock != Lock::Busy);
        }
        catch (const std::exception& e) {
            say() << "cannot inspect " << dbPath << ": " << e.what() << std::endl;
            return 3;
        }
    }

    const std::string from = f.found.empty() ? std::string() : f.found[0];
    switch (decide(f)) {
        case Action::Open:
            if (convertOnly) {
                say() << "nothing to convert: the block database is already " << configured << std::endl;
                return 0;
            }
            return -1;

        case Action::StopBoth:
            say() << "found both " << f.found[0] << " and " << f.found[1] << " in " << dbPath
                  << ", remove the one you do not want" << std::endl;
            return 3;

        case Action::StopMismatch:
            say() << "the block database is " << from << ", config.ini asks for " << configured
                  << ": run the node with --convert-db to convert it, or set db_backend=" << from
                  << " (in Docker: CREDITS_CONVERT_DB=yes)" << std::endl;
            return 3;

        case Action::Ask: {
            if (inUse(lock, dbPath)) {
                return 3;
            }
            const uint64_t need = csdb::convert::estimatePeakBytes(dbPath, from, batch);
            say() << "found " << from << ", configured " << configured << std::endl;
            say() << "database size " << mb(sizeOf(from, dbPath)) << ", estimated peak disk use " << mb(need)
                  << ", free space " << mb(freeSpace(dbPath)) << std::endl;
            std::cout << "Convert the block database from " << from << " to " << configured << " now? [y/N] " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "y" && answer != "Y" && answer != "yes") {
                say() << "nothing changed" << std::endl;
                return 3;
            }
            return convertNow(dbPath, from, configured, convertOnly, batch);
        }

        case Action::Convert:
            if (inUse(lock, dbPath)) {
                return 3;
            }
            return convertNow(dbPath, from, configured, convertOnly, batch);

        case Action::Resume:
            if (state.to != configured) {
                say() << "db/.convert converts to " << state.to << ", config.ini asks for " << configured
                      << ": set db_backend=" << state.to << " to finish it" << std::endl;
                return 3;
            }
            if (inUse(lock, dbPath)) {
                return 3;
            }
            return convertNow(dbPath, state.from, state.to, convertOnly, batch);
    }
    return -1;
}

}  // namespace

int check(const std::string& dbPath, const std::string& configured, bool convertFlag, bool convertOnly, uint32_t batch) {
    // No engine exception may escape: print it and stop with nothing changed. The signal handlers
    // are installed and restored inside convertNow, around csdb::convert::run only.
    if (batch < 1 || batch > kMaxBatch) {
        say() << "--convert-batch must be between 1 and " << kMaxBatch << ", got " << batch << std::endl;
        return 3;
    }
    try {
        return checkImpl(dbPath, configured, convertFlag, convertOnly, batch);
    }
    catch (const std::exception& e) {
        say() << "failed: " << e.what() << std::endl;
        return 3;
    }
}

}  // namespace dbcheck
