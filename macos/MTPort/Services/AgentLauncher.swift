import Foundation

/// Finds and starts the local agent. Arguments are passed as an array: the app never builds shell strings.
struct AgentLauncher: Sendable {
    static func agentExecutable() -> URL? {
        let fm = FileManager.default
        var candidates: [URL] = []
        if let env = ProcessInfo.processInfo.environment["MTPORT_AGENT_PATH"] { candidates.append(URL(fileURLWithPath: env)) }
        if let res = Bundle.main.resourceURL { candidates.append(res.appendingPathComponent("bin/mtport-agent")) }
        candidates.append(Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("mtport-agent"))
        return candidates.first { fm.isExecutableFile(atPath: $0.path) }
    }

    /// Launches the agent detached from the app. Returns false if no agent binary is available.
    @discardableResult
    static func launch() -> Bool {
        guard let exe = agentExecutable() else { return false }
        let p = Process()
        p.executableURL = exe
        p.arguments = []
        p.standardInput = FileHandle.nullDevice
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run(); return true } catch { return false }
    }
}
