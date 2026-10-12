// mtport-agent: per-user coordinator. Watches for phones, owns the control socket and
// launches/stops mount workers. It never opens MTP sessions itself (workers are the single owners).
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>

#include "AgentCore.hpp"
#include "MountState.hpp"
#include "IpcServer.hpp"
#include "mtport/RuntimePaths.hpp"

using namespace mtport;

namespace {
std::atomic<bool> g_quit{false};
void onSignal(int) {
    g_quit = true;
}
}  // namespace

int main(int argc, char** argv) {
    BackendSpec backend;
    std::filesystem::path socketPath = runtimeDir() / "agent.sock";
    std::chrono::milliseconds poll{2000};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };
        if (a == "--backend")
            backend.kind = next();
        else if (a == "--fake-root")
            backend.fakeRoot = next();
        else if (a == "--fake-no-partial-read")
            backend.fakePartialRead = false;
        else if (a == "--socket")
            socketPath = next();
        else if (a == "--poll-ms")
            poll = std::chrono::milliseconds(std::max(200, std::atoi(next().c_str())));
        else if (a == "--version") {
            std::cout << "mtport-agent 0.1.0\n";
            return 0;
        } else {
            std::cerr
                << "usage: mtport-agent [--backend real|fake --fake-root DIR] [--socket PATH] [--poll-ms N]\n";
            return 2;
        }
    }
    if (::geteuid() == 0) {
        std::cerr << "mtport-agent refuses to run as root\n";
        return 78;
    }

    try {
        std::shared_ptr<IMtpDeviceFactory> factory(makeFactory(backend));
        auto coordinator = std::make_shared<DeviceCoordinator>(factory);
        auto controller = std::make_shared<MountSupervisor>(backend);
        AgentCore core(coordinator, controller, backend, appSupportDir() / "settings.json");
        IpcServer server(socketPath, [&](const std::string& line) { return core.handleLine(line); });

        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        for (const auto& e : ejectionMarkers()) removeMountState(e.deviceId);  // a fresh agent treats every phone as newly attached
        coordinator->refresh();
        coordinator->startPolling(poll);
        server.start();
        std::cout << "mtport-agent: listening on " << socketPath << std::endl;
        while (!g_quit) {
            controller->reap();
            core.autoMountTick();
            ::usleep(500 * 1000);
        }
        server.stop();
        coordinator->stopPolling();
        return 0;
    } catch (const MtpError& e) {
        std::cerr << "mtport-agent: " << e.what() << "\n";
        return e.code() == MtpErrc::DeviceBusy ? 75 : 1;
    }
}
