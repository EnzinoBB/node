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
            f.found = csdb::backend::detect(dbPath);
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
            return convertNow(dbPath, from, configured, convertOnly, batch);

        case Action::Resume:
            if (state.to != configured) {
                say() << "db/.convert converts to " << state.to << ", config.ini asks for " << configured << std::endl;
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
    try {
        return checkImpl(dbPath, configured, convertFlag, convertOnly, batch);
    }
    catch (const std::exception& e) {
        say() << "failed: " << e.what() << std::endl;
        return 3;
    }
}

}  // namespace dbcheck
