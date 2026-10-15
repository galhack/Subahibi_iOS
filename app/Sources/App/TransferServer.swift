import Foundation
import Network

/// Receives the game files from a Mac over the local network
/// (tools/push_data.py): a minimal HTTP server, one request per connection.
///
///   GET /status            the files already received, as JSON
///   PUT /files/<name>      a file of the game folder; the body is the file
///
/// Every request must carry the pairing code shown on the device
/// (X-Subahibi-Code), a new random one each time the server is switched on,
/// so nobody else on the network can write into the app.  Names are plain
/// file names of the kinds a game folder holds (.arc archives, the patch
/// executable, the font, ...): anything with a separator, a dot file or
/// another extension is refused.  A file is written next to
/// its final place and renamed when complete, so an interrupted transfer
/// never leaves a truncated archive behind.
final class TransferServer: ObservableObject {
    static let port: UInt16 = 8765

    @Published private(set) var running = false
    @Published private(set) var lastEvent = ""
    @Published private(set) var bytesReceived: Int64 = 0
    /// The pairing code of this session, shown by the launcher.
    @Published private(set) var code = ""

    var onFileReceived: (() -> Void)?

    private var listener: NWListener?
    private let queue = DispatchQueue(label: "transfer")
    /// The code and the guessing guard, shared by the main thread and `queue` under `lock`.
    private let lock = NSLock()
    private var activeCode = ""
    private var failures: [Date] = []
    private var lockedUntil = Date.distantPast
    private static let maxFailuresPerMinute = 5

    /// At most five wrong codes a minute; then every request is refused for a
    /// minute, so a 6-digit code cannot be guessed and the lock lifts by itself.
    private func checkCode(_ given: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let now = Date()
        if now < lockedUntil || activeCode.isEmpty { return false }
        if given == activeCode { return true }
        failures = failures.filter { now.timeIntervalSince($0) < 60 } + [now]
        if failures.count >= Self.maxFailuresPerMinute {
            lockedUntil = now.addingTimeInterval(60)
            failures.removeAll()
            DispatchQueue.main.async { self.lastEvent = "配对码错误次数过多，暂停接收 1 分钟。" }
        }
        return false
    }

    func start() {
        guard listener == nil else { return }
        code = String(format: "%06d", Int.random(in: 0..<1_000_000))
        lock.lock()
        activeCode = code
        failures.removeAll()
        lockedUntil = .distantPast
        lock.unlock()
        do {
            let params = NWParameters.tcp
            params.allowLocalEndpointReuse = true
            let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: Self.port)!)
            listener.service = NWListener.Service(name: "Subahibi", type: "_subahibi._tcp")
            listener.newConnectionHandler = { [weak self] conn in self?.accept(conn) }
            listener.stateUpdateHandler = { [weak self] state in
                DispatchQueue.main.async {
                    switch state {
                    case .ready: self?.running = true
                    case .failed(let error):
                        self?.running = false
                        self?.lastEvent = "监听失败：\(error)"
                    case .cancelled: self?.running = false
                    default: break
                    }
                }
            }
            listener.start(queue: queue)
            self.listener = listener
        } catch {
            lastEvent = "无法开启传输服务：\(error)"
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
        running = false
        code = ""
        lock.lock()
        activeCode = ""
        lock.unlock()
    }

    /// The device's IPv4 addresses on Wi-Fi / Ethernet, for the launcher to show.
    static func addresses() -> [String] {
        var result: [String] = []
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return result }
        defer { freeifaddrs(ifaddr) }
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = ptr.pointee
            guard let addr = ifa.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: ifa.ifa_name)
            guard name.hasPrefix("en") else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                result.append(String(cString: host))
            }
        }
        return result
    }

    // MARK: - connections

    private func accept(_ conn: NWConnection) {
        conn.start(queue: queue)
        readHeader(conn, buffer: Data())
    }

    private func readHeader(_ conn: NWConnection, buffer: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, complete, error in
            guard let self else { return }
            var buffer = buffer
            if let data { buffer.append(data) }
            if let range = buffer.range(of: Data("\r\n\r\n".utf8)) {
                let head = String(decoding: buffer[..<range.lowerBound], as: UTF8.self)
                let rest = buffer[range.upperBound...]
                self.handle(conn, head: head, initialBody: Data(rest))
            } else if error != nil || complete || buffer.count > 64 * 1024 {
                conn.cancel()
            } else {
                self.readHeader(conn, buffer: buffer)
            }
        }
    }

    private func handle(_ conn: NWConnection, head: String, initialBody: Data) {
        let lines = head.components(separatedBy: "\r\n")
        let parts = (lines.first ?? "").split(separator: " ")
        guard parts.count >= 2 else { return respond(conn, 400, "bad request") }
        let method = String(parts[0]), path = String(parts[1])
        var length: Int64 = 0
        var given = ""
        for line in lines.dropFirst() {
            let kv = line.split(separator: ":", maxSplits: 1)
            guard kv.count == 2 else { continue }
            let key = kv[0].trimmingCharacters(in: .whitespaces).lowercased()
            let value = kv[1].trimmingCharacters(in: .whitespaces)
            if key == "content-length" { length = Int64(value) ?? 0 }
            if key == "x-subahibi-code" { given = value }
        }
        guard checkCode(given) else { return respond(conn, 403, "wrong pairing code or locked") }
        guard length >= 0 && length < (4 << 30) else { return respond(conn, 400, "bad length") }
        if method == "GET" && path == "/status" {
            return respond(conn, 200, statusJSON(), type: "application/json")
        }
        if method == "PUT" && path.hasPrefix("/files/") {
            let raw = String(path.dropFirst("/files/".count))
            guard let name = raw.removingPercentEncoding, Self.isSafe(name) else {
                return respond(conn, 400, "bad file name")
            }
            return receiveFile(conn, name: name, length: length, initialBody: initialBody)
        }
        respond(conn, 404, "not found")
    }

    /// The kinds of files a game folder holds.
    static let allowedExtensions: Set<String> = ["arc", "exe", "ttf", "gdb", "hvl", "txt", "lst"]

    static func isSafe(_ name: String) -> Bool {
        guard !name.isEmpty, name.count < 200, !name.hasPrefix("."),
              name.unicodeScalars.allSatisfy({ $0.value >= 0x20 && $0 != "/" && $0 != "\\" && $0 != ":" }) else { return false }
        let ext = (name as NSString).pathExtension.lowercased()
        return allowedExtensions.contains(ext)
    }

    private func receiveFile(_ conn: NWConnection, name: String, length: Int64, initialBody: Data) {
        let target = GamePaths.transferTarget
        let final = target.appendingPathComponent(name)
        let partial = target.appendingPathComponent(".\(name).part")
        FileManager.default.createFile(atPath: partial.path, contents: nil)
        guard let handle = try? FileHandle(forWritingTo: partial) else {
            return respond(conn, 500, "cannot write")
        }
        var written: Int64 = 0
        func write(_ data: Data) -> Bool {
            do { try handle.write(contentsOf: data) } catch { return false }
            written += Int64(data.count)
            DispatchQueue.main.async { self.bytesReceived += Int64(data.count) }
            return true
        }
        func finish(ok: Bool) {
            try? handle.close()
            if ok && written == length {
                try? FileManager.default.removeItem(at: final)
                do {
                    try FileManager.default.moveItem(at: partial, to: final)
                } catch {
                    respond(conn, 500, "cannot rename")
                    return
                }
                DispatchQueue.main.async {
                    self.lastEvent = "已接收 \(name)"
                    self.onFileReceived?()
                }
                respond(conn, 200, "ok")
            } else {
                try? FileManager.default.removeItem(at: partial)
                respond(conn, 400, "incomplete")
            }
        }
        if !initialBody.isEmpty && !write(initialBody.prefix(Int(length))) {
            return finish(ok: false)
        }
        func next() {
            if written >= length { return finish(ok: true) }
            conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { data, _, complete, error in
                if let data, !data.isEmpty, !write(data) { return finish(ok: false) }
                if error != nil || (complete && written < length) { return finish(ok: false) }
                next()
            }
        }
        next()
    }

    private func statusJSON() -> String {
        let target = GamePaths.transferTarget
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: target.path)) ?? [])
            .filter { !$0.hasPrefix(".") }.sorted()
        let files = names.map { name -> [String: Any] in
            let size = (try? FileManager.default.attributesOfItem(atPath: target.appendingPathComponent(name).path)[.size]
                as? NSNumber)?.int64Value ?? 0
            return ["name": name, "size": size]
        }
        let data = (try? JSONSerialization.data(withJSONObject: ["files": files], options: [.sortedKeys])) ?? Data()
        return String(decoding: data, as: UTF8.self)
    }

    private func respond(_ conn: NWConnection, _ code: Int, _ body: String, type: String = "text/plain") {
        let reason = [200: "OK", 400: "Bad Request", 403: "Forbidden", 404: "Not Found",
                      500: "Internal Server Error"][code] ?? "Error"
        let bytes = Data(body.utf8)
        var head = "HTTP/1.1 \(code) \(reason)\r\nContent-Type: \(type); charset=utf-8\r\n"
        head += "Content-Length: \(bytes.count)\r\nConnection: close\r\n\r\n"
        conn.send(content: Data(head.utf8) + bytes, completion: .contentProcessed { _ in conn.cancel() })
    }
}
