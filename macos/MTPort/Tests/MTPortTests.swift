import Foundation
import Testing
@testable import MTPort

private func device(_ state: String, id: String = "dev", mount: String = "") -> DeviceInfo {
    DeviceInfo(key: id, deviceId: id, name: "Pixel", usbId: "18d1:4ee1", state: state, detail: "", mountPoint: mount,
               readOnly: true, capacityBytes: 0, freeBytes: 0)
}
private func transfer(active: UInt64 = 0) -> TransferInfo {
    TransferInfo(deviceId: "dev", phase: "mounted", bytesRead: 0, bytesStaged: 0, bytesUploaded: 0, uploadsOk: 0, uploadsFailed: 0,
                 uploadsActive: active, uploadBytesDone: 0, uploadBytesTotal: 0, pendingUploads: 0, recoverableUploads: 0)
}

@Suite("State derivation")
struct StateDerivationTests {
    @Test func noAgentMeansStarting() {
        #expect(StateDerivation.overall(devices: [], transfers: [], agentReachable: false, justEjected: false, throughput: 0) == .agentStarting)
    }
    @Test func emptyIsNoPhoneOrSafeToDisconnect() {
        #expect(StateDerivation.overall(devices: [], transfers: [], agentReachable: true, justEjected: false, throughput: 0) == .noPhone)
        #expect(StateDerivation.overall(devices: [], transfers: [], agentReachable: true, justEjected: true, throughput: 0) == .safeToDisconnect)
    }
    @Test func awaitingMtpAndReady() {
        #expect(StateDerivation.overall(devices: [device("awaitingMtp")], transfers: [], agentReachable: true, justEjected: false, throughput: 0) == .awaitingFileTransfer)
        #expect(StateDerivation.overall(devices: [device("detected")], transfers: [], agentReachable: true, justEjected: false, throughput: 0) == .readyToMount)
        #expect(StateDerivation.overall(devices: [device("connecting")], transfers: [], agentReachable: true, justEjected: false, throughput: 0) == .connecting)
    }
    @Test func mountedVsTransferring() {
        let d = [device("mounted")]
        #expect(StateDerivation.overall(devices: d, transfers: [transfer()], agentReachable: true, justEjected: false, throughput: 0) == .mounted)
        #expect(StateDerivation.overall(devices: d, transfers: [transfer(active: 1)], agentReachable: true, justEjected: false, throughput: 0) == .transferring)
        #expect(StateDerivation.overall(devices: d, transfers: [transfer()], agentReachable: true, justEjected: false, throughput: 5_000_000) == .transferring)
    }
    @Test func errorWinsAndCarriesTheMessage() {
        var e = device("error")
        e = DeviceInfo(key: e.key, deviceId: e.deviceId, name: e.name, usbId: e.usbId, state: "error", detail: "DeviceBusy: close Image Capture",
                       mountPoint: "", readOnly: true, capacityBytes: 0, freeBytes: 0)
        #expect(StateDerivation.overall(devices: [device("mounted"), e], transfers: [], agentReachable: true, justEjected: false, throughput: 0)
                == .error("DeviceBusy: close Image Capture"))
    }
    @Test func throughputIsMeasuredNotAssumed() {
        #expect(StateDerivation.throughput(previous: nil, current: 100, seconds: 2) == 0)
        #expect(StateDerivation.throughput(previous: 0, current: 4_000_000, seconds: 2) == 2_000_000)
        #expect(StateDerivation.throughput(previous: 100, current: 50, seconds: 2) == 0)   // counter reset
        #expect(StateDerivation.throughput(previous: 0, current: 100, seconds: 0) == 0)
    }
    @Test func mountPointsAreOnlyDirectlyUnderVolumes() {
        #expect(StateDerivation.isSafeMountPoint("/Volumes/MTPort-Pixel-8"))
        #expect(!StateDerivation.isSafeMountPoint("/Volumes/"))
        #expect(!StateDerivation.isSafeMountPoint("/Volumes/a/b"))
        #expect(!StateDerivation.isSafeMountPoint("/Volumes/.."))
        #expect(!StateDerivation.isSafeMountPoint("/etc"))
        #expect(!StateDerivation.isSafeMountPoint(""))
    }
}

@Suite("Wire format")
struct WireTests {
    @Test func requestShape() throws {
        let data = try AgentClient.encode(method: "mount.start", parameters: MountParams(deviceId: "18d1:4ee1@20.3", readOnly: true), requestId: "r1")
        let obj = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        #expect(obj["protocolVersion"] as? Int == 1)
        #expect(obj["requestId"] as? String == "r1")
        #expect(obj["method"] as? String == "mount.start")
        let params = try #require(obj["parameters"] as? [String: Any])
        #expect(params["deviceId"] as? String == "18d1:4ee1@20.3")
        #expect(params["readOnly"] as? Bool == true)
        #expect(params["force"] == nil)       // optional fields are omitted, not sent as null
    }
    @Test func decodesDevicesAndErrors() throws {
        let ok = #"{"protocolVersion":1,"requestId":"1","result":{"devices":[{"key":"k","id":"18d1:4ee1@1.2","name":"Pixel","usbId":"18d1:4ee1","state":"mounted","detail":"","mountPoint":"/Volumes/X","readOnly":true,"capacityBytes":100,"freeBytes":40}]}}"#
        let r = try JSONDecoder().decode(Response<DevicesResult>.self, from: Data(ok.utf8))
        #expect(r.result?.devices.first?.deviceId == "18d1:4ee1@1.2")
        #expect(r.result?.devices.first?.id == "k")
        let bad = #"{"protocolVersion":1,"requestId":"1","error":{"code":"UnknownDevice","message":"no such device is attached"}}"#
        let e = try JSONDecoder().decode(Response<DevicesResult>.self, from: Data(bad.utf8))
        #expect(e.error == AgentError(code: "UnknownDevice", message: "no such device is attached"))
    }
}

/// Optional contract test against the real C++ agent (fake backend). Set MTPORT_TEST_AGENT to the
/// mtport-agent binary and MTPORT_TEST_FAKE_ROOT to a folder to enable it.
@Suite("Live agent contract")
struct LiveAgentTests {
    @Test func swiftClientTalksToCppAgent() async throws {
        let env = ProcessInfo.processInfo.environment
        guard let agent = env["MTPORT_TEST_AGENT"], let root = env["MTPORT_TEST_FAKE_ROOT"] else { return }
        let home = NSTemporaryDirectory() + "af-swift-\(UUID().uuidString.prefix(6))"
        // The agent mounts newly attached phones by default; this test must never mount anything on the host.
        try FileManager.default.createDirectory(atPath: home + "/Support", withIntermediateDirectories: true)
        try Data(#"{"autoMount":false}"#.utf8).write(to: URL(fileURLWithPath: home + "/Support/settings.json"))
        let p = Process()
        p.executableURL = URL(fileURLWithPath: agent)
        p.arguments = ["--backend", "fake", "--fake-root", root, "--poll-ms", "200", "--socket", home + "/run/agent.sock"]
        p.environment = env.merging(["MTPORT_HOME": home]) { $1 }
        try p.run()
        defer { p.terminate() }
        let client = AgentClient(socketURL: URL(fileURLWithPath: home + "/run/agent.sock"))
        var devices: [DeviceInfo] = []
        for _ in 0..<40 {
            // Real USB phones on this Mac may also be listed (awaitingMtp, no id); only the fake MTP device counts here.
            if let r: DevicesResult = try? await client.call("devices.list"), r.devices.contains(where: { !$0.deviceId.isEmpty }) {
                devices = r.devices.filter { !$0.deviceId.isEmpty }
                break
            }
            try await Task.sleep(for: .milliseconds(150))
        }
        #expect(devices.count == 1)
        #expect(devices.first?.state == "detected")
        do {
            let _: EmptyResult = try await client.call("mount.start", MountParams(deviceId: "0000:0000@9.9"))
            Issue.record("unknown device must be rejected")
        } catch let e as AgentError { #expect(e.code == "UnknownDevice") }
        let s: SettingsResult = try await client.call("settings.get")
        #expect(s.autoMount == false)  // the test's own settings file switched it off
    }
}
