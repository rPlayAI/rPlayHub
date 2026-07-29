//
//  MirrorView.swift
//  The View Screen: shows the phone, turns clicks and drags into taps and swipes.
//
//  The coded video is NOT the screen. displayservice encodes at 16-pixel alignment with no
//  conformance-window crop, so an iPhone 13 Pro's 1170×2532 screen arrives as a 1184×2576 frame
//  with the screen anchored top-left and black padding on the right and bottom. Verified against
//  a real recording: the padding begins exactly at column 1170 and row 2532.
//
//  So this view crops. Showing the raw frame would display black edges, and — worse — mapping
//  clicks against the full frame would drift every tap toward the top-left by up to 1.7%, which
//  on a 2532px screen is ~44px at the bottom: enough to miss a button.
//
//  Layer arrangement, since videoGravity alone cannot express this crop:
//      backing layer (clips)
//        └─ clipLayer      == exactly the on-screen rectangle of the device screen
//             └─ displayLayer == the whole coded frame, scaled so its screen region fills clipLayer
//

import AppKit
import AVFoundation

final class MirrorView: NSView {
    /// Coded video size, from the stream's parameter sets. Includes the alignment padding.
    var videoSize: CGSize = .zero {
        didSet { needsLayout = true }
    }

    /// True device screen size, from the engine's `list_devices`. Zero until known.
    var deviceSize: CGSize = .zero {
        didSet { needsLayout = true }
    }

    var control: ControlClient?

    /// Right-click commands on the screen itself. When the screen is embedded in the window that
    /// already lists devices, this is where the controls live — a second controls pane beside the
    /// device list would be redundant, which is why Device Hub does it this way too.
    var onCommand: ((DeviceSidebar.Command) -> Void)?

    let displayLayer = VideoLayer()
    private let clipLayer = CALayer()

    private var dragStart: CGPoint?          // normalized 0..1
    private var dragStartedAt: TimeInterval = 0
    /// Below this movement (in normalized units) a drag is really a tap.
    private let swipeThreshold: CGFloat = 0.02

    /// Flipped on purpose, for two reasons. AppKit flips the backing layer's geometry for a
    /// NON-flipped view, which flips manually-added sublayers' *content* too — that rendered the
    /// video upside down. And a top-left origin matches the device's own, so click mapping and
    /// the crop anchoring below need no y inversion.
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setUpLayers()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setUpLayers()
    }

    private func buildContextMenu() {
        let menu = NSMenu()
        func add(_ title: String, _ command: DeviceSidebar.Command) {
            let item = NSMenuItem(title: title, action: #selector(contextAction(_:)),
                                  keyEquivalent: "")
            item.target = self
            item.representedObject = command.rawValue
            menu.addItem(item)
        }
        add("Take Screenshot", .screenshot)
        add("Start / Stop Recording", .record)
        add("Press Home", .home)
        menu.addItem(.separator())
        add("Pin Window on Top", .pin)
        menu.addItem(.separator())
        add("Reconnect", .reconnect)
        self.menu = menu
    }

    @objc private func contextAction(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String,
              let command = DeviceSidebar.Command(rawValue: raw) else { return }
        onCommand?(command)
    }

    private func setUpLayers() {
        buildContextMenu()
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.masksToBounds = true

        clipLayer.masksToBounds = true
        clipLayer.backgroundColor = NSColor.black.cgColor

        // .resize, not .resizeAspect: the layer frame below is computed to the video's exact
        // aspect ratio already, so gravity-driven letterboxing would fight the crop maths.
        displayLayer.videoGravity = .resize   // the frame below is already exact
        displayLayer.backgroundColor = NSColor.black.cgColor

        // Filtering is left at CoreAnimation's default on purpose.
        //
        // .trilinear looked like the right answer for a 3-5x downscale — it samples mipmaps rather
        // than undersampling, which is what makes edges break up and text shimmer during motion.
        // But the contents here are an IOSurface, and CoreAnimation cannot build mipmaps for one,
        // so setting it produced a BLACK WINDOW rather than a smoother picture. Reverted after
        // measuring, not after reasoning.
        //
        // Improving the downscale therefore needs the resampling done somewhere that can do it:
        // a Metal layer, or scaling in the decoder's pixel transfer. Not a one-line layer property.
        // No timebase and no scheduling. The layer simply shows the newest decoded picture; the
        // decoder, not the layer, decides what gets decoded, and it decodes everything.
        clipLayer.addSublayer(displayLayer)
        layer?.addSublayer(clipLayer)
    }

    // MARK: - geometry

    /// The fraction of the coded frame that is actually screen, anchored top-left.
    private var visibleFraction: CGSize {
        guard videoSize.width > 0, videoSize.height > 0,
              deviceSize.width > 0, deviceSize.height > 0 else {
            return CGSize(width: 1, height: 1)      // no device size yet: show the whole frame
        }
        return CGSize(width: min(1, deviceSize.width / videoSize.width),
                      height: min(1, deviceSize.height / videoSize.height))
    }

    /// What we present — the device screen when known, otherwise the raw coded frame.
    var presentedSize: CGSize {
        if deviceSize.width > 0, deviceSize.height > 0 { return deviceSize }
        return videoSize
    }

    /// Where the device screen sits inside the view, aspect-fit. Clicks map against THIS, never
    /// against `bounds` — that is the classic off-by-a-letterbox bug.
    private func screenRect() -> CGRect {
        let content = presentedSize
        guard content.width > 0, content.height > 0 else { return bounds }
        let scale = min(bounds.width / content.width, bounds.height / content.height)
        let w = content.width * scale
        let h = content.height * scale
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    /// View point → device fractions, or nil if the click landed outside the screen area.
    private func normalized(_ p: CGPoint) -> CGPoint? {
        let r = screenRect()
        guard r.width > 0, r.height > 0, r.contains(p) else { return nil }
        // The view is flipped, so its origin is top-left like the device's — no inversion needed.
        let fx = (p.x - r.minX) / r.width
        let fy = (p.y - r.minY) / r.height
        return CGPoint(x: min(max(fx, 0), 1), y: min(max(fy, 0), 1))
    }

    override func layout() {
        super.layout()
        let screen = screenRect()
        clipLayer.frame = screen

        let fraction = visibleFraction
        guard fraction.width > 0, fraction.height > 0 else {
            displayLayer.frame = clipLayer.bounds
            return
        }
        // Scale the whole coded frame so its top-left screen region exactly fills clipLayer,
        // pushing the alignment padding off the bottom-right where the clip discards it.
        let full = CGSize(width: screen.width / fraction.width,
                          height: screen.height / fraction.height)
        displayLayer.frame = CGRect(x: 0, y: 0, width: full.width, height: full.height)
    }

    // MARK: - input

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        dragStart = normalized(p)
        dragStartedAt = event.timestamp
    }

    override func mouseUp(with event: NSEvent) {
        defer { dragStart = nil }
        guard let start = dragStart,
              let end = normalized(convert(event.locationInWindow, from: nil)),
              let control else { return }

        let dx = end.x - start.x
        let dy = end.y - start.y
        let distance = (dx * dx + dy * dy).squareRoot()
        let elapsedMS = max(60, Int((event.timestamp - dragStartedAt) * 1000))

        if distance < swipeThreshold {
            control.tap(fx: Double(start.x), fy: Double(start.y))
        } else {
            control.swipe(fx0: Double(start.x), fy0: Double(start.y),
                          fx1: Double(end.x), fy1: Double(end.y),
                          durationMS: min(elapsedMS, 2000))
        }
    }
}
