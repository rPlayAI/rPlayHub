//
//  ControlClient.swift
//  Client for the engine's newline-delimited JSON control API (app/api/PROTOCOL.md).
//
//  Calls are serialized on one queue because the engine serializes them anyway: a
//  coredevice.* service channel is a single XPC socket that cannot safely interleave.
//

import AppKit
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

    private var closedByUs = false

    func close() {
        closedByUs = true
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
        return try callSync(method, params, rawResult: false) as? [String: Any] ?? [:]
    }

    private func callSync(_ method: String, _ params: [String: Any], rawResult: Bool) throws -> Any {
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
                    if rawResult { return obj["result"] as Any? ?? [String: Any]() }
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

    /// Restart / shutdown / sleep via diagnostics_relay over the tunnel. The engine answers
    /// with the device's own Status; restart drops the tunnel and the app reconnects after.
    func deviceAction(_ action: String,
                      completion: ((Result<[String: Any], Error>) -> Void)? = nil) {
        send("device_action", ["action": action], completion: completion)
    }

    // MARK: - apps (coredevice.appservice)

    func listApps(completion: @escaping (Result<[[String: Any]], Error>) -> Void) {
        sendRaw("list_apps") { result in
            completion(result.map { ($0 as? [[String: Any]]) ?? [] })
        }
    }

    func listProcesses(completion: @escaping (Result<[[String: Any]], Error>) -> Void) {
        send("list_processes") { result in
            completion(result.map { ($0["processTokens"] as? [[String: Any]]) ?? [] })
        }
    }

    func launchApp(_ bundleID: String,
                   completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("launch_app", ["bundle_id": bundleID], completion: completion)
    }

    func terminateApp(pid: Int, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("terminate_app", ["pid": pid], completion: completion)
    }

    /// `path` is a local `.ipa` (on this machine, not the device); the engine stages it into
    /// /PublicStaging over AFC and installs it via installation_proxy. Device Hub's Apps `+`.
    func installApp(path: String, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("install_app", ["path": path], completion: completion)
    }

    /// Device Hub's Apps `-`.
    func uninstallApp(bundleID: String, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("uninstall_app", ["bundle_id": bundleID], completion: completion)
    }

    /// The Apps-tab row icon, via springboardservices -- confirmed against the live device to be
    /// full-resolution, correctly oriented PNG data, not the rotated/padded raw format older iOS
    /// versions have needed unrotating.
    func getAppIcon(bundleID: String, completion: @escaping (Result<NSImage, Error>) -> Void) {
        send("get_app_icon", ["bundle_id": bundleID]) { result in
            switch result {
            case .failure(let e): completion(.failure(e))
            case .success(let r):
                guard let b64 = r["png_b64"] as? String, let data = Data(base64Encoded: b64),
                      let image = NSImage(data: data) else {
                    completion(.failure(NSError(domain: "ControlClient", code: -1,
                        userInfo: [NSLocalizedDescriptionKey: "bad icon data"])))
                    return
                }
                completion(.success(image))
            }
        }
    }

    // MARK: - profiles (misagent, MCInstall)

    func listProfiles(completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("list_profiles", completion: completion)
    }

    /// `path` is a local `.mobileprovision` or `.mobileconfig`, dispatched by extension on the
    /// engine side. Device Hub's Profiles `+`.
    func installProfile(path: String, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("install_profile", ["path": path], completion: completion)
    }

    /// Device Hub's Profiles `-`. Provisioning profiles remove by `uuid`, configuration profiles
    /// by `identifier` -- the two id fields `list_profiles` already returns.
    func removeProvisioningProfile(uuid: String, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("remove_profile", ["type": "provisioning", "uuid": uuid], completion: completion)
    }

    func removeConfigurationProfile(identifier: String, completion: @escaping (Result<[String: Any], Error>) -> Void) {
        send("remove_profile", ["type": "configuration", "identifier": identifier], completion: completion)
    }

    // MARK: - raw results and streams

    /// Like `send`, but hands back whatever `result` is -- the app list is a JSON array, which
    /// the dictionary-typed path would silently turn into `[:]`.
    private func sendRaw(_ method: String, _ params: [String: Any] = [:],
                         completion: @escaping (Result<Any, Error>) -> Void) {
        queue.async { [weak self] in
            guard let self else { return }
            let result: Result<Any, Error>
            do {
                result = .success(try self.callSync(method, params, rawResult: true))
            } catch {
                result = .failure(error)
            }
            DispatchQueue.main.async { completion(result) }
        }
    }

    /// A streaming method: one request, then `{"event": ...}` objects until the connection ends.
    /// Owns this client's queue for the life of the stream, so use a dedicated ControlClient.
    /// Callbacks are delivered on the main thread; `onEnd` gets nil when the stream closed
    /// because we did.
    func stream(_ method: String, _ params: [String: Any] = [:],
                onEvent: @escaping ([String: Any]) -> Void,
                onEnd: @escaping (Error?) -> Void) {
        queue.async { [weak self] in
            guard let self else { return }
            do {
                _ = try self.callSync(method, params)
                while true {
                    if let nl = self.buffer.firstIndex(of: 0x0A) {
                        let raw = self.buffer.subdata(in: self.buffer.startIndex..<nl)
                        self.buffer.removeSubrange(self.buffer.startIndex...nl)
                        if let obj = try JSONSerialization.jsonObject(with: raw) as? [String: Any] {
                            DispatchQueue.main.async { onEvent(obj) }
                        }
                        continue
                    }
                    guard let chunk = try self.socket.read() else { continue }
                    self.buffer.append(chunk)
                }
            } catch {
                var quiet = false
                if case .closed? = error as? SocketError, self.closedByUs { quiet = true }
                DispatchQueue.main.async { onEnd(quiet ? nil : error) }
            }
        }
    }
}
