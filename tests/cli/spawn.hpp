#pragma once

// Test-only process helper for the command-line tests. It starts a program with posix_spawn, gives
// it a file descriptor as its standard input, sends it signals, waits for it with a bound, and
// kills and reaps it when its owner goes away. Standard output and standard error go to files in a
// directory the test owns. The environment is the parent's.
//
// Only std and POSIX are used. Spawn failures throw: this is test code, and the public-API rules do
// not apply to it. A descriptor handed over as standard input must be the read end of a pipe made
// with O_CLOEXEC, so no other child inherits it and a closed writer is seen as end of input.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace lws::test {

// Owns one file descriptor.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

// A pipe whose two ends close on exec.
struct Pipe {
    Fd read_end;
    Fd write_end;
};

[[nodiscard]] inline Pipe make_pipe() {
    std::array<int, 2> ends{-1, -1};
    if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::generic_category(), "make_pipe: pipe2");
    }
    return Pipe{.read_end = Fd{ends[0]}, .write_end = Fd{ends[1]}};
}

// A fresh directory under the system temporary directory, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "lws-cli-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "TempDir: mkdtemp");
        }
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline std::string read_text_file(const std::filesystem::path& path) {
    const std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

namespace spawn_detail {

inline constexpr int kCreateFlags = O_WRONLY | O_CREAT | O_TRUNC;
inline constexpr mode_t kCreateMode = 0644;

// The argv shape posix_spawn wants: pointers into `storage`, then a null.
[[nodiscard]] inline std::vector<char*> pointers_of(std::vector<std::string>& storage) {
    std::vector<char*> pointers;
    pointers.reserve(storage.size() + 1);
    std::ranges::transform(storage, std::back_inserter(pointers),
                           [](std::string& entry) { return entry.data(); });
    pointers.push_back(nullptr);
    return pointers;
}

// The exit status of a waited-for child: its code, or 128 plus the signal number when a signal
// killed it.
[[nodiscard]] inline int status_of(int raw) noexcept {
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    if (WIFSIGNALED(raw)) {
        return 128 + WTERMSIG(raw);
    }
    return -1;
}

} // namespace spawn_detail

// A child process started with posix_spawn. Move-only. The destructor sends SIGKILL to the child
// when it is still running and reaps it, so a failed test leaks no process.
class Spawned {
public:
    // Starts argv[0] (searched on PATH) with the whole argv as its arguments. Standard output and
    // standard error are created or truncated at the given files. `stdin_fd`, when given, becomes
    // descriptor 0 in the child; without it the child reads from /dev/null. Throws
    // std::system_error when the program cannot start.
    Spawned(const std::vector<std::string>& argv, const std::filesystem::path& stdout_file,
            const std::filesystem::path& stderr_file, std::optional<int> stdin_fd = std::nullopt) {
        if (argv.empty()) {
            throw std::invalid_argument("Spawned needs a program name");
        }
        std::vector<std::string> arguments_storage = argv;
        std::vector<char*> arguments = spawn_detail::pointers_of(arguments_storage);
        const std::string stdout_path = stdout_file.string();
        const std::string stderr_path = stderr_file.string();

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (stdin_fd) {
            posix_spawn_file_actions_adddup2(&actions, *stdin_fd, STDIN_FILENO);
        } else {
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        }
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                         spawn_detail::kCreateFlags, spawn_detail::kCreateMode);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(),
                                         spawn_detail::kCreateFlags, spawn_detail::kCreateMode);

        // A test may ignore SIGPIPE so a write to a dead child fails instead of killing the test.
        // Ignored signals survive exec, so the child gets the default disposition back.
        sigset_t defaults;
        sigemptyset(&defaults);
        sigaddset(&defaults, SIGPIPE);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setsigdefault(&attributes, &defaults);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF);

        pid_t pid = 0;
        const int result = ::posix_spawnp(&pid, arguments.front(), &actions, &attributes,
                                          arguments.data(), environ);
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if (result != 0) {
            throw std::system_error(result, std::generic_category(),
                                    "Spawned: cannot start " + argv.front());
        }
        pid_ = pid;
    }

    Spawned(const Spawned&) = delete;
    Spawned& operator=(const Spawned&) = delete;
    Spawned(Spawned&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)), reaped_(std::exchange(other.reaped_, true)),
          status_(std::exchange(other.status_, std::nullopt)) {}
    Spawned& operator=(Spawned&& other) noexcept {
        if (this != &other) {
            kill_and_reap();
            pid_ = std::exchange(other.pid_, -1);
            reaped_ = std::exchange(other.reaped_, true);
            status_ = std::exchange(other.status_, std::nullopt);
        }
        return *this;
    }
    ~Spawned() { kill_and_reap(); }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    // Sends `signal` unless the child has been reaped. Returns false when it cannot be delivered.
    bool send(int signal) noexcept { return !reaped_ && pid_ > 0 && ::kill(pid_, signal) == 0; }

    // Waits up to `timeout` for the child to exit. Returns its exit status (see
    // spawn_detail::status_of), or nullopt when it is still running at the deadline.
    [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout) {
        if (reaped_) {
            return status_;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // waitpid(WNOHANG) has no timeout, so the wait polls against a deadline every 10 ms.
        for (;;) {
            int raw = 0;
            const pid_t done = ::waitpid(pid_, &raw, WNOHANG);
            if (done == pid_) {
                reaped_ = true;
                status_ = spawn_detail::status_of(raw);
                return status_;
            }
            if (done < 0 && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "Spawned: waitpid");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

private:
    // SIGKILL and reap, but only a child this object started and has not yet reaped. A reaped pid
    // may already belong to another process, so it is never signalled again.
    void kill_and_reap() noexcept {
        if (pid_ <= 0 || reaped_) {
            return;
        }
        ::kill(pid_, SIGKILL);
        int raw = 0;
        pid_t done = 0;
        do {
            done = ::waitpid(pid_, &raw, 0);
        } while (done < 0 && errno == EINTR);
        reaped_ = true;
        status_ = spawn_detail::status_of(raw);
    }

    pid_t pid_{-1};
    bool reaped_{false};
    std::optional<int> status_;
};

} // namespace lws::test
