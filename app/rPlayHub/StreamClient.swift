//
//  StreamClient.swift
//  Reads the engine's live Annex-B HEVC stream on a blocking background thread.
//

import Foundation

final class StreamClient {
    private let socket: TCPSocket
    private var thread: Thread?
    private var stopping = false
    private let parser = AnnexBParser()

    /// Called on the stream thread for every NAL unit. Keep the work here cheap.
    var onNAL: ((Data) -> Void)?
    /// Called on the main queue when the connection ends, with a reason.
    var onDisconnect: ((String) -> Void)?
    /// Called on the main queue once the first bytes arrive.
    var onFirstBytes: (() -> Void)?

    private(set) var bytesReceived = 0

    /// Set RPLAYHUB_TEE=/path/to/file to write every byte this client receives, verbatim.
    ///
    /// The engine has been proven to emit decodable video (an independent decoder rendered 382
    /// frames from a 10s capture) while the app showed snow from the same stream. Either the app
    /// receives something different, or it mishandles what it receives — and comparing this file
    /// with the engine's own recording answers that in one step instead of by inspection.
    private lazy var tee: FileHandle? = {
        guard let path = ProcessInfo.processInfo.environment["RPLAYHUB_TEE"], !path.isEmpty
        else { return nil }
        FileManager.default.createFile(atPath: path, contents: nil)
        let h = FileHandle(forWritingAtPath: path)
        NSLog("rPlayHub: teeing the received stream to \(path)")
        return h
    }()

    init(host: String = "127.0.0.1", port: UInt16 = 9877) {
        socket = TCPSocket(host: host, port: port)
    }

    func start() throws {
        try socket.connect()
        stopping = false
        let t = Thread { [weak self] in self?.readLoop() }
        t.name = "rplayhub.video"
        t.stackSize = 1 << 20
        thread = t
        t.start()
    }

    func stop() {
        stopping = true
        socket.shutdownAndClose()
        try? tee?.close()
    }

    private func readLoop() {
        var announced = false
        while !stopping {
            do {
                guard let chunk = try socket.read() else { continue }   // timeout, retry
                bytesReceived += chunk.count
                if let tee { try? tee.write(contentsOf: chunk) }
                if !announced {
                    announced = true
                    DispatchQueue.main.async { [weak self] in self?.onFirstBytes?() }
                }
                parser.feed(chunk) { nal in
                    self.onNAL?(nal)
                }
            } catch {
                if !stopping {
                    let reason = "\(error)"
                    DispatchQueue.main.async { [weak self] in self?.onDisconnect?(reason) }
                }
                return
            }
        }
    }
}
