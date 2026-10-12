import Darwin
import Foundation

// MARK: - Wire models (protocolVersion 1, see native/agent/AgentCore.hpp)

struct AgentError: Error, Codable, Sendable, Equatable, LocalizedError {
    let code: String
    let message: String
    var errorDescription: String? { "\(code): \(message)" }
}

struct DeviceInfo: Codable, Sendable, Equatable, Identifiable {
    let key: String
    let deviceId: String
    let name: String
    let usbId: String
    let state: String          // detected | awaitingMtp | connecting | mounted | draining | error
    let detail: String
    let mountPoint: String
    let readOnly: Bool
    let capacityBytes: UInt64
    let freeBytes: UInt64

    var id: String { key }

    enum CodingKeys: String, CodingKey {
        case key, name, usbId, state, detail, mountPoint, readOnly, capacityBytes, freeBytes
        case deviceId = "id"
    }
}

struct TransferInfo: Codable, Sendable, Equatable {
    let deviceId: String
    let phase: String
    let bytesRead: UInt64
    let bytesStaged: UInt64
    let bytesUploaded: UInt64
    let uploadsOk: UInt64
    let uploadsFailed: UInt64
    let uploadsActive: UInt64
    let uploadBytesDone: UInt64
    let uploadBytesTotal: UInt64
    let pendingUploads: UInt64
    let recoverableUploads: UInt64
}

struct DevicesResult: Codable, Sendable { let devices: [DeviceInfo] }
struct TransfersResult: Codable, Sendable { let transfers: [TransferInfo] }
struct SettingsResult: Codable, Sendable { let autoMount: Bool }
struct EmptyResult: Codable, Sendable {}
struct DoctorCheck: Codable, Sendable, Equatable, Identifiable {
    let id: String
    let status: String
    let summary: String
    let remedy: String
}
struct DoctorResult: Codable, Sendable { let ok: Bool; let checks: [DoctorCheck] }

struct MountParams: Codable, Sendable { let deviceId: String; var readOnly: Bool? = nil; var force: Bool? = nil }
struct SettingsParams: Codable, Sendable { let autoMount: Bool }
struct NoParams: Codable, Sendable {}

struct Request<P: Encodable & Sendable>: Encodable, Sendable {
    let protocolVersion = 1
    let requestId: String
    let method: String
    let parameters: P
}

struct Response<R: Decodable & Sendable>: Decodable, Sendable {
    let result: R?
    let error: AgentError?
}

// MARK: - Transport

/// Talks to mtport-agent over its per-user Unix domain socket. Control messages only.
struct AgentClient: Sendable {
    static let maxMessageBytes = 64 * 1024

    let socketURL: URL

    init(socketURL: URL = AgentClient.defaultSocketURL()) { self.socketURL = socketURL }

    static func defaultSocketURL() -> URL {
        if let home = ProcessInfo.processInfo.environment["MTPORT_HOME"], !home.isEmpty {
            return URL(fileURLWithPath: home).appendingPathComponent("Support/runtime/agent.sock")
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("MTPort/runtime/agent.sock")
    }

    static func encode<P: Encodable & Sendable>(method: String, parameters: P, requestId: String = UUID().uuidString) throws -> Data {
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]
        return try enc.encode(Request(requestId: requestId, method: method, parameters: parameters))
    }

    func call<P: Encodable & Sendable, R: Decodable & Sendable>(_ method: String, _ parameters: P, as: R.Type = R.self) async throws -> R {
        let line = try Self.encode(method: method, parameters: parameters)
        let url = socketURL
        let reply = try await Task.detached(priority: .userInitiated) { try Self.roundTrip(url, line) }.value
        let decoded = try JSONDecoder().decode(Response<R>.self, from: reply)
        if let e = decoded.error { throw e }
        guard let r = decoded.result else { throw AgentError(code: "BadResponse", message: "empty result") }
        return r
    }

    func call<R: Decodable & Sendable>(_ method: String, as: R.Type = R.self) async throws -> R {
        try await call(method, NoParams(), as: R.self)
    }

    /// Blocking request/response on a fresh connection (the agent serves one request per connection).
    static func roundTrip(_ socketURL: URL, _ request: Data, timeout: TimeInterval = 3) throws -> Data {
        signal(SIGPIPE, SIG_IGN)   // a vanished agent must surface as an error, not kill the app
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw AgentError(code: "Socket", message: String(cString: strerror(errno))) }
        defer { close(fd) }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, socklen_t(MemoryLayout<Int32>.size))
        var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let path = socketURL.path
        let capacity = MemoryLayout.size(ofValue: addr.sun_path)
        guard path.utf8.count < capacity else { throw AgentError(code: "Socket", message: "socket path too long") }
        withUnsafeMutablePointer(to: &addr.sun_path) { p in
            p.withMemoryRebound(to: CChar.self, capacity: capacity) { _ = strlcpy($0, path, capacity) }
        }
        let rc = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard rc == 0 else { throw AgentError(code: "AgentNotRunning", message: "the MTPort agent is not running") }

        var out = request
        out.append(0x0A)
        try out.withUnsafeBytes { raw in
            var sent = 0
            while sent < raw.count {
                let n = write(fd, raw.baseAddress!.advanced(by: sent), raw.count - sent)
                if n < 0 { if errno == EINTR { continue }; throw AgentError(code: "Socket", message: "write failed") }
                sent += n
            }
        }
        var reply = Data()
        var buf = [UInt8](repeating: 0, count: 4096)
        while reply.count <= maxMessageBytes {
            let n = read(fd, &buf, buf.count)
            if n < 0 { if errno == EINTR { continue }; throw AgentError(code: "Timeout", message: "no answer from the agent") }
            if n == 0 { break }
            reply.append(buf, count: n)
            if reply.last == 0x0A { break }
        }
        guard reply.last == 0x0A else { throw AgentError(code: "BadResponse", message: "truncated answer") }
        reply.removeLast()
        return reply
    }
}
