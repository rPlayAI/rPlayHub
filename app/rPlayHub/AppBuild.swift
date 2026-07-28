//
//  AppBuild.swift
//  Which build is actually running.
//
//  Not cosmetic. A rebuild that silently failed — the linker cannot overwrite a binary while the
//  app is running — left an old build in place and made a fix look ineffective, and there was no
//  way to tell from the window. The stamp is the executable's own modification time, so it cannot
//  drift from the binary the way a hand-edited version constant can: if the title shows an old
//  time, the code being run is old, full stop.
//
import Foundation

enum AppBuild {
    /// Append a line to logs/app.log next to the engine's own logs.
    ///
    /// NSLog from an ad-hoc signed app does not reliably reach the unified log, which made the
    /// USB-capture decision invisible: the app fell back silently and there was no way to tell
    /// whether it was permission, no cable, or a bug. A file we control always works.
    static func log(_ message: String) {
        let line = "\(stampNow) \(message)\n"
        // Create only if absent. createFile() truncates an existing file, so calling it on every
        // write meant the log held exactly one line -- the most recent -- and every earlier line
        // was destroyed. That hid the sequence this file exists to record.
        if !FileManager.default.fileExists(atPath: logPath) {
            FileManager.default.createFile(atPath: logPath, contents: nil)
        }
        if let h = FileHandle(forWritingAtPath: logPath) {
            h.seekToEndOfFile()
            h.write(Data(line.utf8))
            try? h.close()
        }
        NSLog("rPlayHub: \(message)")
    }

    static let logPath: String = {
        // Beside the engine's logs, so one directory holds both halves of a session.
        let root = (Bundle.main.bundlePath as NSString)
            .deletingLastPathComponent as NSString      // .../build/DerivedData/.../Debug
        var dir = root as String
        while !dir.isEmpty, dir != "/", !FileManager.default.fileExists(atPath: dir + "/scripts/live.sh") {
            dir = (dir as NSString).deletingLastPathComponent
        }
        let logs = (dir == "/" || dir.isEmpty) ? NSTemporaryDirectory() : dir + "/logs"
        try? FileManager.default.createDirectory(atPath: logs, withIntermediateDirectories: true)
        return logs + "/app.log"
    }()

    private static var stampNow: String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f.string(from: Date())
    }

    /// The running executable's build time, as "MM-dd HH:mm:ss".
    static let stamp: String = {
        guard let url = Bundle.main.executableURL,
              let values = try? url.resourceValues(forKeys: [.contentModificationDateKey]),
              let date = values.contentModificationDate else { return "unknown" }
        let f = DateFormatter()
        f.dateFormat = "MM-dd HH:mm:ss"
        return f.string(from: date)
    }()
}
