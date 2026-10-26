#ifndef PLATFORM_HPP
#define PLATFORM_HPP

// File, lock, process and thread primitives that differ between Linux, macOS and Windows, for the
// code that needs more than std::fstream: reads and writes at an offset on a file kept open,
// durable writes (fsync), an exclusive lock held for the life of the process, a child process
// without a shell, and a lower priority for one thread.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <crt_externs.h>
#include <pthread.h>
#endif
#endif

namespace cs::platform {

#ifdef _WIN32
using Handle = HANDLE;
inline const Handle kInvalidHandle = INVALID_HANDLE_VALUE;
#else
using Handle = int;
constexpr Handle kInvalidHandle = -1;
#endif

enum class Open {
    Read,          // an existing file, read only
    ReadWrite,     // created when missing, kept otherwise
    Truncate       // created when missing, emptied otherwise, write only
};

// noFollow: a symbolic link at path is refused (POSIX); on Windows a reparse point is not followed
inline Handle openFile(const std::filesystem::path& path, Open mode, bool noFollow = false, unsigned permissions = 0644) {
#ifdef _WIN32
    (void)permissions;
    DWORD access = mode == Open::Read ? GENERIC_READ : (mode == Open::Truncate ? GENERIC_WRITE : GENERIC_READ | GENERIC_WRITE);
    DWORD disposition = mode == Open::Read ? OPEN_EXISTING : (mode == Open::Truncate ? CREATE_ALWAYS : OPEN_ALWAYS);
    DWORD flags = FILE_ATTRIBUTE_NORMAL | (noFollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    return ::CreateFileW(path.wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         disposition, flags, nullptr);
#else
    int flags = O_CLOEXEC | (noFollow ? O_NOFOLLOW : 0);
    flags |= mode == Open::Read ? O_RDONLY : (mode == Open::Truncate ? O_WRONLY | O_CREAT | O_TRUNC : O_RDWR | O_CREAT);
    return ::open(path.c_str(), flags, static_cast<mode_t>(permissions));
#endif
}

// a handle kept as an integer where a header must not include the system headers (-1 = none)
inline Handle fromInt(intptr_t v) {
#ifdef _WIN32
    return reinterpret_cast<Handle>(v);
#else
    return static_cast<Handle>(v);
#endif
}

inline intptr_t toInt(Handle h) {
#ifdef _WIN32
    return reinterpret_cast<intptr_t>(h);
#else
    return static_cast<intptr_t>(h);
#endif
}

inline bool valid(Handle h) {
    return h != kInvalidHandle;
}

inline void closeFile(Handle h) {
    if (!valid(h)) {
        return;
    }
#ifdef _WIN32
    ::CloseHandle(h);
#else
    ::close(h);
#endif
}

// bytes read at offset, 0 at the end, -1 on an error; the file position is not used
inline int64_t readAt(Handle h, void* buffer, size_t size, uint64_t offset) {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    const DWORD want = size > 0x40000000u ? 0x40000000u : static_cast<DWORD>(size);
    if (!::ReadFile(h, buffer, want, &done, &at)) {
        return ::GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return static_cast<int64_t>(done);
#else
    return ::pread(h, buffer, size, static_cast<off_t>(offset));
#endif
}

// bytes written at offset, -1 on an error
inline int64_t writeAt(Handle h, const void* buffer, size_t size, uint64_t offset) {
#ifdef _WIN32
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    const DWORD want = size > 0x40000000u ? 0x40000000u : static_cast<DWORD>(size);
    if (!::WriteFile(h, buffer, want, &done, &at)) {
        return -1;
    }
    return static_cast<int64_t>(done);
#else
    return ::pwrite(h, buffer, size, static_cast<off_t>(offset));
#endif
}

// the whole buffer at offset, retrying short writes
inline bool writeAllAt(Handle h, const void* buffer, size_t size, uint64_t offset) {
    const auto* p = static_cast<const char*>(buffer);
    size_t done = 0;
    while (done < size) {
        const auto n = writeAt(h, p + done, size - done, offset + done);
        if (n <= 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

inline bool resize(Handle h, uint64_t size) {
#ifdef _WIN32
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return ::SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof(info)) != 0;
#else
    return ::ftruncate(h, static_cast<off_t>(size)) == 0;
#endif
}

// -1 on an error
inline int64_t fileSize(Handle h) {
#ifdef _WIN32
    LARGE_INTEGER size{};
    return ::GetFileSizeEx(h, &size) ? static_cast<int64_t>(size.QuadPart) : -1;
#else
    struct stat st {};
    return ::fstat(h, &st) == 0 ? static_cast<int64_t>(st.st_size) : -1;
#endif
}

inline bool sync(Handle h) {
#ifdef _WIN32
    return ::FlushFileBuffers(h) != 0;
#else
    return ::fsync(h) == 0;
#endif
}

// the directory entry of a file renamed or created in dir reaches the disk (POSIX); Windows has no
// such call and keeps the directory with the file
inline void syncDirectory([[maybe_unused]] const std::filesystem::path& dir) {
#ifndef _WIN32
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

// the file is about to be read from start to end, more than once (Linux only)
inline void willRead([[maybe_unused]] Handle h) {
#if defined(__linux__)
    ::posix_fadvise(h, 0, 0, POSIX_FADV_WILLNEED);
#endif
}

// an existing file or directory reaches the disk
inline void syncPath(const std::filesystem::path& path, bool directory) {
    if (directory) {
        syncDirectory(path);
        return;
    }
#ifdef _WIN32
    // FlushFileBuffers needs write access
    HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(h);
        ::CloseHandle(h);
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
#endif
}

// a small file written in full or not at all: a temporary file next to it, synced, then renamed
// over it (and the directory synced when asked); permissions apply on POSIX
inline bool writeFileAtomically(const std::filesystem::path& path, const std::string& content, unsigned permissions = 0644,
                                bool noFollow = false, bool syncDir = false) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    const Handle h = openFile(tmp, Open::Truncate, noFollow, permissions);
    if (!valid(h)) {
        return false;
    }
    bool ok = writeAllAt(h, content.data(), content.size(), 0);
#ifndef _WIN32
    ok = ok && ::fchmod(h, static_cast<mode_t>(permissions)) == 0;
#endif
    ok = ok && sync(h);
    closeFile(h);
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, path, ec);
        ok = !ec;
    }
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (syncDir) {
        syncDirectory(path.has_parent_path() ? path.parent_path() : std::filesystem::path("."));
    }
    return true;
}

enum class Lock { Held, Busy, Failed };

// an exclusive lock on an open file, kept until the handle is closed or the process exits
inline Lock lockExclusive(Handle h) {
#ifdef _WIN32
    OVERLAPPED at{};
    if (::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &at)) {
        return Lock::Held;
    }
    return ::GetLastError() == ERROR_LOCK_VIOLATION ? Lock::Busy : Lock::Failed;
#else
    if (::flock(h, LOCK_EX | LOCK_NB) == 0) {
        return Lock::Held;
    }
    return errno == EWOULDBLOCK ? Lock::Busy : Lock::Failed;
#endif
}

// name found on the PATH (on Windows also with .exe), empty when there is none
inline std::string findExecutable(const std::string& name) {
    const char* path = std::getenv("PATH");
#ifdef _WIN32
    const char separator = ';';
    const std::vector<std::string> names = {name + ".exe", name};
#else
    const char separator = ':';
    const std::vector<std::string> names = {name};
#endif
    std::stringstream dirs(path ? path : "");
    std::string dir;
    while (std::getline(dirs, dir, separator)) {
        for (const auto& n : names) {
            const auto candidate = (std::filesystem::path(dir.empty() ? "." : dir) / n);
            std::error_code ec;
#ifdef _WIN32
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate.string();
            }
#else
            if (::access(candidate.c_str(), X_OK) == 0) {
                return candidate.string();
            }
#endif
        }
    }
    return {};
}

#ifdef _WIN32
namespace detail {
// one argument quoted for CreateProcess, as CommandLineToArgvW reads it back
inline std::wstring quoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        }
        else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
}  // namespace detail
#endif

// runs argv[0] with these arguments, no shell in between, and waits; its exit code, -1 when it did
// not run
inline int runProcess(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return -1;
    }
#ifdef _WIN32
    std::wstring line;
    for (const auto& a : argv) {
        line += (line.empty() ? L"" : L" ") + detail::quoteArgument(std::filesystem::path(a).wstring());
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring program = std::filesystem::path(argv[0]).wstring();
    if (!::CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    const bool got = ::GetExitCodeProcess(pi.hProcess, &code) != 0;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return got ? static_cast<int>(code) : -1;
#else
    std::vector<std::string> copy(argv);
    std::vector<char*> args;
    for (auto& a : copy) {
        args.push_back(a.data());
    }
    args.push_back(nullptr);
#if defined(__APPLE__)
    char** env = *::_NSGetEnviron();
#else
    char** env = ::environ;
#endif
    pid_t pid = 0;
    if (::posix_spawn(&pid, copy[0].c_str(), nullptr, nullptr, args.data(), env) != 0) {
        return -1;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// the calling thread runs below the process's other threads
inline void lowerThreadPriority() {
#ifdef _WIN32
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
#elif defined(__APPLE__)
    // the whole process on macOS has no per-thread nice value: a background QoS class instead
    ::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
}

}  // namespace cs::platform

#endif  // PLATFORM_HPP
