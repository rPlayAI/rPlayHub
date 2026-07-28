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
