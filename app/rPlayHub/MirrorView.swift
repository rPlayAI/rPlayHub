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

    /// The coded size of the frame currently on screen, published by VideoLayer as it enqueues
    /// that exact picture. Zero means "not known yet, treat the frame as full size".
    var activeSize: CGSize = .zero {
        didSet { if activeSize != oldValue { needsLayout = true } }
    }

    /// The device whose body we draw around the picture. Setting it redraws the bezel.
    var productType: String? {
        didSet { if productType != oldValue { needsLayout = true } }
    }

    /// Covers the notch or island. The device streams the pixels behind them, so without this the
    /// status bar sits in a black band that reads as a video artifact rather than as a phone.
    private let cutoutLayer = CAShapeLayer()

    /// A screenshot shown in place of video while none is flowing -- what Device Hub's device
    /// pane shows before View Screen, and all it can show for a device that cannot mirror
    /// (iOS < 27). Sits above the video layer and is hidden the moment a frame arrives.
    private let stillLayer = CALayer()

    /// Show `image` where the video would be. Sets the device size from the pixel size when
    /// nothing else has, so the pane takes the phone's shape instead of a 0x0 guess.
    func showStill(_ image: CGImage) {
        if deviceSize == .zero {
            deviceSize = CGSize(width: image.width, height: image.height)
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        stillLayer.contents = image
        stillLayer.isHidden = false
        CATransaction.commit()
    }

    func hideStill() {
        guard !stillLayer.isHidden else { return }
        stillLayer.isHidden = true
        stillLayer.contents = nil
    }

    var isShowingStill: Bool { !stillLayer.isHidden }

    /// Device Hub's device pane, before View Screen: a small fixed-size phone mockup with the
    /// device name + OS + button stacked underneath it, both centered as one block -- not a
    /// full-bleed picture with the text overlaid on top. AppKit views (not layers, like the rest
    /// of this class) since they need text layout and a click target; positioned by hand in
    /// `layout()` (not Auto Layout) because where they sit depends on `screenRect()`, which is
    /// itself only known there.
    private let viewScreenName = NSTextField(labelWithString: "")
    private let viewScreenOS = NSTextField(labelWithString: "")
    private let viewScreenButton = NSButton()
    /// What showViewScreenPrompt/hideViewScreenPrompt actually show and hide.
    private var viewScreenStack: NSStackView!

    /// True while the picture is the small pre-connect mockup rather than the full-bleed
    /// screen/video -- `screenRect()` and `layout()` both key off this.
    private var isGated = false { didSet { needsLayout = true } }

    /// Called when the View Screen button is clicked. AppDelegate starts the video pipeline.
    var onViewScreen: (() -> Void)?

    func showViewScreenPrompt(name: String, os: String) {
        viewScreenName.stringValue = name
        viewScreenOS.stringValue = os
        viewScreenOS.isHidden = os.isEmpty
        viewScreenStack.isHidden = false
        isGated = true
    }

    func hideViewScreenPrompt() {
        viewScreenStack.isHidden = true
        isGated = false
    }

    private func buildViewScreenPrompt() {
        viewScreenName.font = .systemFont(ofSize: 13, weight: .semibold)
        viewScreenName.alignment = .center
        viewScreenName.textColor = .labelColor
        viewScreenOS.font = .systemFont(ofSize: 11)
        viewScreenOS.textColor = .secondaryLabelColor
        viewScreenOS.alignment = .center
        viewScreenButton.title = "  View Screen"
        viewScreenButton.image = NSImage(systemSymbolName: "rectangle.on.rectangle",
                                         accessibilityDescription: "View Screen")
        viewScreenButton.imagePosition = .imageLeading
        viewScreenButton.bezelStyle = .rounded
        viewScreenButton.target = self
        viewScreenButton.action = #selector(viewScreenClicked)

        let stack = NSStackView(views: [viewScreenName, viewScreenOS, viewScreenButton])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 6
        // Frame-based, like the phone mockup it sits under -- `layout()` places both by hand.
        stack.translatesAutoresizingMaskIntoConstraints = true
        stack.isHidden = true
        addSubview(stack)
        viewScreenStack = stack
    }

    @objc private func viewScreenClicked() { onViewScreen?() }

    /// Quarter-turns applied to the picture, 0-3, clockwise.
    ///
    /// The device is not rotated by this -- iOS orientation follows the phone's own sensors and
    /// there is no remote verb for it. What this does is turn the view, which is the useful half:
    /// a phone lying in a dock, or an app that has locked itself to landscape, can be watched the
    /// right way up. Input is rotated with it, so a tap still lands where it looks like it will.
    private(set) var rotation = 0

    func rotate() {
        rotation = (rotation + 1) % 4
        needsLayout = true
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
        add("Sleep (Lock)…", .sleep)
        add("Restart…", .restart)
        add("Shut Down…", .shutdown)
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
        // Plain white, sampled directly off the live Device Hub pane -- true in both states, not
        // just the letterboxing margin around a fitted picture.
        layer?.backgroundColor = NSColor.white.cgColor
        layer?.masksToBounds = true

        clipLayer.masksToBounds = true
        // Device Hub's own placeholder, before any real screenshot exists: a plain blue
        // rectangle inside the phone bezel, not a wallpaper mockup. Only visible while neither
        // stillLayer nor displayLayer has content -- both fully cover this the moment either does.
        clipLayer.backgroundColor = NSColor.systemBlue.cgColor
        // The bezel outline -- without it the phone reads as a plain rounded rectangle rather
        // than a device. Width is set proportionally in layout(), since it must scale with the
        // mockup's own (much smaller, fixed) size before View Screen.
        clipLayer.borderColor = NSColor.black.withAlphaComponent(0.85).cgColor
        // Above the video, inside the clip, so it rounds off with the screen corners.
        cutoutLayer.fillColor = NSColor.black.cgColor
        cutoutLayer.zPosition = 10

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
        stillLayer.contentsGravity = .resize       // the screenshot IS the screen, edge to edge
        stillLayer.isHidden = true
        clipLayer.addSublayer(stillLayer)
        clipLayer.addSublayer(cutoutLayer)
        layer?.addSublayer(clipLayer)

        // Built last so its (implicit) layer lands above clipLayer's sublayers in the view's
        // layer tree -- built earlier, it rendered UNDER the still/video and was invisible.
        buildViewScreenPrompt()
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
        var content = presentedSize
        if content.width <= 0 || content.height <= 0 {
            guard isGated else { return bounds }
            // The real aspect isn't known yet -- deviceSize arrives asynchronously and can lag
            // behind the gated prompt showing. Falling back to `bounds` here (as the non-gated
            // path does) put the mockup at the CANVAS's own aspect ratio, which happens to look
            // roughly phone-shaped and so read as "correct but huge" rather than obviously wrong.
            // A generic phone ratio keeps the mockup small and phone-shaped either way; it is
            // replaced by the real one within a layout pass or two once deviceSize is known.
            content = CGSize(width: 9, height: 19.5)
        }
        if rotation % 2 == 1 { content = CGSize(width: content.height, height: content.width) }
        if isGated {
            // Device Hub's pre-connect mockup is a small fixed-size phone (measured off the live
            // app: ~94pt wide in its ~390pt canvas, a ratio of 0.24), not the picture scaled to
            // fill the pane -- that fill only happens once there is an actual screen to show. The
            // block (phone + gap + name/OS/button) is centered as a whole, matching the roughly
            // equal top/bottom margins Device Hub leaves around it.
            //
            // Ratio, not a flat constant: an earlier version used bounds.width * 0.12, which is
            // 94/923 -- Device Hub's WINDOW width, not this view's own (canvas-only) bounds. That
            // silently halved the mockup every time, since `bounds` here has always been the
            // canvas alone.
            let targetWidth = min(120, max(60, bounds.width * 0.24))
            let scale = targetWidth / content.width
            let w = content.width * scale
            let h = content.height * scale
            let gap: CGFloat = 16
            let stackHeight = viewScreenStack?.fittingSize.height ?? 0
            let blockHeight = h + gap + stackHeight
            let top = max(20, (bounds.height - blockHeight) / 2)
            return CGRect(x: (bounds.width - w) / 2, y: top, width: w, height: h)
        }
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
        var fx = (p.x - r.minX) / r.width
        var fy = (p.y - r.minY) / r.height
        // Undo the view rotation so the fraction is in the device's own frame. Without this a tap
        // lands wherever the unrotated picture had that point, which is the worst kind of broken:
        // it looks like it worked and hits something else.
        switch rotation {
        case 1: (fx, fy) = (fy, 1 - fx)
        case 2: (fx, fy) = (1 - fx, 1 - fy)
        case 3: (fx, fy) = (1 - fy, fx)
        default: break
        }
        return CGPoint(x: min(max(fx, 0), 1), y: min(max(fy, 0), 1))
    }

    override func layout() {
        super.layout()
        let screen = screenRect()
        clipLayer.frame = screen
        stillLayer.frame = clipLayer.bounds
        // displayLayer sits above clipLayer's blue placeholder fill and is opaque black by
        // default (see setUpLayers) -- with no video content that painted over the placeholder
        // solid black instead of letting the blue mockup show through, even once it was sized
        // down to the small pre-connect mockup rather than filling the whole pane.
        displayLayer.isHidden = isGated
        // Turn the clip, not the display layer: the crop maths below is expressed in the
        // device's own frame, and rotating underneath it would mean redoing all of it per angle.
        clipLayer.transform = CATransform3DMakeRotation(CGFloat(rotation) * .pi / 2, 0, 0, 1)
        // Everything below is laid out INSIDE the clip, whose own coordinate space is always the
        // device's upright shape -- the rotation is applied to the clip as a whole. screenRect()
        // returns the on-screen footprint, which is turned on its side at a quarter turn, so
        // using it for the contents made the picture a fraction of its proper size and mostly
        // black. clipSize is the space the contents actually live in.
        let clipSize = rotation % 2 == 1
            ? CGSize(width: screen.height, height: screen.width)
            : CGSize(width: screen.width, height: screen.height)
        if rotation % 2 == 1 {
            clipLayer.bounds = CGRect(origin: .zero, size: clipSize)
            clipLayer.position = CGPoint(x: screen.midX, y: screen.midY)
        }

        // Round the screen corners and mask the cutout, so the mirror reads as a phone rather
        // than as a rectangle of video. Both are pure presentation -- the picture underneath is
        // untouched, and taps still map to the full screen including behind the cutout.
        clipLayer.cornerRadius = clipSize.width * DeviceModel.cornerFraction(for: productType)
        clipLayer.borderWidth = max(1, clipSize.width * 0.03)
        cutoutLayer.frame = clipLayer.bounds
        if let c = DeviceModel.cutoutRect(for: productType), clipSize.width > 0 {
            let w = clipSize.width * c.w
            let h = clipSize.height * c.h
            let top = clipSize.height * c.top
            let rect = CGRect(x: (clipSize.width - w) / 2, y: top, width: w, height: h)
            // A notch hangs off the top edge, so only its bottom corners are round; an island
            // floats free and is a capsule.
            let path: CGPath
            if c.top > 0 {
                path = CGPath(roundedRect: rect, cornerWidth: h / 2, cornerHeight: h / 2,
                              transform: nil)
            } else {
                // A notch hangs off the top edge, so it is drawn taller than it shows and the
                // clip takes the overhang -- that is what leaves only its bottom corners round.
                // insetBy(dy:) grows BOTH edges, which made the visible part 2h and the notch
                // look far too deep; extend upward only.
                let overhung = CGRect(x: rect.minX, y: -h, width: w, height: h * 2)
                path = CGPath(roundedRect: overhung, cornerWidth: h * 0.55,
                              cornerHeight: h * 0.55, transform: nil)
            }
            cutoutLayer.path = path
            cutoutLayer.isHidden = false
        } else {
            cutoutLayer.isHidden = true
        }

        // Positioned here, not after the video-geometry guards below: those return early
        // whenever there's no video size yet (videoSize is genuinely (0,0) for the whole time
        // before View Screen is first clicked), which used to skip this every single time while
        // gated and leave the name/OS/button stuck at a stale or default frame.
        if isGated {
            let fit = viewScreenStack.fittingSize
            viewScreenStack.frame = CGRect(
                x: (bounds.width - fit.width) / 2, y: screen.maxY + 16,
                width: fit.width, height: fit.height)
        }

        let fraction = visibleFraction
        guard fraction.width > 0, fraction.height > 0 else {
            displayLayer.frame = clipLayer.bounds
            return
        }
        // Scale the whole coded frame so its live top-left region exactly fills clipLayer,
        // pushing everything else off the bottom-right where the clip discards it.
        //
        // Which region is live depends on the tier, and the two cases genuinely differ:
        //
        // The padding fraction is the same at every tier: the encoder scales the screen and its
        // alignment padding together, so the active rect always contains both. Measured on a real
        // capture, screen/active came out 0.9882x0.9829 at 1184x2576, 0.9890x0.9833 at 1088x1920
        // and 0.9889x0.9836 at 720x1280 -- one ratio, not two cases.
        //
        // An earlier version treated reduced tiers as pure screen with no padding. That put the
        // padding back on screen as black edges and shifted the scale slightly on every
        // downshift, which under a swipe reads as the picture twitching.
        let coded = CGSize(width: videoSize.width, height: videoSize.height)
        let active = (activeSize.width > 0 && activeSize.height > 0) ? activeSize : coded
        let live = CGSize(width: active.width * fraction.width,
                          height: active.height * fraction.height)
        guard live.width > 0, live.height > 0 else {
            displayLayer.frame = clipLayer.bounds
            return
        }
        let full = CGSize(width: clipSize.width * coded.width / live.width,
                          height: clipSize.height * coded.height / live.height)
        // No implicit animation. The frame changes whenever the encoder flaps between tiers, and
        // letting Core Animation interpolate turns each switch into a visible zoom -- the picture
        // appears to breathe rather than simply being drawn at the right size.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        displayLayer.frame = CGRect(x: 0, y: 0, width: full.width, height: full.height)
        CATransaction.commit()
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
