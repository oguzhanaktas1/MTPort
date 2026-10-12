#pragma once

#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Backend.hpp"
#include "Json.hpp"

namespace mtport::cli {

struct Args {
    std::string command;
    std::map<std::string, std::string> options;  // --name value
    std::set<std::string> flags;                 // --name
    std::vector<std::string> positional;

    bool has(const std::string& f) const { return flags.count(f) != 0; }
    std::string opt(const std::string& name, const std::string& fallback = {}) const {
        auto it = options.find(name);
        return it == options.end() ? fallback : it->second;
    }
};

// Parses argv. Throws std::invalid_argument (usage error) on unknown options / missing values.
Args parseArgs(int argc, const char* const* argv);

enum ExitCode : int {
    kOk = 0,
    kGeneric = 1,
    kUsage = 2,
    kBusy = 3,
    kNoDevice = 4,
    kNotReady = 5,
    kNotFound = 6,
    kExists = 7,
    kBackendUnavailable = 8,
};

// Runs one CLI invocation. Output goes to `out`, diagnostics to `err`; returns the exit code.
int run(const Args& args, std::ostream& out, std::ostream& err);

BackendSpec backendFromArgs(const Args& args);
int exitCodeFor(MtpErrc);

}  // namespace mtport::cli
