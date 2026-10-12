import AppKit
import Foundation
import Observation
import ServiceManagement

@MainActor
@Observable
final class DeviceViewModel {
    // Published state
    private(set) var devices: [DeviceInfo] = []
    private(set) var transfers: [TransferInfo] = []
    private(set) var overall: OverallState = .agentStarting
    private(set) var throughput: Double = 0            // bytes/s, measured between polls
    private(set) var lastError: String?
    private(set) var doctor: DoctorResult?
    private(set) var busyDevices: Set<String> = []     // devices with a mount/eject request in flight
    var autoMount = false
    var launchAtLogin = false

    private let client: AgentClient
    private var pollTask: Task<Void, Never>?
    private var previousBytes: UInt64?
    private var previousSample: Date?
    private var agentLaunchAttempted = false
    private var justEjected = false
    private var knownMounted: Set<String> = []     // devices already seen as mounted (to open Finder once per mount)

    init(client: AgentClient = AgentClient()) {
        self.client = client
        launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    func start() {
        guard pollTask == nil else { return }
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refresh()
                try? await Task.sleep(for: .seconds(2))     // modest cadence; file contents are never polled
            }
        }
    }

    func stop() { pollTask?.cancel(); pollTask = nil }

    func refresh() async {
        do {
            let d: DevicesResult = try await client.call("devices.list")
            let t: TransfersResult = try await client.call("transfer.list")
            devices = d.devices
            transfers = t.transfers
            openFinderForNewMounts()
            sampleThroughput()
            if devices.contains(where: { $0.state == "mounted" }) { justEjected = false }
            overall = StateDerivation.overall(devices: devices, transfers: transfers, agentReachable: true, justEjected: justEjected, throughput: throughput)
            if let s: SettingsResult = try? await client.call("settings.get") { autoMount = s.autoMount }
        } catch let e as AgentError where e.code == "AgentNotRunning" {
            overall = .agentStarting
            devices = []
            transfers = []
            if !agentLaunchAttempted {
                agentLaunchAttempted = true
                if !AgentLauncher.launch() {
                    lastError = String(localized: "error.agentMissing")
                }
            }
        } catch {
            lastError = error.localizedDescription
        }
    }

    /// Plug in -> the agent mounts -> Finder shows the phone, without any click.
    private func openFinderForNewMounts() {
        let nowMounted = Set(devices.filter { $0.state == "mounted" }.map(\.key))
        for device in devices where device.state == "mounted" && !knownMounted.contains(device.key) {
            openInFinder(device)
        }
        knownMounted = nowMounted
    }

    private func sampleThroughput() {
        let total = transfers.reduce(UInt64(0)) { $0 &+ $1.bytesRead &+ $1.bytesUploaded }
        let now = Date()
        if let prevT = previousSample {
            throughput = StateDerivation.throughput(previous: previousBytes, current: total, seconds: now.timeIntervalSince(prevT))
        }
        previousBytes = total
        previousSample = now
    }

    // MARK: Actions

    func mount(_ device: DeviceInfo, readOnly: Bool = true) {
        guard !device.deviceId.isEmpty else { return }
        run(device.deviceId) { [client] in
            let _: EmptyResult = try await client.call("mount.start", MountParams(deviceId: device.deviceId, readOnly: readOnly))
        }
    }

    func eject(_ device: DeviceInfo) {
        run(device.deviceId) { [client] in
            let _: EmptyResult = try await client.call("mount.stop", MountParams(deviceId: device.deviceId, force: false))
        }
        justEjected = true
    }

    func openInFinder(_ device: DeviceInfo) {
        guard StateDerivation.isSafeMountPoint(device.mountPoint) else { return }
        NSWorkspace.shared.open(URL(fileURLWithPath: device.mountPoint, isDirectory: true))
    }

    func setAutoMount(_ on: Bool) {
        autoMount = on
        Task { [client] in
            let _: SettingsResult? = try? await client.call("settings.set", SettingsParams(autoMount: on))
        }
    }

    func setLaunchAtLogin(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            launchAtLogin = on
        } catch {
            lastError = error.localizedDescription
            launchAtLogin = SMAppService.mainApp.status == .enabled
        }
    }

    func runDoctor() {
        Task { [client] in
            do { doctor = try await client.call("app.doctor") } catch { lastError = error.localizedDescription }
        }
    }

    func dismissError() { lastError = nil }

    func transfer(for device: DeviceInfo) -> TransferInfo? { transfers.first { $0.deviceId == device.deviceId } }

    private func run(_ id: String, _ work: @escaping @Sendable () async throws -> Void) {
        busyDevices.insert(id)
        Task {
            do { try await work() } catch { lastError = error.localizedDescription }
            busyDevices.remove(id)
            await refresh()
        }
    }
}
