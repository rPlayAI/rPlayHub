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
    private var controls: ControlPanel!
    private var isPinned = false
    private var stream: StreamClient?
    private var control: ControlClient?
    private var hevc: HEVCStream?
    private var usb: USBMirror?

    private var recordItem: NSMenuItem?
    private var isRecording = false
    private var lastRecordingPath: String?
    private var retryTimer: Timer?
    private var statusTimer: Timer?
    private var deviceLabel = "no device"
    private var lastFrameCount = 0
    private var fps = 0

    private let videoPort: UInt16 = 9877
    private let controlPort: UInt16 = 9876

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenu()
        buildWindow()
        connect()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            self?.updateStatus()
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
        controls = ControlPanel(frame: NSRect(x: 0, y: 0, width: 260, height: rect.height))

        sidebar.onCommand = { [weak self] command, device in
            self?.perform(command, on: device)
        }
        view.onCommand = { [weak self] command in self?.perform(command, on: nil) }
        controls.onAction = { [weak self] action in
            switch action {
            case .pin:        self?.perform(.pin, on: nil)
            case .home:       self?.perform(.home, on: nil)
            case .screenshot: self?.perform(.screenshot, on: nil)
            case .record:     self?.perform(.record, on: nil)
            }
        }

        let split = NSSplitView(frame: NSRect(x: 0, y: 0, width: 250 + rect.width + 260,
                                              height: rect.height))
        split.isVertical = true
        split.dividerStyle = .thin
        split.autoresizingMask = [.width, .height]
        split.addArrangedSubview(sidebar)
        split.addArrangedSubview(view)
        split.addArrangedSubview(controls)
        // Embedded: the screen sits next to the device list, so the controls pane is hidden and
        // right-clicking the screen is how you reach them. View > Show Controls brings it back.
        controls.isHidden = true
        split.setHoldingPriority(NSLayoutConstraint.Priority(260), forSubviewAt: 0)
        split.setHoldingPriority(NSLayoutConstraint.Priority(240), forSubviewAt: 1)
        split.setHoldingPriority(NSLayoutConstraint.Priority(260), forSubviewAt: 2)

        // Without explicit widths the split view squeezes the side panes and the rows clip.
        // Equal-constant at a lower priority sets the resting width; the >= keeps them usable.
        for (pane, width) in [(sidebar as NSView, 250.0), (controls as NSView, 260.0)] {
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

    private func buildMenu() {
        let mainMenu = NSMenu()

        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About rPlayHub", action: nil, keyEquivalent: "")
        appMenu.addItem(.separator())
        recordItem = appMenu.addItem(withTitle: "Start Recording",
                                     action: #selector(toggleRecording), keyEquivalent: "r")
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

        let viewItem = NSMenuItem()
        mainMenu.addItem(viewItem)
        let viewMenu = NSMenu(title: "View")
        viewMenu.addItem(withTitle: "Show Controls", action: #selector(toggleControls),
                         keyEquivalent: "i")
        viewItem.submenu = viewMenu

        let windowItem = NSMenuItem()
        mainMenu.addItem(windowItem)
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Minimize",
                           action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)),
                           keyEquivalent: "")
        windowItem.submenu = windowMenu

        NSApp.mainMenu = mainMenu
        NSApp.windowsMenu = windowMenu
    }

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
                    self.controls.setRecording(false)
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
                    self.controls.setRecording(true)
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
        controls.isHidden.toggle()
        sender.title = controls.isHidden ? "Show Controls" : "Hide Controls"
    }

    private func perform(_ command: DeviceSidebar.Command, on device: DeviceRow?) {
        switch command {
        case .pin:
            isPinned.toggle()
            window.level = isPinned ? .floating : .normal
            controls.setPinned(isPinned)

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
        stream?.stop()
        control?.close()
        stream = nil
        control = nil
        hevc = nil
        connect()
    }

    /// Try the USB capture path. Returns false if no cabled iPhone is available, in which case
    /// the caller falls back to the CoreDevice video stream.
    ///
    /// Control still runs over CoreDevice either way: touch goes through universalhidservice, and
    /// nothing about the picture changes that.
    private func startUSBMirror() -> Bool {
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
            NSLog("rPlayHub: USB capture unavailable — \(mirror.status); using the CoreDevice stream")
            return false
        }
        usb = mirror
        retryTimer?.invalidate()
        retryTimer = nil
        return true
    }

    private func connect() {
        let c = ControlClient(port: controlPort)
        do {
            try c.connect()
        } catch {
            scheduleRetry(because: "engine not reachable on port \(controlPort)")
            return
        }
        control = c
        view.control = c

        // Prefer the USB capture path when a cable is present. Both paths end at the same layer,
        // so this is purely about which produces the picture.
        //
        // The CoreDevice stream is capped by the device at 1184x2544 / 6 Mbps, which is about
        // 0.03 bits per pixel and visibly falls apart the moment anything moves. Apple's own
        // Device Hub negotiates the identical numbers and shows the identical artefacting, so it
        // is a property of that transport rather than something to fix. The capture plug-in is
        // built for screen recording and carries no such budget.
        if startUSBMirror() { return }

        // Decode is explicit and unconditional; the layer only ever shows the newest picture.
        let vt = VideoDecoder()
        vt.onFrame = { [weak self] picture in self?.view.displayLayer.present(picture) }
        let decoder = HEVCStream(decoder: vt)
        decoder.onFormat = { [weak self] size in
            guard let self else { return }
            self.view.videoSize = size          // coded size, padding included
            self.applySizing()
        }
        hevc = decoder

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

        c.listDevices { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let devices):
                self.sidebar.update(devices.map(DeviceRow.init(json:)))
                if let d = devices.first {
                    let model = d["product_type"] as? String ?? "iPhone"
                    let os = d["os_version"] as? String ?? "?"
                    self.deviceLabel = "\(model) · iOS \(os)"
                    // The real screen size, which is smaller than the coded frame: the encoder
                    // pads up to 16-pixel alignment. MirrorView crops and maps clicks with it.
                    if let s = d["screen_size"] as? [String: Any],
                       let w = (s["w"] as? NSNumber)?.doubleValue,
                       let h = (s["h"] as? NSNumber)?.doubleValue, w > 0, h > 0 {
                        self.view.deviceSize = CGSize(width: w, height: h)
                        self.applySizing()
                    }
                } else {
                    self.deviceLabel = "engine has no device"
                }
            case .failure(let e):
                self.deviceLabel = "list_devices failed: \(e)"
            }
        }
    }

    private func scheduleRetry(because reason: String) {
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
        controls.setHealth(health)
    }
}


// MARK: - toolbar

extension AppDelegate: NSToolbarDelegate {
    private static let sidebarItem = NSToolbarItem.Identifier("toggleSidebar")

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [Self.sidebarItem, .flexibleSpace]
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [Self.sidebarItem, .flexibleSpace]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard id == Self.sidebarItem else { return nil }
        let item = NSToolbarItem(itemIdentifier: id)
        item.label = "Sidebar"
        item.toolTip = "Hide Sidebar"
        item.image = NSImage(systemSymbolName: "sidebar.left",
                             accessibilityDescription: "Hide Sidebar")
        item.target = self
        item.action = #selector(toggleSidebar)
        return item
    }
}
