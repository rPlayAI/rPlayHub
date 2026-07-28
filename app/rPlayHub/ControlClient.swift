//
//  ControlClient.swift
//  Client for the engine's newline-delimited JSON control API (app/api/PROTOCOL.md).
//
//  Calls are serialized on one queue because the engine serializes them anyway: a
//  coredevice.* service channel is a single XPC socket that cannot safely interleave.
//

import Foundation

struct ControlError: Error, CustomStringConvertible {
    let code: String
    let message: String
    var description: String { "\(code): \(message)" }
}

final class ControlClient {
    private let socket: TCPSocket
    private let queue = DispatchQueue(label: "rplayhub.control")
    private var buffer = Data()
    private var nextID = 0

    init(host: String = "127.0.0.1", port: UInt16 = 9876) {
        socket = TCPSocket(host: host, port: port)
    }

    func connect() throws {
        try socket.connect(timeout: 30)      // a screenshot can take a moment
    }

    func close() {
        socket.shutdownAndClose()
    }

    // MARK: - calls

    /// Fire-and-forget: used for taps and swipes, where blocking the UI would be wrong.
    func send(_ method: String, _ params: [String: Any] = [:],
              completion: ((Result<[String: Any], Error>) -> Void)? = nil) {
        queue.async { [weak self] in
            guard let self else { return }
            let result: Result<[String: Any], Error>
            do {
                result = .success(try self.callSync(method, params))
            } catch {
                result = .failure(error)
            }
            if let completion {
                DispatchQueue.main.async { completion(result) }
            }
        }
    }

    private func callSync(_ method: String, _ params: [String: Any]) throws -> [String: Any] {
        nextID += 1
        var req: [String: Any] = ["id": nextID, "method": method]
        if !params.isEmpty { req["params"] = params }
        var line = try JSONSerialization.data(withJSONObject: req)
        line.append(0x0A)
        try socket.writeAll(line)

        while true {
            if let nl = buffer.firstIndex(of: 0x0A) {
                let raw = buffer.subdata(in: buffer.startIndex..<nl)
                buffer.removeSubrange(buffer.startIndex...nl)
                guard let obj = try JSONSerialization.jsonObject(with: raw) as? [String: Any] else {
                    throw ControlError(code: "bad_response", message: "not a JSON object")
                }
                if obj["ok"] as? Bool == true {
                    return obj["result"] as? [String: Any] ?? [:]
                }
                let err = obj["error"] as? [String: Any] ?? [:]
                throw ControlError(code: err["code"] as? String ?? "internal_error",
                                   message: err["message"] as? String ?? "(no message)")
            }
            guard let chunk = try socket.read() else { continue }
            buffer.append(chunk)
        }
    }

    // MARK: - convenience

    /// Taps at a normalized position. `fx`/`fy` are 0..1 across the device screen.
    func tap(fx: Double, fy: Double, durationMS: Int = 60) {
        send("tap", ["fx": fx, "fy": fy, "duration_ms": durationMS]) { result in
            if case .failure(let e) = result { NSLog("rPlayHub: tap failed — \(e)") }
        }
    }

    func swipe(fx0: Double, fy0: Double, fx1: Double, fy1: Double, durationMS: Int = 300) {
        send("swipe", ["fx0": fx0, "fy0": fy0, "fx1": fx1, "fy1": fy1,
                       "duration_ms": durationMS]) { result in
            if case .failure(let e) = result { NSLog("rPlayHub: swipe failed — \(e)") }
        }
    }

    /// Start recording the live stream on the engine side. `path` is engine-local; omit it and
    /// the engine picks a timestamped file under `recordings/`.
    func startRecording(path: String? = nil,
                        completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("start_recording", path.map { ["path": $0] } ?? [:], completion: completion)
    }

    func stopRecording(completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("stop_recording", completion: completion)
    }

    func listDevices(completion: @escaping (Result<[[String: Any]], Error>) -> Void) {
        send("list_devices") { result in
            completion(result.map { ($0["devices"] as? [[String: Any]]) ?? [] })
        }
    }

    func streamInfo(completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("stream_info", completion: completion)
    }
}
