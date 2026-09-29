#pragma once
#include <cstddef>
#include <string>

namespace ksh {

std::string quote(const std::string& s);

struct Result {
    std::string out;
    int rc{0};
    bool timed_out{false};
    bool size_capped{false};
};

// Runs `cmd` through /bin/sh in its own process group, with a wall-clock
// timeout and an output size cap. On timeout/size-cap the whole process group
// is killed, so a hung child can never wedge the caller (CLI or HTTP pool).
// timeout_ms <= 0 means no timeout.
Result run(const std::string& cmd, int timeout_ms = 0, size_t max_bytes = 0);

// Convenience: throws on timeout / size-cap / nonzero exit.
std::string run_checked(const std::string& cmd, int timeout_ms = 0, size_t max_bytes = 0);

// Default limits, overridable via KNOWLEDGE_CMD_TIMEOUT_S / KNOWLEDGE_CMD_MAX_BYTES.
int default_timeout_ms();
size_t default_max_bytes();

}  // namespace ksh
