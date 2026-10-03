import Foundation
import Network

/// A minimal HTTP/1.1 GET over Network.framework, for plain http:// servers.
/// App Transport Security only covers URLSession, so this works inside the
/// screen saver host, which doesn't allow plain http through URLSession.
enum PlainHTTP {
    private struct Failure: LocalizedError {
        let errorDescription: String?
    }

    private static let maxResponse = 8 * 1024 * 1024
    private static let queue = DispatchQueue(label: "screensaver-mesh.http")

    static func get(_ url: URL, headers: [String: String], timeout: TimeInterval = 10,
                    completion: @escaping (Data?, Int, Error?) -> Void) {
        guard let host = url.host, let port = NWEndpoint.Port(rawValue: UInt16(url.port ?? 80)) else {
            completion(nil, 0, Failure(errorDescription: "Bad server address"))
            return
        }
        var target = url.path.isEmpty ? "/" : url.path
        if let components = URLComponents(url: url, resolvingAgainstBaseURL: false) {
            target = components.percentEncodedPath.isEmpty ? "/" : components.percentEncodedPath
            if let query = components.percentEncodedQuery { target += "?" + query }
        }
        var lines = ["GET \(target) HTTP/1.1",
                     "Host: \(url.port.map { "\(host):\($0)" } ?? host)",
                     "User-Agent: screensaver-mesh",
                     "Connection: close"]
        lines += headers.map { "\($0.key): \($0.value)" }
        let request = Data((lines.joined(separator: "\r\n") + "\r\n\r\n").utf8)

        let connection = NWConnection(host: NWEndpoint.Host(host), port: port, using: .tcp)
        var buffer = Data()
        var finished = false
        let finish: (Data?, Int, Error?) -> Void = { data, status, error in
            guard !finished else { return }
            finished = true
            connection.cancel()
            completion(data, status, error)
        }

        func receive() {
            connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { chunk, _, isComplete, error in
                if let chunk { buffer.append(chunk) }
                if buffer.count > maxResponse {
                    finish(nil, 0, Failure(errorDescription: "Response too large"))
                } else if isComplete || error != nil {
                    if let parsed = parse(buffer) {
                        finish(parsed.body, parsed.status, nil)
                    } else {
                        finish(nil, 0, error ?? Failure(errorDescription: "Not an HTTP response"))
                    }
                } else {
                    receive()
                }
            }
        }

        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                connection.send(content: request, completion: .contentProcessed { error in
                    if let error { finish(nil, 0, error) } else { receive() }
                })
            case .failed(let error):
                finish(nil, 0, error)
            case .waiting(let error):
                finish(nil, 0, error)
            default:
                break
            }
        }
        connection.start(queue: queue)
        queue.asyncAfter(deadline: .now() + timeout) {
            finish(nil, 0, Failure(errorDescription: "The server didn't answer"))
        }
    }

    /// Splits a complete response into its status and body, decoding chunked bodies.
    static func parse(_ response: Data) -> (status: Int, body: Data)? {
        let separator = Data("\r\n\r\n".utf8)
        guard let split = response.range(of: separator),
              let head = String(data: response[..<split.lowerBound], encoding: .isoLatin1) else { return nil }
        let headerLines = head.components(separatedBy: "\r\n")
        let statusParts = headerLines.first?.split(separator: " ") ?? []
        guard statusParts.count >= 2, statusParts[0].hasPrefix("HTTP/"), let status = Int(statusParts[1]) else { return nil }

        var body = Data(response[split.upperBound...])
        let chunked = headerLines.dropFirst().contains {
            let lower = $0.lowercased()
            return lower.hasPrefix("transfer-encoding:") && lower.contains("chunked")
        }
        if chunked {
            guard let decoded = dechunk(body) else { return nil }
            body = decoded
        }
        return (status, body)
    }

    private static func dechunk(_ data: Data) -> Data? {
        var out = Data()
        var index = data.startIndex
        let crlf = Data("\r\n".utf8)
        while index < data.endIndex {
            guard let lineEnd = data.range(of: crlf, in: index..<data.endIndex),
                  let sizeLine = String(data: data[index..<lineEnd.lowerBound], encoding: .ascii),
                  let size = Int(sizeLine.split(separator: ";").first?.trimmingCharacters(in: .whitespaces) ?? "", radix: 16)
            else { return nil }
            if size == 0 { return out }
            let start = lineEnd.upperBound
            guard data.distance(from: start, to: data.endIndex) >= size + 2 else { return nil }
            let end = data.index(start, offsetBy: size)
            out.append(data[start..<end])
            index = data.index(end, offsetBy: 2)
        }
        return nil
    }
}
