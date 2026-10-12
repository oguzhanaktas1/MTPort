import Foundation

/// Pure logic (no UI, no I/O) so it can be unit-tested: how raw agent data becomes what the menu shows.
/// Device states shown in the UI (+ `readyToMount`: MTP is up, waiting for the user to press Mount).
enum OverallState: Equatable, Sendable {
    case agentStarting
    case noPhone
    case awaitingFileTransfer
    case readyToMount
    case connecting
    case mounted
    case transferring
    case safeToDisconnect
    case error(String)

    /// Localisation key (see Localizable.xcstrings).
    var titleKey: String {
        switch self {
        case .agentStarting: "state.agentStarting"
        case .noPhone: "state.noPhone"
        case .awaitingFileTransfer: "state.awaitingFileTransfer"
        case .readyToMount: "state.readyToMount"
        case .connecting: "state.connecting"
        case .mounted: "state.mounted"
        case .transferring: "state.transferring"
        case .safeToDisconnect: "state.safeToDisconnect"
        case .error: "state.error"
        }
    }

    var symbol: String {
        switch self {
        case .agentStarting, .connecting: "arrow.triangle.2.circlepath"
        case .noPhone: "iphone.slash"
        case .awaitingFileTransfer: "cable.connector"
        case .readyToMount: "iphone.gen3"
        case .mounted: "iphone.gen3.badge.checkmark"
        case .transferring: "arrow.up.arrow.down.circle"
        case .safeToDisconnect: "checkmark.circle"
        case .error: "exclamationmark.triangle"
        }
    }
}

enum StateDerivation {
    /// Throughput threshold (bytes/s) above which a mounted volume counts as "transferring".
    static let transferThreshold: Double = 1024

    static func overall(devices: [DeviceInfo], transfers: [TransferInfo], agentReachable: Bool, justEjected: Bool, throughput: Double) -> OverallState {
        guard agentReachable else { return .agentStarting }
        if let bad = devices.first(where: { $0.state == "error" }) { return .error(bad.detail) }
        if devices.contains(where: { $0.state == "mounted" || $0.state == "draining" }) {
            let active = transfers.contains { $0.uploadsActive > 0 } || throughput > transferThreshold
            return active ? .transferring : .mounted
        }
        if devices.contains(where: { $0.state == "connecting" }) { return .connecting }
        if devices.contains(where: { $0.state == "detected" }) { return .readyToMount }
        if devices.contains(where: { $0.state == "awaitingMtp" }) { return .awaitingFileTransfer }
        return justEjected ? .safeToDisconnect : .noPhone
    }

    /// Measured (never assumed) throughput between two samples of the cumulative byte counters.
    static func throughput(previous: UInt64?, current: UInt64, seconds: TimeInterval) -> Double {
        guard let previous, seconds > 0, current >= previous else { return 0 }
        return Double(current - previous) / seconds
    }

    /// Only ever open volumes that MTPort mounted directly below /Volumes.
    static func isSafeMountPoint(_ path: String) -> Bool {
        let prefix = "/Volumes/"
        guard path.hasPrefix(prefix) else { return false }
        let rest = path.dropFirst(prefix.count)
        return !rest.isEmpty && !rest.contains("/") && rest != "." && rest != ".."
    }
}

enum ByteFormat {
    static func string(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: bytes), countStyle: .file)
    }
    static func rate(_ bytesPerSecond: Double) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytesPerSecond), countStyle: .file) + "/s"
    }
}
