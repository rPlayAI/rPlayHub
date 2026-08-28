//
//  AppDelegate.swift
//  Window, engine connection, and the status line.
//
//  The app is a *client*. The engine (host/mirror.py today, the C core later) owns the
//  privileged part — creating the tunnel interface needs root — and publishes video on
//  127.0.0.1:9877 and control on 127.0.0.1:9876. Keeping that split is what lets the app
//  ship unprivileged and be signed and notarized normally.
//

import AppKit
import AVFoundation

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private var view: MirrorView!
    private var sidebar: DeviceSidebar!
    private var inspector: InspectorPane!
    private var split: NSSplitView!
    private let screenWindow = ScreenWindow()
    private var strip: ControlStrip!
    private var stage: NSView!
    private var controls: ControlPanel { inspector.controls }
    private var isPinned = false
    private var stream: StreamClient?
    private var control: ControlClient?
    private var hevc: HEVCStream?
    private var usb: USBMirror?
    private var usbAttempts = 0
    private var directStream: DirectStream?
    /// Kept so the status timer can compare what we acknowledged against what we actually decoded.
    private var videoDecoder: VideoDecoder?
    /// Last RTP loss count written to the log, so only changes are recorded.
    private var lastLoggedLoss: UInt64 = 0
    private var loggedRTPOnce = false
    private var lastRTPLogAt: TimeInterval = 0
    private var idleFlush: Timer?
    private var askedForCamera = false

    private var recordItem: NSMenuItem?
    private var isRecording = false
    private var lastRecordingPath: String?
    private var retryTimer: Timer?
    /// True from the start of connect() until it has either got video or given up.
    private var connecting = false
    /// True once the user has clicked View Screen for the currently-selected device -- Device
    /// Hub's device pane shows a static picture and a button until then, gating the actual video
    /// pipeline (not just its display) behind the click. Reset on a deliberate device switch;
    /// kept across a background reconnect so video resumes on its own.
    private var wantsVideo = false
    private var statusTimer: Timer?
    private var deviceLabel = "no device"
    /// Which device the daemon is currently bound to, so re-selecting it is a no-op.
    private var boundUDID: String?
    private var lastFrameCount = 0
    private var fps = 0

    private let videoPort: UInt16 = 9877
    private let controlPort: UInt16 = 9876

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Load the capture plug-in now: it takes about a second to expose devices.
        USBMirror.prime()
        enableEngineIfEmbedded()
        buildMenu()
        buildWindow()
        connect()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.updateStatus()
            self?.refreshStillIfIdle()
        }
    }

    // MARK: - still picture while no video flows

    private var stillInFlight = false
    private var lastStillAt: TimeInterval = 0
    private var framesAtLastTick = 0
    private var quietTicks = 0

    /// Device Hub's device pane shows the phone's current screen as a still picture before View
    /// Screen, and that is all it can show for a device that cannot mirror. Do the same: while
    /// no frames are arriving, take a screenshot every few seconds and put it where the video
    /// would be. The first frame of real video hides it.
    private func refreshStillIfIdle() {
        // Every video path -- the engine proxy AND the direct RTP stream -- ends at
        // displayLayer.present, so its counter is the one signal that means "a picture arrived".
        // hevc.framesEnqueued only moves on the proxy path, so watching it painted a screenshot
        // over live direct-stream video and left clicks landing on a frozen still.
        let frames = view.displayLayer.framesPresented
        if frames != framesAtLastTick {
            framesAtLastTick = frames
            quietTicks = 0
            view.hideStill()
            return
        }
        quietTicks += 1
        // Two quiet seconds before the first still, so a stream that is merely starting up does
        // not flash a screenshot; then one every four seconds -- a full-screen PNG is a
        // multi-megabyte round trip over the tunnel and the phone renders it on demand.
        guard quietTicks >= 2, let control, !stillInFlight,
              Date().timeIntervalSinceReferenceDate - lastStillAt >= 4 else { return }
        stillInFlight = true
        control.send("take_screenshot") { [weak self] result in
            guard let self else { return }
            self.stillInFlight = false
            self.lastStillAt = Date().timeIntervalSinceReferenceDate
            guard case .success(let info) = result,
                  let b64 = info["image_b64"] as? String,
                  let data = Data(base64Encoded: b64),
                  let image = NSImage(data: data)?.cgImage(forProposedRect: nil, context: nil,
                                                           hints: nil),
                  // Video may have started while the screenshot was in flight.
                  self.view.displayLayer.framesPresented == self.framesAtLastTick else { return }
            let hadSize = self.view.presentedSize.width > 0
            self.view.showStill(image)
            if !hadSize { self.applySizing() }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        stream?.stop()
        control?.close()
    }

    // MARK: - UI

    private func buildWindow() {
        // Portrait-ish default; the real aspect ratio is applied once the stream reports it.
        let rect = NSRect(x: 0, y: 0, width: 390, height: 844)
        window = NSWindow(contentRect: rect,
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered,
                          defer: false)
        window.title = "rPlayHub — build \(AppBuild.stamp)"
        window.center()
        window.setFrameAutosaveName("MirrorWindow")

        // Device Hub's shape: device list on the left, the device in the middle, an inspector on
        // the right. Ours puts the live screen in the middle and our own controls on the right.
        view = MirrorView(frame: rect)
        sidebar = DeviceSidebar(frame: NSRect(x: 0, y: 0, width: 250, height: rect.height))
        inspector = InspectorPane(frame: NSRect(x: 0, y: 0, width: 260, height: rect.height))

        // Selecting a device mirrors it, as it does in Device Hub. The daemon binds one device
        // at a time -- the tunnel and the media session belong to it -- so this asks the daemon
        // to rebind and then reconnects once it is back. Simulators are skipped: they have no
        // tunnel to rebind, and their rows do other things.
        sidebar.onSelect = { [weak self] device in
            guard let self, !device.isSimulator, let control = self.control else { return }
            guard device.udid != self.boundUDID else { return }
            AppBuild.log("selecting device \(device.udid)")
            control.send("select_device", ["udid": device.udid]) { [weak self] result in
                guard let self else { return }
                if case .success(let r) = result, (r["rebinding"] as? Bool) == true {
                    self.boundUDID = device.udid
                    self.deviceLabel = "switching device…"
                    self.updateStatus()
                    // A newly-selected device always starts at the static-picture-and-button
                    // state, matching Device Hub -- it does not carry over "already watching"
                    // from whatever was selected before.
                    self.wantsVideo = false
                    // The daemon re-executes, so the socket goes away and comes back. Give it
                    // time to rebuild the tunnel before reconnecting; too eager and we connect
                    // to the dying process.
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4) { self.reconnect() }
                } else if case .failure(let e) = result {
                    self.present(title: "Could not switch device", text: "\(e)")
                }
            }
        }
        sidebar.onCommand = { [weak self] command, device in
            self?.perform(command, on: device)
        }
        view.onCommand = { [weak self] command in self?.perform(command, on: nil) }
        view.onViewScreen = { [weak self] in self?.viewScreenTapped() }
        controls.onAction = { [weak self] action in
            switch action {
            case .pin:        self?.perform(.pin, on: nil)
            case .home:       self?.perform(.home, on: nil)
            case .rotate:     self?.perform(.rotate, on: nil)
            case .screenshot: self?.perform(.screenshot, on: nil)
            case .record:     self?.perform(.record, on: nil)
            case .restart:    self?.powerAction("restart")
            case .shutdown:   self?.powerAction("shutdown")
            case .sleep:      self?.powerAction("sleep")
            }
        }

        split = NSSplitView(frame: NSRect(x: 0, y: 0, width: 250 + rect.width + 260,
                                              height: rect.height))
        split.isVertical = true
        split.dividerStyle = .thin
        split.autoresizingMask = [.width, .height]
        // Screen with its actions directly underneath, as Device Hub arranges them: the buttons
        // act on the picture, so they sit with it rather than off in the inspector.
        strip = ControlStrip()
        strip.onAction = { [weak self] action in
            switch action {
            case .pin:        self?.perform(.pin, on: nil)
            case .home:       self?.perform(.home, on: nil)
            case .rotate:     self?.perform(.rotate, on: nil)
            case .screenshot: self?.perform(.screenshot, on: nil)
            case .record:     self?.perform(.record, on: nil)
            case .restart:    self?.powerAction("restart")
            case .shutdown:   self?.powerAction("shutdown")
            case .sleep:      self?.powerAction("sleep")
            }
        }
        stage = NSView()
        for sub in [view as NSView, strip as NSView] {
            sub.translatesAutoresizingMaskIntoConstraints = false
            stage.addSubview(sub)
        }
        NSLayoutConstraint.activate([
            // Breathing room around the screen, as Device Hub leaves. Flush against the title
            // bar the picture reads as part of the window chrome rather than as a device.
            view.topAnchor.constraint(equalTo: stage.topAnchor, constant: 16),
            view.leadingAnchor.constraint(equalTo: stage.leadingAnchor, constant: 12),
            view.trailingAnchor.constraint(equalTo: stage.trailingAnchor, constant: -12),
            strip.topAnchor.constraint(equalTo: view.bottomAnchor),
            strip.leadingAnchor.constraint(equalTo: stage.leadingAnchor),
            strip.trailingAnchor.constraint(equalTo: stage.trailingAnchor),
            strip.bottomAnchor.constraint(equalTo: stage.bottomAnchor),
        ])

        split.addArrangedSubview(sidebar)
        split.addArrangedSubview(stage)
        split.addArrangedSubview(inspector)
        // Visible by default, as Device Hub's inspector is. It was hidden while the pane held
        // only buttons that duplicated the screen's right-click menu; now that the Info tab
        // reports what the device actually is, hiding it means the first thing anyone wants is
        // behind a menu item they have to find. View > Hide Controls still puts it away.
        split.setHoldingPriority(NSLayoutConstraint.Priority(260), forSubviewAt: 0)
        split.setHoldingPriority(NSLayoutConstraint.Priority(240), forSubviewAt: 1)
        split.setHoldingPriority(NSLayoutConstraint.Priority(260), forSubviewAt: 2)

        // Without explicit widths the split view squeezes the side panes and the rows clip.
        // Equal-constant at a lower priority sets the resting width; the >= keeps them usable.
        for (pane, width) in [(sidebar as NSView, 250.0), (inspector as NSView, 260.0)] {
            pane.translatesAutoresizingMaskIntoConstraints = false
            let resting = pane.widthAnchor.constraint(equalToConstant: width)
            resting.priority = NSLayoutConstraint.Priority(700)
            resting.isActive = true
            pane.widthAnchor.constraint(greaterThanOrEqualToConstant: width - 60).isActive = true
        }

        window.contentView = split
        window.setContentSize(NSSize(width: 250 + 390 + 260, height: 844))
        buildToolbar()
        window.makeFirstResponder(view)
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Device Hub's toolbar carries a "Hide Sidebar" button and an overflow popup. The sidebar
    /// toggle is the part that matters: it is how you get to a screen-only window.
    private func buildToolbar() {
        let toolbar = NSToolbar(identifier: "main")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        window.toolbar = toolbar
        window.toolbarStyle = .unified
    }

    @objc private func toggleSidebar() {
        sidebar.isHidden.toggle()
    }

    /// Bring the embedded root engine up via SMAppService on launch. No-op in dev builds, which
    /// carry no daemon and expect a manually-run cdhost.
    private func enableEngineIfEmbedded() {
        guard EngineService.isEmbedded else { return }
        // SMAppService only trusts a daemon whose app is in /Applications. Say so plainly rather
        // than letting the approval silently fail to stick -- the exact trap the first tester hit.
        if !EngineService.isInApplications {
            let a = NSAlert()
            a.messageText = "Move rPlayHub to Applications"
            a.informativeText = "Drag rPlayHub into your Applications folder and open it from "
                + "there. Its background engine can only be approved from /Applications, not from "
                + "the disk image or Downloads."
            a.addButton(withTitle: "OK")
            a.runModal()
            return
        }
        switch EngineService.enable() {
        case .enabled:
            break                       // already approved; the engine is up
        case .requiresApproval, .notRegistered:
            promptEngineApproval()
        case .failed(let why):
            present(title: "Could not register the engine", text: why)
        case .notEmbedded:
            break
        }
    }

    private func promptEngineApproval() {
        let alert = NSAlert()
        alert.messageText = "Turn on the rPlayHub engine"
        alert.informativeText = "In the window that opens, find rPlayHub under \u{201C}Allow in "
            + "the Background\u{201D} and switch it ON (it starts OFF). That one-time approval "
            + "lets the engine reach your device; after it, rPlayHub connects on its own."
        alert.addButton(withTitle: "Open Login Items")
        alert.addButton(withTitle: "Later")
        if alert.runModal() == .alertFirstButtonReturn {
            EngineService.openApprovalSettings()
            // Poll: when the user flips it on, the engine comes up and connect() succeeds.
            pollEngineUp(attempts: 60)
        }
    }

    private func pollEngineUp(attempts: Int) {
        guard attempts > 0 else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self else { return }
            if case .enabled = EngineService.state {
                self.reconnect()        // engine is up; (re)connect to it
            } else {
                self.pollEngineUp(attempts: attempts - 1)
            }
        }
    }

    @objc private func manageEngine() {
        switch EngineService.state {
        case .notEmbedded:
            present(title: "No embedded engine",
                    text: "This build has no bundled engine. Run the engine yourself with "
                        + "\u{2018}sudo ./host-c/cdhost\u{2019}, which is how development builds work.")
        case .enabled:
            present(title: "Engine is running",
                    text: "The rPlayHub engine is enabled and runs at startup with system "
                        + "privileges. Manage it under System Settings > General > Login Items.")
        case .requiresApproval, .notRegistered:
            _ = EngineService.enable()
            EngineService.openApprovalSettings()
        case .failed(let why):
            present(title: "Engine problem", text: why)
        }
    }

    private func buildMenu() {
        let mainMenu = NSMenu()

        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About rPlayHub",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        recordItem = appMenu.addItem(withTitle: "Start Recording",
                                     action: #selector(toggleRecording), keyEquivalent: "r")
        appMenu.addItem(withTitle: "Engine (Background Service)…",
                        action: #selector(manageEngine), keyEquivalent: "").target = self
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Reveal Last Recording",
                        action: #selector(revealRecording), keyEquivalent: "R")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Reconnect", action: #selector(reconnect), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide rPlayHub", action: #selector(NSApplication.hide(_:)),
                        keyEquivalent: "h")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit rPlayHub",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        // Standard Edit menu. Without it, Cut/Copy/Paste and Select All don't work in any text
        // field (the search box, filters) — the actions route to the first responder.
        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "Cut", action: Selector(("cut:")), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: Selector(("copy:")), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: Selector(("paste:")), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Delete", action: Selector(("delete:")), keyEquivalent: "")
        editMenu.addItem(withTitle: "Select All", action: Selector(("selectAll:")), keyEquivalent: "a")
        editItem.submenu = editMenu

        let viewItem = NSMenuItem()
        mainMenu.addItem(viewItem)
        let viewMenu = NSMenu(title: "View")
        viewMenu.addItem(withTitle: "Show Controls", action: #selector(toggleControls),
                         keyEquivalent: "i")
        viewItem.submenu = viewMenu

        // Device menu — the actions Device Hub groups for the selected device.
        let deviceItem = NSMenuItem()
        mainMenu.addItem(deviceItem)
        let deviceMenu = NSMenu(title: "Device")
        deviceMenu.addItem(withTitle: "Take Screenshot",
                           action: #selector(menuScreenshot), keyEquivalent: "s").target = self
        deviceMenu.addItem(withTitle: "Home",
                           action: #selector(menuHome), keyEquivalent: "H").target = self
        deviceMenu.addItem(.separator())
        deviceMenu.addItem(withTitle: "Restart…",
                           action: #selector(menuRestart), keyEquivalent: "").target = self
        deviceMenu.addItem(withTitle: "Shut Down…",
                           action: #selector(menuShutdown), keyEquivalent: "").target = self
        deviceMenu.addItem(withTitle: "Sleep",
                           action: #selector(menuSleep), keyEquivalent: "").target = self
        deviceItem.submenu = deviceMenu

        let windowItem = NSMenuItem()
        mainMenu.addItem(windowItem)
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Minimize",
                           action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)),
                           keyEquivalent: "")
        windowItem.submenu = windowMenu

        let helpItem = NSMenuItem()
        mainMenu.addItem(helpItem)
        let helpMenu = NSMenu(title: "Help")
        let help = helpMenu.addItem(withTitle: "rPlayHub Help", action: #selector(openHelp),
                                    keyEquivalent: "?")   // ⌘? is the standard Help shortcut
        help.target = self
        helpMenu.addItem(.separator())
        let sdk = helpMenu.addItem(withTitle: "rPlayHub SDK on GitHub",
                                   action: #selector(openSDK), keyEquivalent: "")
        sdk.target = self
        helpItem.submenu = helpMenu

        NSApp.mainMenu = mainMenu
        NSApp.windowsMenu = windowMenu
        NSApp.helpMenu = helpMenu   // routes the Help-menu search field and ⌘? here
    }

    /// Where the Help menu points: the help page served by GitHub Pages from the SDK repo, so it
    /// renders as a real page rather than as source. (Source lives in `app/rPlayHub/Help/` and is
    /// published to the SDK repo's `docs/`, which Pages serves.)
    private static let helpURL = "https://rplayai.github.io/rplayhub-sdk/help.html"
    private static let sdkURL  = "https://github.com/rPlayAI/rplayhub-sdk"

    @objc private func openHelp() {
        if let url = URL(string: Self.helpURL) { NSWorkspace.shared.open(url) }
    }

    @objc private func openSDK() {
        if let url = URL(string: Self.sdkURL) { NSWorkspace.shared.open(url) }
    }

    // Device-menu actions, routed to the same handlers the control strip / right-click use.
    @objc private func menuScreenshot() { perform(.screenshot, on: nil) }
    @objc private func menuHome()       { perform(.home, on: nil) }
    @objc private func menuRestart()    { powerAction("restart") }
    @objc private func menuShutdown()   { powerAction("shutdown") }
    @objc private func menuSleep()      { powerAction("sleep") }

    // MARK: - engine connection

    // MARK: - recording
    //
    // Recording happens in the ENGINE, not here: it already has every NAL and the cached
    // parameter sets, so it can write a self-describing file without the app re-encoding
    // anything. The app only toggles it and reports where the file went.

    @objc private func toggleRecording() {
        guard let control else {
            present(title: "Not connected", text: "The engine is not reachable yet.")
            return
        }
        if isRecording {
            control.stopRecording { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let info):
                    self.isRecording = false
                    self.recordItem?.title = "Start Recording"
                    self.strip.setRecording(false)
                    self.lastRecordingPath = info["path"] as? String
                    let nals = info["nals"] as? Int ?? 0
                    let bytes = info["bytes"] as? Int ?? 0
                    let seconds = info["duration_s"] as? Double ?? 0
                    let keyframe = info["saw_keyframe"] as? Bool ?? false
                    var text = String(format: "%@\n\n%d NAL units, %.1f MB, %.1fs.",
                                      self.lastRecordingPath ?? "?", nals,
                                      Double(bytes) / 1_048_576, seconds)
                    if !keyframe {
                        // Worth surfacing: the device only emits an IRAP every ~10s, so a short
                        // recording can contain no keyframe and will start mid-GOP.
                        text += "\n\nNo keyframe was captured, so playback starts mid-GOP."
                    }
                    self.present(title: "Recording saved", text: text)
                case .failure(let e):
                    self.present(title: "Could not stop recording", text: "\(e)")
                }
            }
        } else {
            control.startRecording { [weak self] result in
                guard let self else { return }
                switch result {
                case .success(let info):
                    self.isRecording = true
                    self.recordItem?.title = "Stop Recording"
                    self.strip.setRecording(true)
                    self.lastRecordingPath = info["path"] as? String
                case .failure(let e):
                    self.present(title: "Could not start recording", text: "\(e)")
                }
            }
        }
    }

    @objc private func revealRecording() {
        guard let path = lastRecordingPath else {
            present(title: "No recording yet", text: "Record something first.")
            return
        }
        NSWorkspace.shared.selectFile(path, inFileViewerRootedAtPath: "")
    }

    private func present(title: String, text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.alertStyle = .informational
        alert.runModal()
    }

    @objc private func toggleControls(_ sender: NSMenuItem) {
        inspector.setHidden(!inspector.isHidden)
        sender.title = inspector.isHidden ? "Show Controls" : "Hide Controls"
    }

    /// The subset of actions a simulator understands. Boot and shutdown are ours to perform;
    /// a screenshot works even with Simulator.app closed, which is worth keeping.
    private func performSimulator(_ command: DeviceSidebar.Command, on device: DeviceRow) {
        switch command {
        case .reconnect:
            let booting = !device.connected
            let err = booting ? Simulator.boot(device.udid)
                              : Simulator.shutdown(device.udid)
            if let err {
                present(title: booting ? "Could not boot" : "Could not shut down", text: err)
            }
            if let control { refreshDevices(control) }

        case .screenshot:
            let path = NSTemporaryDirectory() + "\(device.name) \(Int(Date().timeIntervalSince1970)).png"
            if let err = Simulator.screenshot(device.udid, to: path) {
                present(title: "Screenshot failed", text: err)
            } else {
                NSWorkspace.shared.open(URL(fileURLWithPath: path))
            }

        case .copyUDID:
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(device.udid, forType: .string)

        case .shutdown:
            if let err = Simulator.shutdown(device.udid) { present(title: "Could not shut down", text: err) }
            if let control { refreshDevices(control) }

        default:
            // Mirroring a simulator needs a frame source we do not have: it is not a display on
            // this Mac (checked -- booting one adds no screen), simctl's recordVideo buffers to
            // disk instead of streaming, and screenshots come back at about 1.5 a second. The
            // routes left are Simulator.app's window through ScreenCaptureKit, or SimulatorKit,
            // which is private.
            present(title: "Not available for simulators",
                    text: "Boot, Shut Down, Screenshot and Copy UDID work. Live mirroring and "
                        + "input need a frame source for simulators, which is not built yet.")
        }
    }

    private func perform(_ command: DeviceSidebar.Command, on device: DeviceRow?) {
        // A simulator answers to almost none of this: no tunnel, no pairing, no HID surfaces.
        // Route the few that do mean something and say so plainly for the rest, rather than
        // sending a tap into a daemon that is talking to a different device entirely.
        if let device, device.isSimulator {
            performSimulator(command, on: device)
            return
        }
        switch command {
        case .openInNewTab, .openInNewWindow:
            // One live stream, one MirrorView: this moves the view rather than making a second
            // one, because two views cannot share a display layer and decoding the stream twice
            // would double the only expensive thing in this app.
            // The stage carries the screen AND its action strip, so the detached window gets
            // both -- a screen in a window of its own with no way to act on it would be worse
            // than not detaching at all.
            screenWindow.open(stage: stage, from: split, title: window.title,
                              tabbedWith: command == .openInNewTab ? window : nil)

        case .pin:
            isPinned.toggle()
            window.level = isPinned ? .floating : .normal
            strip.setPinned(isPinned)

        case .record:
            toggleRecording()

        case .screenshot:
            guard let control else { present(title: "Not connected",
                                             text: "The engine is not reachable yet."); return }
            control.send("take_screenshot") { [weak self] result in
                switch result {
                case .success(let info):
                    self?.saveScreenshot(info)
                case .failure(let e):
                    self?.present(title: "Screenshot failed", text: "\(e)")
                }
            }

        case .rotate:
            // Turns the view, not the device: iOS orientation follows the phone's own sensors
            // and CoreDevice exposes no verb to override it. Rotating what we show is the half
            // that is ours to do, and MirrorView rotates the input mapping with it.
            view.rotate()
            applySizing()

        case .home:
            guard let control else { present(title: "Not connected",
                                             text: "The engine is not reachable yet."); return }
            control.send("press_button", ["button": "home"]) { [weak self] result in
                if case .failure(let e) = result {
                    // Expected for now, and the message says exactly what is missing.
                    self?.present(title: "Home is not implemented yet", text: "\(e)")
                }
            }

        case .copyUDID:
            let udid = device?.udid ?? sidebar.selectedRow()?.udid ?? ""
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(udid, forType: .string)

        case .reconnect:
            reconnect()

        case .restart:  powerAction("restart")
        case .shutdown: powerAction("shutdown")
        case .sleep:    powerAction("sleep")
        }
    }

    /// Restart / Shutdown / Sleep through diagnostics_relay. Each confirms first: the buttons are
    /// icon-only, a restart drops the tunnel for a minute, and a shutdown leaves the phone off
    /// until someone presses its side button.
    private func powerAction(_ action: String) {
        guard let control else {
            present(title: "Not connected", text: "The engine is not reachable yet.")
            return
        }
        let (verb, note): (String, String) = {
            switch action {
            case "restart":  return ("Restart", "The device reboots and the mirror reconnects "
                                     + "when it is back; expect about a minute without a picture.")
            case "shutdown": return ("Shut Down", "The device powers off. It must be turned back "
                                     + "on by hand before anything here works again.")
            default:         return ("Sleep", "Locks the screen. The mirror keeps running and "
                                     + "shows the lock screen.")
            }
        }()
        let alert = NSAlert()
        alert.messageText = "\(verb) \(deviceLabel == "no device" ? "the device" : deviceLabel)?"
        alert.informativeText = note
        alert.alertStyle = action == "sleep" ? .informational : .warning
        alert.addButton(withTitle: verb)
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        control.deviceAction(action) { [weak self] result in
            switch result {
            case .success:
                if action == "restart" {
                    // The tunnel dies with the device; reconnect once it has had time to boot.
                    self?.deviceLabel = "restarting…"
                    self?.updateStatus()
                    DispatchQueue.main.asyncAfter(deadline: .now() + 45) { self?.reconnect() }
                }
            case .failure(let e):
                self?.present(title: "\(verb) failed", text: "\(e)")
            }
        }
    }

    private func saveScreenshot(_ info: [String: Any]) {
        guard let b64 = info["image_b64"] as? String,
              let data = Data(base64Encoded: b64) else {
            present(title: "Screenshot failed", text: "no image in the response")
            return
        }
        let stamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
        let url = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Desktop/rPlayHub-\(stamp).png")
        do {
            try data.write(to: url)
            NSWorkspace.shared.selectFile(url.path, inFileViewerRootedAtPath: "")
        } catch {
            present(title: "Could not save the screenshot", text: "\(error)")
        }
    }

    @objc private func reconnect() {
        directStream?.stop()
        directStream = nil
        stream?.stop()
        control?.close()
        stream = nil
        control = nil
        inspector.control = nil
        DeviceInfo.engine = nil
        hevc = nil
        // Drop the previous device's still at once, and reset the still timer so a fresh capture
        // is taken on the next idle tick rather than the old picture lingering for up to four
        // seconds after a device switch.
        view.hideStill()
        framesAtLastTick = 0
        quietTicks = 0
        lastStillAt = 0
        connect()
    }

    /// Try the USB capture path. Returns false if no cabled iPhone is available, in which case
    /// the caller falls back to the CoreDevice video stream.
    ///
    /// Control still runs over CoreDevice either way: touch goes through universalhidservice, and
    /// nothing about the picture changes that.
    private func startUSBMirror() -> Bool {
        // Prompt on first run. Asking is asynchronous, so a fresh grant cannot help this attempt —
        // reconnect once the answer arrives and the next attempt will take the USB path.
        // Raw value, not my interpretation of it: 0 notDetermined, 1 restricted, 2 denied,
        // 3 authorized. The friendly string said "denied" while TCC held no record for this
        // bundle at all, and those cannot both be true — so log the number AVFoundation actually
        // returns, and whether asking changes it.
        let raw = AVCaptureDevice.authorizationStatus(for: .video).rawValue
        AppBuild.log("USB capture: authorizationStatus raw=\(raw) "
                     + "(0 notDetermined, 1 restricted, 2 denied, 3 authorized); "
                     + "\(USBMirror.muxedDeviceSummary)")
        // Ask ONCE per launch, and only when the answer is still open.
        //
        // Asking repeatedly is not free: a process with no GUI session to draw a prompt in gets an
        // immediate denial, and that denial is recorded permanently. The retry loop below runs
        // several times a second, so an unconditional request here burned the undecided state on
        // the first attempt and left a denial that looked like the user had refused.
        if raw == AVAuthorizationStatus.notDetermined.rawValue, !askedForCamera {
            askedForCamera = true
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                AppBuild.log("requestAccess returned \(granted) "
                             + "(status now \(AVCaptureDevice.authorizationStatus(for: .video).rawValue))")
                if granted { DispatchQueue.main.async { self?.reconnect() } }
            }
        }
        // Ask if we have never asked, but do not wait for the answer or make it a precondition —
        // try the capture regardless and report what actually fails.
        let mirror = USBMirror()
        mirror.onFrame = { [weak self] picture in self?.view.displayLayer.present(picture) }
        mirror.onSize = { [weak self] size in
            guard let self else { return }
            // The plug-in delivers the screen itself, with none of the 16-pixel encoder padding
            // the CoreDevice path carries, so the frame IS the screen and must not be cropped.
            self.view.videoSize = size
            self.view.deviceSize = size
            self.applySizing()
        }
        guard mirror.start() else {
            // The capture plug-in loads asynchronously, so "no device" a fraction of a second
            // after launch is normal rather than final. Retry before settling for the CoreDevice
            // stream: without this the USB path never engaged, because the first enumeration
            // always lost the race and losing was indistinguishable from having no cable.
            if usbAttempts < 6 {
                usbAttempts += 1
                AppBuild.log("USB capture not ready (attempt \(usbAttempts)) — \(mirror.status)")
                // Retry only the capture, not the whole connection. Calling reconnect() here
                // tore down and rebuilt the CoreDevice viewer on every attempt, which showed up
                // as a burst of "viewer connected" churn in the engine and had each new viewer
                // arriving mid-stream.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.7) { [weak self] in
                    guard let self, self.usb == nil else { return }
                    if self.startUSBMirror() {
                        // Capture took over, so stop pulling the engine's stream.
                        self.stream?.stop()
                        self.stream = nil
                    }
                }
                return false
            }
            AppBuild.log("USB capture unavailable — \(mirror.status); using the CoreDevice stream")
            return false
        }
        usb = mirror
        retryTimer?.invalidate()
        retryTimer = nil
        return true
    }

    private func connect() {
        // One connect at a time.
        //
        // connect() finishes asynchronously -- tunnel_info is a round trip -- so two calls can be
        // in flight at once: a reconnect scheduled after switching device, and the retry timer
        // firing a second later. Both then negotiate a media stream, and the device permits one
        // per session, so the second killed the first and no packets arrived at all. Overlap is
        // the failure, not the second call.
        guard !connecting else {
            AppBuild.log("connect already in progress; ignoring")
            return
        }
        connecting = true

        // Tear down the previous receiver before building another.
        //
        // directStream was assigned and never stopped -- not here, not in reconnect(). Each retry
        // therefore left a live receiver behind, holding its socket and answering the device's
        // Sender Reports on its own. Seventy-one of them accumulated in half a minute of retrying,
        // all acknowledging the same stream, and nothing decoded.
        directStream?.stop()
        directStream = nil

        // The picture first, and independently of the engine.
        //
        // USB capture goes straight to the cable and needs nothing from the engine — but it used
        // to sit behind the control connection, so an engine that failed to start took the video
        // with it even though the two share nothing. That is exactly what happened when a tunnel
        // died on startup: capture was never attempted at all.
        //
        // Preferred over the CoreDevice stream because that stream is capped by the device at
        // 1184x2544 / 6 Mbps -- about 0.03 bits per pixel, which visibly falls apart the moment
        // anything moves. Apple's Device Hub negotiates identical numbers and shows identical
        // artefacting, so it is a property of that transport rather than something to fix.
        // USB capture is opt-in and off by default: RPLAYHUB_USB_CAPTURE=1.
        //
        // It needs camera access, because macOS presents a tethered iPhone through the camera
        // subsystem, and that turned into a long detour for no picture. The CoreDevice path now
        // recovers from artefacting on its own, so it is the sane default and nothing here should
        // pester about permissions to reach it.
        // USB capture goes straight to the cable and needs nothing from the engine, so this is
        // attempted before the control connection below rather than after it -- only if the user
        // has already opted into video for this device, though: Device Hub shows a static
        // picture and a View Screen button until clicked, and starting capture before that would
        // show a live picture the click was supposed to gate.
        var usbRunning = false
        if wantsVideo {
            let wantUSB = ProcessInfo.processInfo.environment["RPLAYHUB_USB_CAPTURE"] == "1"
            usbRunning = wantUSB && (usb != nil || startUSBMirror())
        }

        let c = ControlClient(port: controlPort)
        do {
            try c.connect()
        } catch {
            // Control is how taps reach the phone, so its absence still deserves a retry — but
            // not at the cost of a picture that is already working.
            if EngineService.isEmbedded, EngineService.isInApplications {
                if case .enabled = EngineService.state {} else {
                    // The engine is bundled but not switched on yet -- point the user at the toggle.
                    deviceLabel = "engine not enabled"
                    updateStatus()
                    promptEngineApproval()
                    return
                }
            }
            scheduleRetry(because: "engine not reachable on port \(controlPort)")
            return
        }
        control = c
        view.control = c
        inspector.control = c
        DeviceInfo.engine = c

        if usbRunning { connecting = false; return }

        refreshDevices(c)
        connecting = false

        // Device Hub's device pane shows a static picture and a View Screen button until
        // clicked; the actual video pipeline below is deferred until then (or run immediately
        // here on a reconnect, once the user has already opted in for this device).
        guard wantsVideo else {
            updateViewScreenPrompt()
            return
        }
        startVideo(c)
    }

    /// The actual video pipeline: USB capture if opted in, else the RTP/proxy path. Split out of
    /// connect() so the View Screen click can run it directly against the control connection
    /// connect() already established, without repeating that handshake.
    private func startVideo(_ c: ControlClient) {
        // Prefer receiving RTP ourselves, with nothing in the data path.
        //
        // The daemon only has to create the utun; once it exists the tunnel addresses are
        // ordinary routes and this process can open sockets on them unprivileged. Taking the
        // stream directly removes the daemon-as-producer / app-as-consumer coupling entirely --
        // a viewer reading slowly can no longer apply backpressure to a real-time source that
        // cannot retransmit. Falls back to the loopback Annex-B stream when the engine does not
        // offer tunnel_info, which is the case for the Python one.
        // RPLAYHUB_VIDEO selects the route explicitly:
        //   direct  take RTP ourselves; fail rather than fall back
        //   proxy   read Annex-B from the engine over loopback
        //   auto    direct when the engine offers tunnel_info (default)
        //
        // The fallback exists because the Python engine has no tunnel_info, and because a proxy
        // is still the right shape when something other than this app wants the stream -- ffplay,
        // a recorder, a second window. Choosing it explicitly beats inferring it: a silent
        // fallback makes "which path am I actually testing" unanswerable.
        let route = ProcessInfo.processInfo.environment["RPLAYHUB_VIDEO"] ?? "auto"
        if route == "proxy" {
            AppBuild.log("video route: proxy (requested)")
            startProxiedStream(c)
            return
        }

        DirectStream.fetchTunnel(c) { [weak self] tunnel in
            guard let self else { return }
            guard let tunnel else {
                if route == "direct" {
                    AppBuild.log("video route: direct requested but the engine offers no "
                                 + "tunnel_info; not falling back")
                    return
                }
                AppBuild.log("video route: proxy (engine offers no tunnel_info)")
                self.startProxiedStream(c)
                return
            }
            let decoder = self.makeDecoder()
            let direct = DirectStream()
            // Hand each NAL straight to the decoder. The depacketizer already knows exactly
            // where every NAL starts and ends, so writing start codes and then scanning for them
            // again is not just wasted work: the Annex-B parser cannot know a NAL has ended until
            // the NEXT start code arrives, so it holds the newest one back. Over the proxy that
            // meant every frame waited for the following frame's bytes. Here the boundaries are
            // known, so nothing waits.
            // Submit each picture the moment the transport says it is complete.
            direct.onEndOfFrame = { [weak decoder] in decoder?.endAccessUnit() }
            direct.onNAL = { [weak decoder] framed in
                guard framed.count > 4 else { return }
                decoder?.handle(nal: framed.subdata(in: 4..<framed.count))
            }
            // Packets went missing inside an access unit, so stop decoding until a keyframe
            // anchors the chain again rather than predicting from a reference nobody received.
            direct.onDiscontinuity = { [weak decoder] in decoder?.signalDiscontinuity() }
            // Deliberately unused. This came from rp_rtp_active_rect's reading of the RTP
            // header extension, and that mapping was measured to be wrong -- it flips tiers
            // several times a second on a still screen. The real per-frame size comes from the
            // trailer on the last slice NAL (HEVCStream.parseActiveRectTrailer), which agrees
            // with what avconferenced passes its decoder. Left unwired rather than deleted so
            // the C side keeps a place to publish from if the extension is ever decoded.
            if direct.start(tunnel) {
                self.directStream = direct
                // Video is flowing, so stop retrying. Without this the 2-second retry timer keeps
                // calling connect() forever, and every call built another receiver.
                self.retryTimer?.invalidate()
                self.retryTimer = nil
            } else if route == "direct" {
                AppBuild.log("video route: direct requested but the stream did not start")
                // Say so on screen. A black window with the reason only in a log file is what
                // made the same device limitation look like a fresh bug several times over.
                self.deviceLabel = "no video — the device did not start a stream"
                self.updateStatus()
            } else {
                AppBuild.log("video route: falling back to proxy, direct did not start")
                self.startProxiedStream(c)
            }
        }
    }

    /// Shows the device's name/OS and a View Screen button in place of video, as Device Hub does
    /// until it is clicked. Re-derives the text each time since refreshDevices(_:) learns the
    /// real name/OS asynchronously, after the prompt may already be showing a placeholder.
    private func updateViewScreenPrompt() {
        guard !wantsVideo else { return }
        if let row = sidebar.selectedRow() {
            view.showViewScreenPrompt(name: row.name, os: "iOS \(row.version)")
        } else {
            view.showViewScreenPrompt(name: deviceLabel, os: "")
        }
    }

    /// Device Hub's View Screen: starts the actual video pipeline against the control connection
    /// connect() already has, without repeating that handshake.
    private func viewScreenTapped() {
        guard let c = control else { return }
        wantsVideo = true
        view.hideViewScreenPrompt()
        startVideo(c)
    }

    /// Populate the sidebar and learn the real screen size.
    ///
    /// Belongs to neither video path. It was left inside the proxied one during the direct-path
    /// refactor, so the device silently disappeared from the sidebar whenever video came straight
    /// from the phone.
    private func refreshDevices(_ c: ControlClient) {
        c.listDevices { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let devices):
                let rows = devices.map(DeviceRow.init(json:))
                if let bound = devices.first(where: { ($0["bound"] as? Bool) == true }),
                   let u = bound["udid"] as? String { self.boundUDID = u }
                // Simulators share the list, as they do in Device Hub. They come from simctl
                // rather than the engine -- a simulator never touches CoreDevice -- so the two
                // sources are merged here rather than pretended to be one upstream.
                self.sidebar.update(rows + Simulator.list().map(DeviceRow.init(simulator:)))
                // The engine's device list has no product type, so the subtitle would stay blank.
                // Lockdown knows it, and asking costs nothing the user waits on: the row fills in
                // when the answer arrives. Only for rows that still lack one, so this does not
                // re-query on every refresh tick.
                for r in rows where r.productType == nil {
                    DeviceInfo.fetch(udid: r.udid) { [weak self] result in
                        guard case .success(let info) = result, let pt = info.productType,
                              let self else { return }
                        self.sidebar.setModel(pt, forUDID: r.udid)
                        self.view.productType = pt
                        // Also the crop, and it OVERRIDES whatever is there. onFormat below
                        // fills deviceSize with the coded size as a stopgap the moment the SPS
                        // arrives, which is always before this answer comes back; leaving that
                        // in place makes visibleFraction 1x1 and the alignment padding shows as
                        // black bands. A real screen size outranks the frame it was padded into.
                        if let native = DeviceModel.screenSize(for: pt),
                           self.view.deviceSize != native {
                            self.view.deviceSize = native
                            self.applySizing()
                            AppBuild.log("screen size from model \(pt): "
                                       + "\(Int(native.width))x\(Int(native.height))")
                        }
                    }
                }
                if let d = devices.first {
                    let model = d["product_type"] as? String ?? "iPhone"
                    let os = d["os_version"] as? String ?? "?"
                    self.deviceLabel = "\(model) · iOS \(os)"
                    // The real screen size, which is smaller than the coded frame: the encoder
                    // pads up to 16-pixel alignment. MirrorView crops and maps clicks with it.
                    //
                    // Two spellings, same reason as os_version/product_version above: mirror.py
                    // nests it as screen_size{w,h}, cdhost emits flat screen_width/screen_height.
                    // Reading only the nested one left deviceSize at zero against the C daemon,
                    // visibleFraction fell back to 1x1, and the padding was never cropped -- the
                    // black bands at the top and bottom of the picture were the alignment rows.
                    let nested = d["screen_size"] as? [String: Any]
                    let w = (nested?["w"] as? NSNumber)?.doubleValue
                        ?? (d["screen_width"] as? NSNumber)?.doubleValue
                    let h = (nested?["h"] as? NSNumber)?.doubleValue
                        ?? (d["screen_height"] as? NSNumber)?.doubleValue
                    if let w, let h, w > 0, h > 0 {
                        self.view.deviceSize = CGSize(width: w, height: h)
                        self.applySizing()
                    }
                } else {
                    self.deviceLabel = "engine has no device"
                }
                // The View Screen prompt may already be showing a placeholder from before this
                // answer arrived; refresh it with the real name/OS now that sidebar.update(...)
                // above has them.
                self.updateViewScreenPrompt()
            case .failure(let e):
                self.deviceLabel = "list_devices failed: \(e)"
            }
        }
    }

    /// Build the decoder and the idle flush. Both video paths need exactly this, and having it
    /// in one place is what stops the direct path feeding a decoder that was never created --
    /// which it did, silently, on the first attempt.
    @discardableResult
    private func makeDecoder() -> HEVCStream {
        // Decode is explicit and unconditional; the layer only ever shows the newest picture.
        let vt = VideoDecoder()
        self.videoDecoder = vt
        vt.onFrame = { [weak self] picture in self?.view.displayLayer.present(picture) }
        let decoder = HEVCStream(decoder: vt)
        decoder.onFormat = { [weak self] size in
            guard let self else { return }
            self.view.videoSize = size          // coded size, padding included
            // The direct path has no engine telling us the screen size, and an unknown one would
            // leave the crop disabled. The coded frame is the screen plus 16-pixel alignment
            // padding, so fall back to it rather than showing nothing.
            if self.view.deviceSize == .zero { self.view.deviceSize = size }
            self.applySizing()
        }
        // ONE publisher for the active size, and it is the display layer -- the size arrives
        // attached to the very picture being enqueued. Wiring the parser's callback here as well
        // looks equivalent and is not: parsing runs on the receive thread, ahead of decode and
        // display, so it would keep re-sizing the layer to describe a frame that is not on
        // screen yet, and the picture visibly vibrates while the encoder flaps between tiers.
        view.displayLayer.onPresentSize = { [weak self] size in
            guard let self else { return }
            if self.view.activeSize != size {
                // Log every tier change. This is the measurement for whether the encoder can be
                // told to stop adapting resolution (RVRA1:0) -- with it honoured there should be
                // no line here but the first, however hard the screen is swiped.
                AppBuild.log("coded tier: \(Int(size.width))x\(Int(size.height))")
                self.view.activeSize = size
            }
        }
        hevc = decoder

        // Release a finished picture once the stream goes quiet, rather than leaving it to wait
        // for the next one.
        idleFlush?.invalidate()
        idleFlush = Timer.scheduledTimer(withTimeInterval: 0.016, repeats: true) { [weak decoder] _ in
            decoder?.flushPendingIfIdle()
        }
        return decoder
    }

    /// The original route: the engine depacketizes and we read Annex-B over loopback.
    private func startProxiedStream(_ c: ControlClient) {
        let decoder = makeDecoder()

        let s = StreamClient(port: videoPort)
        s.onNAL = { [weak decoder] nal in decoder?.handle(nal: nal) }
        s.onDisconnect = { [weak self] reason in
            self?.scheduleRetry(because: "video stream ended: \(reason)")
        }
        do {
            try s.start()
        } catch {
            scheduleRetry(because: "video port \(videoPort) not reachable")
            return
        }
        stream = s

        retryTimer?.invalidate()
        retryTimer = nil

        // Ask which codec the device actually chose before frames arrive — the engine reads it off
        // the RTP payload type, so it knows rather than guesses, and HEVC and H.264 need different
        // parameter sets and NAL parsing.
        c.streamInfo { [weak self] result in
            guard let self, case .success(let info) = result else { return }
            if let name = info["codec"] as? String, let codec = VideoCodec(rawValue: name) {
                self.hevc?.codec = codec
            }
        }


    }

    private func scheduleRetry(because reason: String) {
        // Whatever went wrong, this connect attempt is over. Leaving the flag set would be worse
        // than the overlap it prevents: the app would never try again.
        connecting = false
        stream?.stop()
        stream = nil
        deviceLabel = reason
        guard retryTimer == nil else { return }
        retryTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.connect()
        }
    }

    /// Size the window to what we actually present — the device screen once known, which is not
    /// the coded video size. Called whenever either of those two facts arrives; they can come in
    /// either order.
    private func applySizing() {
        let size = view.presentedSize
        guard size.width > 0, size.height > 0 else { return }

        // No contentAspectRatio: the window holds the sidebar and inspector too, so locking the
        // whole window to the video's aspect ratio would make it unresizable in practice. Instead
        // size it so the middle pane shows the screen at a sensible scale.
        if let screen = window.screen ?? NSScreen.main {
            let maxH = screen.visibleFrame.height * 0.85
            let scale = min(1.0, maxH / size.height)
            let videoW = (size.width * scale).rounded()
            let videoH = (size.height * scale).rounded()
            window.setContentSize(NSSize(width: 250 + videoW + 260, height: videoH))
            window.center()
        }
    }

    private func updateStatus() {
        guard let decoder = hevc else {
            window.title = "rPlayHub — \(deviceLabel)"
            return
        }
        fps = decoder.framesEnqueued - lastFrameCount
        lastFrameCount = decoder.framesEnqueued
        let size = view.presentedSize

        // Titled like Device Hub — "iPhone13 – iOS 27.0" — with the numbers living in the Stream
        // section instead. Anything WRONG still goes in the title, because a quiet failure that
        // only shows in a hidden pane is how the first black window went undiagnosed.
        var parts = [sidebar.selectedRow().map { "\($0.name) – iOS \($0.version)" } ?? deviceLabel]
        if decoder.awaitingKeyframe, decoder.nalsSeen > 0 {
            parts.append("waiting for keyframe (\(decoder.framesBeforeKeyframe) frames skipped)")
        }
        if let err = decoder.lastError, decoder.decodeFailures > 0 {
            parts.append("decode error ×\(decoder.decodeFailures): \(err)")
        }
        if isRecording { parts.append("● REC") }
        window.title = parts.joined(separator: " — ")

        controls.setStatus(size.width > 0
            ? "\(Int(size.width))×\(Int(size.height))  ·  \(fps) fps"
            : "no video yet")
        var health = ""
        if decoder.awaitingKeyframe && decoder.nalsSeen > 0 {
            health += "waiting for keyframe\n\(decoder.framesBeforeKeyframe) frames skipped\n"
        }
        health += "\(decoder.nalsSeen) NALs\n\(decoder.framesEnqueued) frames decoded"
        let skipped = view.displayLayer.framesSkipped
        if skipped > 0 { health += "\n\(skipped) not shown (display behind)" }
        if let err = decoder.lastError { health += "\ndecode error: \(err)" }

        // RTP health, on the direct path where we own the socket. This is the measurement that
        // separates the two remaining explanations for the artifacts, and nothing else does:
        // if `lost` climbs during a swipe, packets are being lost between the device and this
        // process; if it stays at zero while the picture breaks up, nothing was lost and the
        // encoder simply spent its budget -- and no amount of work on our side will fix that.
        if let direct = directStream {
            let st = direct.stats
            let pct = st.packets > 0 ? Double(st.lost) / Double(st.lost + st.packets) * 100 : 0
            health += String(format: "\n%.1f Mbit/s · %llu keyframes\n%llu lost (%.2f%%)",
                             st.mbps, st.keyframes, st.lost, pct)
            // Arrived but did not add up: a NAL whose declared length disagreed with the bytes
            // present, or one too large to reassemble. Separate from loss on purpose -- these
            // packets were received, so a non-zero count here points at our parsing, not the link.
            if st.bad > 0 { health += "\n\(st.bad) malformed (received, did not add up)" }
            // Arrived too late to use. Counted separately from loss because it IS separate:
            // these packets reached us and were thrown away, so a stream reading "0 lost" can
            // still be missing data. Reading loss alone overstates how intact the stream was.
            if st.late > 0 { health += "\n\(st.late) too late to use" }
            // Log the first sample unconditionally, then on any change, then every 30 s.
            //
            // Logging only on change was wrong: the counter starts at zero, so a session with no
            // loss at all produced no lines -- indistinguishable from the measurement not running.
            // "No evidence of loss" and "evidence of no loss" are different claims and this has to
            // support the second one, since that is the whole reason the counter exists.
            let now = Date().timeIntervalSinceReferenceDate
            if !loggedRTPOnce || st.lost != lastLoggedLoss || now - lastRTPLogAt > 30 {
                loggedRTPOnce = true
                lastRTPLogAt = now
                AppBuild.log(String(format: "rtp: %llu lost of %llu (%.2f%%), %.1f Mbit/s, "
                                            + "%llu keyframes, %llu ltr-acked, %llu malformed, "
                                            + "%llu late, %llu dup, %d not shown, "
                                            + "%d decoded, %d failed, %d discontinuities, "
                                            + "%d dropped after loss, %d presented, %d superseded",
                                    st.lost, st.lost + st.packets, pct, st.mbps,
                                    st.keyframes, st.ltrAcked, st.bad,
                                    st.late, st.dup, skipped,
                                    videoDecoder?.framesDecoded ?? 0,
                                    videoDecoder?.decodeFailures ?? 0,
                                    hevc?.discontinuities ?? 0,
                                    hevc?.framesBeforeKeyframe ?? 0,
                                    view?.displayLayer.framesPresented ?? 0,
                                    view?.displayLayer.framesSkipped ?? 0))
                lastLoggedLoss = st.lost
            }
        }
        controls.setHealth(health)
    }
}


// MARK: - toolbar

extension AppDelegate: NSToolbarDelegate {
    private static let sidebarItem = NSToolbarItem.Identifier("toggleSidebar")
    /// Settings/Report/Info -- Device Hub keeps these in the title bar itself, at the trailing
    /// edge, same row as the traffic lights; not inside the inspector's content area.
    private static let inspectorTabsItem = NSToolbarItem.Identifier("inspectorTabs")

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [Self.sidebarItem, .flexibleSpace, Self.inspectorTabsItem]
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [Self.sidebarItem, .flexibleSpace, Self.inspectorTabsItem]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        if id == Self.sidebarItem {
            let item = NSToolbarItem(itemIdentifier: id)
            item.label = "Sidebar"
            item.toolTip = "Hide Sidebar"
            item.image = NSImage(systemSymbolName: "sidebar.left",
                                 accessibilityDescription: "Hide Sidebar")
            item.target = self
            item.action = #selector(toggleSidebar)
            return item
        }
        if id == Self.inspectorTabsItem {
            let item = NSToolbarItem(itemIdentifier: id)
            item.label = "Inspector"
            item.view = inspector.iconTabs
            return item
        }
        return nil
    }
}
