import Foundation

/// Polls a MeshMonitor server's REST API v1 for nodes and new messages.
///
/// MeshMonitor has no push feed, so the client asks for the latest messages
/// every few seconds and reports the ones it hasn't seen. Messages that were
/// already there on the first poll are not replayed.
final class MeshMonitorClient {
    struct Config {
        var baseURL: URL
        var token: String
        var source: String
    }

    var onNodes: (([String]) -> Void)?
    var onMessage: ((_ from: String, _ to: String?) -> Void)?
    var onOffline: (() -> Void)?

    static let maxNodes = 160
    private let config: Config
    private let session: URLSession
    private var timers: [Timer] = []
    private var seen = Set<String>()
    private var seenOrder: [String] = []
    private var primed = false
    private var failures = 0

    init(config: Config) {
        self.config = config
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 10
        c.waitsForConnectivity = false
        session = URLSession(configuration: c)
    }

    /// "mesh.example.com:8080" or "http://10.0.0.5:8080/" -> a base URL. No scheme means https.
    static func baseURL(from text: String) -> URL? {
        var s = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return nil }
        if !s.contains("://") { s = "https://" + s }
        while s.hasSuffix("/") { s.removeLast() }
        guard let url = URL(string: s), url.host != nil, ["http", "https"].contains(url.scheme ?? "") else { return nil }
        return url
    }

    func start() {
        stop()
        fetchNodes()
        fetchMessages()
        timers.append(Timer.scheduledTimer(withTimeInterval: 300, repeats: true) { [weak self] _ in self?.fetchNodes() })
        timers.append(Timer.scheduledTimer(withTimeInterval: 4, repeats: true) { [weak self] _ in self?.fetchMessages() })
    }

    func stop() {
        timers.forEach { $0.invalidate() }
        timers = []
    }

    /// One-off check for the settings sheet: "Connected: 42 nodes" or the error.
    func test(completion: @escaping (String) -> Void) {
        get("nodes", query: [URLQueryItem(name: "active", value: "true"), URLQueryItem(name: "sinceDays", value: "7")]) { result in
            switch result {
            case .success(let data): completion("Connected: \(data.count) active nodes")
            case .failure(let error): completion(error.localizedDescription)
            }
        }
    }

    // MARK: - Polling

    private func fetchNodes() {
        get("nodes", query: [URLQueryItem(name: "active", value: "true"), URLQueryItem(name: "sinceDays", value: "7")]) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let items):
                self.failures = 0
                let sorted = items.sorted { ($0["lastHeard"] as? Double ?? 0) > ($1["lastHeard"] as? Double ?? 0) }
                let ids = sorted.compactMap { $0["nodeId"] as? String }.prefix(Self.maxNodes)
                self.onNodes?(Array(ids))
            case .failure:
                self.noteFailure()
            }
        }
    }

    private func fetchMessages() {
        get("messages", query: [URLQueryItem(name: "limit", value: "50")]) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let items):
                self.failures = 0
                var fresh: [(time: Double, from: String, to: String?)] = []
                for item in items {
                    guard let from = item["fromNodeId"] as? String else { continue }
                    let key = Self.key(for: item)
                    if self.seen.contains(key) { continue }
                    self.remember(key)
                    let time = (item["createdAt"] as? Double) ?? (item["timestamp"] as? Double) ?? 0
                    fresh.append((time, from, item["toNodeId"] as? String))
                }
                defer { self.primed = true }
                guard self.primed else { return }
                // Oldest first, spaced out a little so a burst reads as a burst.
                for (i, m) in fresh.sorted(by: { $0.time < $1.time }).enumerated() {
                    DispatchQueue.main.asyncAfter(deadline: .now() + Double(i) * 0.35) { [weak self] in
                        self?.onMessage?(m.from, m.to)
                    }
                }
            case .failure:
                self.noteFailure()
            }
        }
    }

    private static func key(for item: [String: Any]) -> String {
        if let id = item["id"] { return "\(id)" }
        return "\(item["fromNodeId"] ?? "")|\(item["timestamp"] ?? "")|\(item["text"] ?? "")"
    }

    private func remember(_ key: String) {
        seen.insert(key)
        seenOrder.append(key)
        if seenOrder.count > 2000 {
            seenOrder.prefix(1000).forEach { seen.remove($0) }
            seenOrder.removeFirst(1000)
        }
    }

    private func noteFailure() {
        failures += 1
        // About 30 seconds of failed polls before falling back to the simulated mesh.
        if failures == 8 { onOffline?() }
    }

    // MARK: - HTTP

    private struct APIError: LocalizedError {
        let errorDescription: String?
    }

    /// GET /api/v1/sources/{source}/{path}; returns the `data` array of the response.
    private func get(_ path: String, query: [URLQueryItem], completion: @escaping (Result<[[String: Any]], Error>) -> Void) {
        let source = config.source.isEmpty ? "default" : config.source
        let encodedSource = source.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed.subtracting(CharacterSet(charactersIn: "/"))) ?? source
        guard var components = URLComponents(url: config.baseURL, resolvingAgainstBaseURL: false) else {
            completion(.failure(APIError(errorDescription: "Bad server address")))
            return
        }
        let basePath = components.percentEncodedPath
        components.percentEncodedPath = basePath + "/api/v1/sources/\(encodedSource)/\(path)"
        components.queryItems = query
        guard let url = components.url else {
            completion(.failure(APIError(errorDescription: "Bad server address")))
            return
        }
        var request = URLRequest(url: url)
        request.setValue("Bearer \(config.token)", forHTTPHeaderField: "Authorization")
        request.setValue("application/json", forHTTPHeaderField: "Accept")

        session.dataTask(with: request) { data, response, error in
            let result: Result<[[String: Any]], Error>
            if let error {
                result = .failure(error)
            } else if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
                let hint = http.statusCode == 401 ? " (check the API token)" : http.statusCode == 404 ? " (check the source)" : ""
                result = .failure(APIError(errorDescription: "Server answered \(http.statusCode)\(hint)"))
            } else if let data,
                      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                      let items = json["data"] as? [[String: Any]] {
                result = .success(items)
            } else {
                result = .failure(APIError(errorDescription: "Not a MeshMonitor API response"))
            }
            DispatchQueue.main.async { completion(result) }
        }.resume()
    }
}
