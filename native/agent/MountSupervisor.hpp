// Starts/stops mtport-mount workers. Never builds a shell command: argv vectors only.
#pragma once

#include <sys/types.h>

#include <vector>

#include "Backend.hpp"
#include "mtport/DeviceTypes.hpp"

namespace mtport {

class IMountController {
public:
    virtual ~IMountController() = default;
    virtual void start(const DeviceId&, bool readOnly) = 0;  // returns once the worker was launched
    virtual void stop(const DeviceId&, bool force) = 0;      // returns once the volume is unmounted
    virtual void reap() {}                                   // collect exited workers (no zombies)
};

class MountSupervisor final : public IMountController {
public:
    explicit MountSupervisor(BackendSpec spec) : spec_(std::move(spec)) {}
    void start(const DeviceId&, bool readOnly) override;
    void stop(const DeviceId&, bool force) override;
    void reap() override;

private:
    BackendSpec spec_;
    std::vector<pid_t> children_;  // workers spawned by this agent; reaped so they never linger as zombies
};

}  // namespace mtport
