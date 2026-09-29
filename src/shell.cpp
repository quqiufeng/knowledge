#include "shell.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ksh {

namespace {

constexpr size_t kDefaultMaxBytes = 16u << 20;

int env_int(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long n = std::strtol(v, &end, 10);
    if (end == v || n <= 0) return fallback;
    return static_cast<int>(n);
}

size_t env_size(const char* name, size_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long long n = std::strtoll(v, &end, 10);
    if (end == v || n <= 0) return fallback;
    return static_cast<size_t>(n);
}

}  // namespace

int default_timeout_ms() { return env_int("KNOWLEDGE_CMD_TIMEOUT_S", 60) * 1000; }
size_t default_max_bytes() { return env_size("KNOWLEDGE_CMD_MAX_BYTES", kDefaultMaxBytes); }

std::string quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

Result run(const std::string& cmd, int timeout_ms, size_t max_bytes) {
    if (timeout_ms <= 0) timeout_ms = default_timeout_ms();
    if (max_bytes == 0) max_bytes = default_max_bytes();

    int fds[2];
    if (pipe(fds) != 0) throw std::runtime_error("[CMD] pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        throw std::runtime_error("[CMD] fork failed");
    }
    if (pid == 0) {
        // Child: own process group (so the parent can kill the whole tree),
        // stdout -> pipe, stderr stays inherited (providers log to stderr).
        close(fds[0]);
        if (setpgid(0, 0) != 0) _exit(127);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(pid, pid);  // close the race with the child's own setpgid
    close(fds[1]);

    Result r;
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    auto start = std::chrono::steady_clock::now();
    char buf[8192];
    bool killed = false;
    while (true) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        int wait_ms = -1;
        if (timeout_ms > 0) {
            if (elapsed_ms >= timeout_ms) {
                r.timed_out = true;
                killed = true;
                break;
            }
            wait_ms = static_cast<int>(timeout_ms - elapsed_ms);
        }
        struct pollfd pfd { fds[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {  // timeout
            r.timed_out = true;
            killed = true;
            break;
        }
        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break;  // EOF: child closed stdout
        if (r.out.size() + static_cast<size_t>(n) > max_bytes) {
            size_t room = max_bytes - r.out.size();
            r.out.append(buf, room);
            r.size_capped = true;
            killed = true;
            break;
        }
        r.out.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    if (killed) kill(-pid, SIGKILL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.rc = 128 + WTERMSIG(status);
    return r;
}

std::string run_checked(const std::string& cmd, int timeout_ms, size_t max_bytes) {
    Result r = run(cmd, timeout_ms, max_bytes);
    if (r.timed_out) throw std::runtime_error("[CMD] timeout: " + cmd);
    if (r.size_capped) throw std::runtime_error("[CMD] output limit exceeded: " + cmd);
    if (r.rc != 0) throw std::runtime_error("[CMD] command failed (rc=" + std::to_string(r.rc) + "): " + cmd);
    return r.out;
}

}  // namespace ksh
