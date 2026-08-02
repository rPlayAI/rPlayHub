//
//  DeviceInfo.swift
//  What lockdown knows about the device, fetched for the Diagnostics tab.
//
//  The query itself lives in `host/deviceinfo.py` and is run as a subprocess. That is deliberate
//  and temporary: lockdown, pairing and diagnostics_relay exist today only in Python, and
//  rewriting them in Swift would have to be undone when the C port lands for Linux and Windows.
//  Shelling out keeps exactly one implementation of the wire format while the UI is built.
//
//  The important property is that none of this touches the CoreDevice tunnel. Lockdown rides
//  usbmuxd, so the panel answers while the tunnel is down, while the stream has stopped, and even
//  while Apple's own Device Hub holds the device -- which is precisely when someone wants to look
//  at it.
//

import Foundation

struct DeviceInfo {
    /// Section title -> ordered key/value rows, already formatted for display.
    let sections: [(title: String, rows: [(String, String)])]
    /// Keys lockdown refused, kept apart so the panel can say "could not read" rather than
    /// silently showing a shorter list than last time.
    let unavailable: [String]
    /// The raw identifier, before any prettying. The sidebar needs it to name the model and pick
    /// a glyph, and the formatted rows have already lost it.
    let productType: String?

    enum Failure: Error, LocalizedError {
        case noCheckout
        case launchFailed(String)
        case deviceError(String)

        var errorDescription: String? {
            switch self {
            case .noCheckout:
                return "Device info needs the source checkout (host/deviceinfo.py), "
                     + "which an installed copy does not have."
            case .launchFailed(let why): return why
            case .deviceError(let why): return why
            }
        }
    }

    /// Human labels for the raw lockdown keys. Anything absent here is shown as-is rather than
    /// hidden -- a key we have not labelled yet is still worth seeing.
    private static let labels: [String: String] = [
        "DeviceName": "Name", "ProductType": "Model Identifier", "ProductVersion": "iOS Version",
        "BuildVersion": "Build", "ProductName": "System", "UniqueDeviceID": "UDID",
        "SerialNumber": "Serial Number", "ModelNumber": "Model Number", "RegionInfo": "Region",
        "HardwareModel": "Hardware Model", "CPUArchitecture": "Architecture",
        "DeviceClass": "Class", "ChipID": "Chip ID", "ECID": "ECID",
        "ActivationState": "Activation", "PasswordProtected": "Passcode",
        "TimeZone": "Time Zone", "WiFiAddress": "Wi-Fi Address",
        "BluetoothAddress": "Bluetooth Address", "EthernetAddress": "Ethernet Address",
        "FirmwareVersion": "Firmware",
        "BatteryCurrentCapacity": "Charge", "BatteryIsCharging": "Charging",
        "ExternalConnected": "Power Connected",
        "TotalDiskCapacity": "Disk Capacity", "TotalDataCapacity": "Data Capacity",
        "TotalDataAvailable": "Data Available",
    ]

    private static let sectionTitles: [String: String] = [
        "default": "Device",
        "com.apple.mobile.battery": "Battery",
        "com.apple.disk_usage": "Storage",
    ]

    /// Keys whose raw value is a byte count. Shown as GB, because 232589340672 tells nobody
    /// anything at a glance.
    private static let byteKeys: Set<String> = [
        "TotalDiskCapacity", "TotalDataCapacity", "TotalDataAvailable",
        "TotalSystemCapacity", "TotalSystemAvailable", "AmountDataAvailable", "AmountDataReserved",
    ]

    private static func format(_ key: String, _ value: Any) -> String {
        if byteKeys.contains(key), let n = (value as? NSNumber)?.doubleValue {
            return String(format: "%.1f GB", n / 1_000_000_000)
        }
        if key == "BatteryCurrentCapacity", let n = (value as? NSNumber)?.intValue {
            return "\(n)%"
        }
        if let b = value as? Bool { return b ? "Yes" : "No" }
        if let n = value as? NSNumber { return n.stringValue }
        return String(describing: value)
    }

    /// Run the query off the main thread and deliver the result on it.
    static func fetch(udid: String?, completion: @escaping (Result<DeviceInfo, Error>) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try fetchSync(udid: udid) }
            DispatchQueue.main.async { completion(result) }
        }
    }

    private static func fetchSync(udid: String?) throws -> DeviceInfo {
        guard let root = AppBuild.repoRoot else { throw Failure.noCheckout }

        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        var args = ["python3", root + "/host/deviceinfo.py"]
        if let udid, !udid.isEmpty { args += ["--udid", udid] }
        task.arguments = args
        task.currentDirectoryURL = URL(fileURLWithPath: root + "/host")
        let out = Pipe(), err = Pipe()
        task.standardOutput = out
        task.standardError = err

        do { try task.run() } catch { throw Failure.launchFailed("could not run python3: \(error)") }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        let errData = err.fileHandleForReading.readDataToEndOfFile()
        task.waitUntilExit()

        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            let stderrText = String(data: errData, encoding: .utf8) ?? ""
            let tail = stderrText.split(separator: "\n").suffix(3).joined(separator: " ")
            throw Failure.launchFailed(tail.isEmpty ? "no output from deviceinfo.py" : tail)
        }
        if let e = json["error"] as? String { throw Failure.deviceError(e) }

        var sections: [(String, [(String, String)])] = []
        // Fixed order, so the panel does not reshuffle between refreshes.
        for key in ["default", "com.apple.mobile.battery", "com.apple.disk_usage"] {
            guard let bucket = json[key] as? [String: Any], !bucket.isEmpty else { continue }
            let rows = bucket.keys.sorted { a, b in
                (labels[a] ?? a).localizedCaseInsensitiveCompare(labels[b] ?? b) == .orderedAscending
            }.map { (labels[$0] ?? $0, format($0, bucket[$0]!)) }
            sections.append((sectionTitles[key] ?? key, rows))
        }

        var unavailable = (json["unavailable"] as? [String: Any])?.keys.sorted() ?? []
        if let s = json["session_error"] as? String {
            // Worth surfacing on its own: without a session the battery and storage sections are
            // simply absent, which otherwise looks like the device not reporting them.
            unavailable.insert("session (\(s))", at: 0)
        }
        let productType = (json["default"] as? [String: Any])?["ProductType"] as? String
        return DeviceInfo(sections: sections, unavailable: unavailable, productType: productType)
    }
}
