#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Converts the block database in db/ from one backend to another in place, one batch at a time:
// newest blocks first from berkeleydb, which shrinks from the end, oldest first from the others.
// Each batch is removed from the old database only after it is written, synced, read back and
// recorded in db/.convert. A crash or a stop resumes from db/.convert, in the recorded order.
namespace csdb::convert {

enum class Step { Read, Written, Verified, Recorded, Removed };

struct Options {
    std::string dbPath;                 // the node's db/ folder
    std::string from;                   // backend now on disk
    std::string to;                     // configured backend
    uint32_t batch = 100000;
    std::function<void(Step, uint32_t lo, uint32_t hi)> hook;   // tests: throw to simulate a crash
    std::function<bool()> stopRequested;                        // checked between batches
    std::function<void(const std::string&)> log;
};

struct Result {
    bool ok = false;
    bool stopped = false;               // stopped between batches on request, resumable
    std::string error;
    uint64_t blocks = 0;
    uint64_t contracts = 0;
    double seconds = 0;
    uint64_t bytesBefore = 0, bytesAfter = 0;
};

struct State {                          // db/.convert
    std::string from, to;
    uint32_t first = 0, last = 0;
    bool ascending = false;             // order=asc: oldest block first; order=desc or no key: newest first
    // newest first: the lowest sequence already converted and removed (-1 = none yet), and the
    // previous_hash of that block (hex), which the next batch's top block must have as its hash
    int64_t doneDownTo = -1;
    std::string nextPrevHash;
    // oldest first: the highest sequence already converted and removed (-1 = none yet), and the hash
    // of that block (hex), which the next batch's lowest block must have as its previous_hash
    int64_t doneUpTo = -1;
    std::string doneUpToHash;
    bool contractsCopied = false;
    bool moving = false;                // every block checked, the old files go and the new ones move into db/
};

bool readState(const std::string& dbPath, State& out);    // false if absent or malformed
bool writeState(const std::string& dbPath, const State& s);
// free space the conversion needs; a resumed conversion from lmdb or rocksdb counts what .convert-new holds
uint64_t estimatePeakBytes(const std::string& dbPath, const std::string& from, uint32_t batch);
Result run(const Options& opt);         // starts a new conversion or resumes db/.convert

}  // namespace csdb::convert
