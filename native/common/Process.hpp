// posix_spawn wrappers. Arguments are passed as argv vectors; nothing is ever run through a shell.
#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mtport {

struct ProcessResult {
    int exitCode{-1};    // -1: could not run / killed by signal
    std::string output;  // stdout+stderr (bounded)
};

// Runs argv[0] (absolute path or resolved via `searchPath`) and waits. Output capped at 1 MiB.
ProcessResult runProcess(const std::vector<std::string>& argv, int timeoutSeconds = 60);

// Starts a detached child (new session, stdio -> /dev/null or the given log file) and returns its pid.
pid_t spawnDetached(const std::vector<std::string>& argv, const std::filesystem::path& logFile = {});

// Absolute path of the directory containing the running executable.
std::filesystem::path executableDir();
// Looks for a sibling executable (e.g. mtport-mount) next to the running binary, then PATH.
std::filesystem::path findSibling(const std::string& name);

bool processAlive(pid_t pid);

}  // namespace mtport
