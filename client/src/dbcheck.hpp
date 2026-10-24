#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Start-up check of the block database: recognises a database of another backend than the
// configured one and converts it (csdb::convert) after asking, or when told to by a flag.
namespace dbcheck {

enum class Action { Open, Convert, Ask, StopMismatch, StopBoth, Resume };

struct Facts {
    std::vector<std::string> found;    // csdb::backend::detect
    std::string configured;            // Config storage settings dbBackend
    bool convertStatePresent = false;  // db/.convert exists
    bool convertFlag = false;          // --convert-db or --convert-only
    bool terminal = false;             // isatty(STDIN_FILENO)
};

Action decide(const Facts& f);

// --convert-batch: blocks read into memory at once
constexpr uint32_t kMaxBatch = 1000000;

// Runs the decision. Returns -1 to continue the normal start, otherwise the process exit code
// (0: done or paused, 3: stopped, nothing changed or conversion failed/refused, or a batch
// outside 1..kMaxBatch).
int check(const std::string& dbPath, const std::string& configured, bool convertFlag, bool convertOnly, uint32_t batch);

// true when another process (a running node) holds <db>/.lock; this process takes the lock otherwise
bool lockedByAnotherProcess(const std::string& dbPath);

}  // namespace dbcheck
