//
//  SimulatorList.swift
//  The simulators installed on this Mac, listed the way Device Hub lists them.
//
//  Simulators are not devices on a tunnel; they are local processes managed by CoreSimulator, so
//  none of the RTP, HEVC or HID work applies to them. This talks to `simctl` directly rather than
//  going through cdhost, for the same reason the Diagnostics tab talks to lockdown directly: the
//  daemon is the CoreDevice engine, and a simulator never touches CoreDevice.
//
//  It is macOS-only by construction. Everything else in this project is being aimed at Linux and
//  Windows; this file never will be, and that is a property of the iOS Simulator rather than a
//  shortcut taken here.
//

import Foundation

struct Simulator {
    let udid: String
    let name: String            // "iPhone 17 Pro"
    let runtime: String         // "26.5"
    let isBooted: Bool
    let isAvailable: Bool

    /// Everything installed, newest runtime first, unavailable ones dropped.
    ///
    /// "Unavailable" means the runtime it needs is not installed -- Xcode keeps those entries
    /// around and Device Hub does not show them, because selecting one can only fail.
    static func list() -> [Simulator] {
        guard let data = run(["simctl", "list", "devices", "--json"]),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let byRuntime = root["devices"] as? [String: [[String: Any]]] else { return [] }

        var out: [Simulator] = []
        for (runtimeID, devices) in byRuntime {
            // "com.apple.CoreSimulator.SimRuntime.iOS-26-5" -> "26.5"
            let version = runtimeID.split(separator: ".").last.map {
                $0.replacingOccurrences(of: "iOS-", with: "")
                  .replacingOccurrences(of: "-", with: ".")
            } ?? runtimeID
            for d in devices {
                guard d["isAvailable"] as? Bool == true,
                      let udid = d["udid"] as? String,
                      let name = d["name"] as? String else { continue }
                out.append(Simulator(udid: udid, name: name, runtime: version,
                                     isBooted: (d["state"] as? String) == "Booted",
                                     isAvailable: true))
            }
        }
        // Booted first, then iPhones before iPads, then newest runtime, then name. Thirty-seven
        // rows sorted purely alphabetically buries every iPhone under the iPads, which is the
        // opposite of what anyone is looking for. Device Hub shows a shorter list than this and
        // it is not clear what it filters on -- ordering is the part that is clearly ours to fix.
        func rank(_ s: Simulator) -> Int { s.name.contains("iPad") ? 1 : 0 }
        return out.sorted { a, b in
            if a.isBooted != b.isBooted { return a.isBooted }
            if rank(a) != rank(b) { return rank(a) < rank(b) }
            if a.runtime != b.runtime {
                return a.runtime.compare(b.runtime, options: .numeric) == .orderedDescending
            }
            return a.name.localizedStandardCompare(b.name) == .orderedAscending
        }
    }

    static func boot(_ udid: String) -> String? { fail(run(["simctl", "boot", udid], wantErr: true)) }
    static func shutdown(_ udid: String) -> String? { fail(run(["simctl", "shutdown", udid], wantErr: true)) }

    /// A PNG of the simulator's screen. Works with Simulator.app not running -- verified -- but
    /// takes roughly two thirds of a second, so it is a screenshot and not a video source.
    static func screenshot(_ udid: String, to path: String) -> String? {
        fail(run(["simctl", "io", udid, "screenshot", path], wantErr: true))
    }

    private static func fail(_ data: Data?) -> String? {
        guard let data, let s = String(data: data, encoding: .utf8) else { return nil }
        let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
        return t.isEmpty || t.hasPrefix("Note:") ? nil : t
    }

    private static func run(_ args: [String], wantErr: Bool = false) -> Data? {
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/xcrun")
        task.arguments = args
        let out = Pipe(), err = Pipe()
        task.standardOutput = out
        task.standardError = err
        do { try task.run() } catch { return nil }
        let o = out.fileHandleForReading.readDataToEndOfFile()
        let e = err.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()
        return wantErr ? e : o
    }
}
