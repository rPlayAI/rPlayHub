//
//  EngineService.swift
//  Manages the embedded engine daemon (cdhost) as an SMAppService launchd daemon.
//
//  The app is a client of cdhost, which must run as root to create the tunnel interface. Rather
//  than have the user run `sudo cdhost` by hand, the daemon is embedded in the app bundle
//  (Contents/MacOS/cdhost + Contents/Library/LaunchDaemons/<plist>) and registered with
//  SMAppService.daemon, which macOS runs as root after a one-time approval in System Settings.
//  This is the DECIDED design in app/DISTRIBUTION.md, and the Developer-ID equivalent of what
//  Apple's own Device Hub does with remoted.
//
//  It is additive: during development the daemon is not embedded, so this reports .notEmbedded
//  and the app simply talks to whatever cdhost is already listening on :9876 (a manual run).
//

import Foundation
import ServiceManagement

enum EngineService {
    static let plistName = "com.rplay.rplayhub.engine.plist"

    enum State {
        case notEmbedded          // no daemon plist in this build (dev builds) -- use a manual cdhost
        case enabled              // registered and running as root
        case requiresApproval     // registered; the user must approve it in System Settings
        case notRegistered        // embedded but not yet registered
        case failed(String)
    }

    /// Whether this build actually carries the daemon. A Debug build run from Xcode does not.
    static var isEmbedded: Bool {
        Bundle.main.url(forResource: "com.rplay.rplayhub.engine",
                        withExtension: "plist",
                        subdirectory: "Library/LaunchDaemons") != nil
            || FileManager.default.fileExists(atPath:
                Bundle.main.bundlePath + "/Contents/Library/LaunchDaemons/" + plistName)
    }

    private static var service: SMAppService {
        SMAppService.daemon(plistName: plistName)
    }

    static var state: State {
        guard isEmbedded else { return .notEmbedded }
        switch service.status {
        case .enabled:          return .enabled
        case .requiresApproval: return .requiresApproval
        case .notRegistered:    return .notRegistered
        case .notFound:         return .notRegistered
        @unknown default:       return .notRegistered
        }
    }

    /// Register the daemon. On first registration macOS marks it "requires approval" and the user
    /// must switch it on in System Settings > General > Login Items & Extensions. Returns the
    /// resulting state.
    ///
    /// Self-heals the classic trap of launching the app from Downloads (or the DMG) once before
    /// dragging it to /Applications: that leaves a registration pointing at the old, now-gone
    /// path, which shadows the good one. If we are already enabled/approval-pending we leave it;
    /// otherwise a fresh register() from the current bundle location supersedes any stale record.
    @discardableResult
    static func enable() -> State {
        guard isEmbedded else { return .notEmbedded }
        // A prior registration from a different location can leave the daemon stuck. Clearing it
        // first makes register() bind to wherever this bundle now lives.
        if case .notRegistered = state { try? service.unregister() }
        do {
            try service.register()
            return state
        } catch {
            if case .enabled = state { return .enabled }
            if case .requiresApproval = state { return .requiresApproval }
            return .failed("\(error.localizedDescription)")
        }
    }

    /// Where macOS runs the app from. SMAppService only trusts a daemon whose app lives in
    /// /Applications; from Downloads or a mounted DMG the approval never sticks.
    static var isInApplications: Bool {
        Bundle.main.bundlePath.hasPrefix("/Applications/")
    }

    static func disable() {
        guard isEmbedded else { return }
        try? service.unregister()
    }

    /// Opens the Login Items pane so the user can approve the daemon.
    static func openApprovalSettings() {
        SMAppService.openSystemSettingsLoginItems()
    }
}
