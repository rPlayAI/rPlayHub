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

    /// Device Hub's empty-screen fill: a vertical gradient, not a flat colour. Sampled straight
    /// off its live window -- (60,143,207) at the top easing to (89,172,237) at the bottom, so it
    /// reads as a screen catching light rather than as a blue rectangle. A flat `systemBlue`
    /// (0,122,255) was far more saturated and was the most visible difference left in the mockup.
    private let placeholderLayer = CAGradientLayer()

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
    private let viewScreenButton = HoverButton()
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
        viewScreenButton.isHidden = false
        isGated = true
    }

    func hideViewScreenPrompt() {
        viewScreenStack.isHidden = true
        viewScreenButton.isHidden = true
        isGated = false
    }

    private func buildViewScreenPrompt() {
        // 15/12, sized against Device Hub's own block rather than guessed: its device name has an
        // ~11px cap height, which is a 15pt bold face, over an ~12pt OS line. 13/11 read notably
        // smaller side by side.
        viewScreenName.font = .systemFont(ofSize: 15, weight: .bold)
        viewScreenName.alignment = .center
        viewScreenName.textColor = .labelColor
        viewScreenOS.font = .systemFont(ofSize: 12)
        viewScreenOS.textColor = .secondaryLabelColor
        viewScreenOS.alignment = .center
        viewScreenButton.title = "  View Screen"
        viewScreenButton.translatesAutoresizingMaskIntoConstraints = true
        // Matches Device Hub's own icon: the stock "screen sharing" symbol (an inset-filled
        // rectangle with a filled person at its bottom-right), identified by cropping the button
        // out of a Device Hub window capture and comparing against rendered SF Symbol candidates
        // side by side. An earlier pass picked rectangle.stack.badge.person.crop and recorded it
        // as confirmed; the user spotted the mismatch -- the real icon has ONE filled rectangle
        // and no circled badge. (Device Hub bundles no icon assets of its own -- confirmed via
        // assetutil against its Assets.car -- so every icon in it is a stock symbol.)
        viewScreenButton.image = NSImage(systemSymbolName: "rectangle.inset.filled.and.person.filled",
                                         accessibilityDescription: "View Screen")
        viewScreenButton.imagePosition = .imageLeading
        // Centre the image+title as one group. The button has an explicit 127pt frame (wider than
        // its fitting size, to match Device Hub), and without this the cell pins the group to the
        // leading edge -- which put the icon inside the capsule's rounded end rather than clear
        // of it. The two leading spaces in the title are the gap between icon and text.
        viewScreenButton.alignment = .center
        // Colour, shape and hover behaviour all live in HoverButton (below) -- it draws its own
        // capsule, because a bezelled button would not take the colour.
        viewScreenButton.title = "  View Screen"
        viewScreenButton.translatesAutoresizingMaskIntoConstraints = true
        viewScreenButton.target = self
        viewScreenButton.action = #selector(viewScreenClicked)

        // The button is NOT in the stack. It has to be exactly 127x29 (Device Hub's own size) and
        // an NSStackView sizes arranged subviews to their fitting size -- it dropped a width
        // constraint, ignored an intrinsicContentSize override, and moved by about a point per
        // padding space. Positioned by hand in layout() instead, like the phone mockup above it,
        // where the frame is simply set and nothing overrides it.
        addSubview(viewScreenButton)
        let stack = NSStackView(views: [viewScreenName, viewScreenOS])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 6
        // Frame-based, like the phone mockup it sits under -- `layout()` places both by hand.
        stack.translatesAutoresizingMaskIntoConstraints = true
        stack.isHidden = true
        viewScreenButton.isHidden = true
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
        // Device Hub's own placeholder, before any real screenshot exists: a blue-gradient
        // screen inside the phone bezel, not a wallpaper mockup. Only visible while neither
        // stillLayer nor displayLayer has content -- both fully cover this the moment either does.
        placeholderLayer.colors = [
            NSColor(srgbRed: 60 / 255.0, green: 143 / 255.0, blue: 207 / 255.0, alpha: 1).cgColor,
            NSColor(srgbRed: 89 / 255.0, green: 172 / 255.0, blue: 237 / 255.0, alpha: 1).cgColor,
        ]
        placeholderLayer.startPoint = CGPoint(x: 0.5, y: 0)
        placeholderLayer.endPoint = CGPoint(x: 0.5, y: 1)
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
        // First, so it sits UNDER the video and the still: both cover it completely the moment
        // either has content, which is exactly the "only while empty" behaviour wanted.
        clipLayer.addSublayer(placeholderLayer)
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
            // 96pt, measured off Device Hub's own mockup — a FIXED size, not a fraction of the
            // canvas. A ratio (0.247 = 96/389, its canvas width) only matches while our canvas is
            // exactly as wide as its, and ours is routinely narrower: the window restores a saved
            // frame that overrides the 389pt resting width, so the ratio kept rendering the phone
            // ~5pt short. Shrinks only if the canvas gets too narrow to hold it.
            let targetWidth = min(bounds.width * 0.6, 95)
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

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard window != nil else { return }
        // "Open in New Window" reparents this view (ScreenWindow.swift), and moving a view to a
        // different window rebinds its layers to that window's presentation context.
        // AVSampleBufferDisplayLayer keeps ACCEPTING frames across that move -- enqueue succeeds,
        // framesPresented keeps counting, so refreshStillIfIdle sees a healthy stream -- but its
        // internal renderer stays tied to the old context and silently stops painting: the
        // detached window froze on the last frame. Flushing on the move resets the renderer so
        // the next enqueued frame actually draws. The dropped image costs one display tick of
        // black during live video; while gated the layer is hidden anyway.
        displayLayer.flushAndRemoveImage()
        AppBuild.log("MirrorView moved to window '\(window?.title ?? "?")'; display layer flushed")
    }

    override func layout() {
        super.layout()
        let screen = screenRect()
        clipLayer.frame = screen
        stillLayer.frame = clipLayer.bounds
        // No implicit animation: this resizes with the pane, and letting Core Animation
        // interpolate the gradient makes the placeholder visibly lag the bezel around it.
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        placeholderLayer.frame = clipLayer.bounds
        CATransaction.commit()
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
        // While gated with no device known, stand in an iPhone 13 Pro so the mockup reads as a
        // phone. `cutout(for: nil)` is `.none`, which zeroes the corner radius and drops the
        // notch, and that drew a bare sharp-cornered rectangle where Device Hub always shows a
        // proper device. Applied ONLY to the placeholder: `.none` is genuinely right for a real
        // SE, and using it as a fallback for live video would round corners off a device that
        // has none and mask pixels it actually displays.
        let shape = productType ?? (isGated ? "iPhone14,2" : nil)
        clipLayer.cornerRadius = clipSize.width * DeviceModel.cornerFraction(for: shape)
        // Applied in both states -- a real device has a visible bezel edge around its screen
        // too, and side-by-side against Device Hub's own live view this reads closer to it than
        // no border did.
        // 5.26% of the body's width: Device Hub's mockup measures 95pt across with an 85pt blue
        // screen inside it, so exactly 5pt of bezel each side (5/95). Measured off its window --
        // an earlier 0.03 gave half this and read as a thin outline rather than a device edge.
        clipLayer.borderWidth = max(1, clipSize.width * 0.0526)
        cutoutLayer.frame = clipLayer.bounds
        if let c = DeviceModel.cutoutRect(for: shape), clipSize.width > 0 {
            let w = clipSize.width * c.w
            // While gated, the notch is drawn to Device Hub's own depth rather than Apple's
            // framebuffer mask: 0.125 of the body's width, measured off its mockup (12px of dark
            // at the centre column on a 96px body), against 0.090 here before.
            //
            // The two numbers measure different things and both are right in their place. The
            // mask (101/2532 of screen height) is the hole in the FRAMEBUFFER, which is what has
            // to be cut out of live video. Device Hub's mockup is a picture of the physical
            // device, whose notch is deeper -- and the bezel border, which draws over the top of
            // the clip, was swallowing most of what little depth we had.
            let h = isGated ? clipSize.width * 0.125 : clipSize.height * c.h
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
            // 127x29 exactly, centred under the name/OS block.
            let size = CGSize(width: 127, height: 29)
            viewScreenButton.frame = CGRect(x: (bounds.width - size.width) / 2,
                                            y: viewScreenStack.frame.maxY + 10,
                                            width: size.width, height: size.height)
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

    override func becomeFirstResponder() -> Bool {
        viewScreenButton.isFocusedPanel = true
        return super.becomeFirstResponder()
    }

    override func resignFirstResponder() -> Bool {
        viewScreenButton.isFocusedPanel = false
        return super.resignFirstResponder()
    }

    override func mouseDown(with event: NSEvent) {
        // Clicking a view does not make it first responder on its own, and this panel's focus is
        // what lights the View Screen button (see HoverButton).
        window?.makeFirstResponder(self)
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

/// Device Hub's View Screen button: a #DCDCDC grey capsule that turns solid #6155F5 indigo with
/// white text and icon.
///
/// It lights when the MIDDLE PANEL is the focused one and its window is key, and goes grey when
/// either the sidebar or the inspector is clicked, or the app goes to the background. Nothing to
/// do with the pointer.
///
/// Recorded because it cost several wrong guesses -- hovering the button, pointer anywhere in the
/// middle panel, then key-window state alone -- each "confirmed" by a measurement that happened
/// to agree with it. Two things kept poisoning those checks: activating Device Hub with System
/// Events `set frontmost` activates the app WITHOUT its window becoming key, and its window moves
/// between runs, so a fixed sample coordinate silently reads background instead of the button.
/// The behaviour above came from the user watching the real app, not from those captures.
///
/// #6155F5 is Device Hub's own tint, not the system accent: this machine's AppleAccentColor is
/// unset, so `controlAccentColor` is the default blue and would be visibly the wrong colour.
///
/// Drawn rather than bezelled. `bezelColor` is documented as tinting a bezelled button but does
/// not take on a `.rounded` button here -- measured: it kept rendering AppKit's default
/// (228,228,228) face whatever it was set to. A borderless button with its own layer background
/// is the only way to get both the exact colour and the capsule shape.
final class HoverButton: NSButton {
    /// Set by the owning panel as it gains and loses first-responder status.
    var isFocusedPanel = false { didSet { if isFocusedPanel != oldValue { apply() } } }
    private var observers: [NSObjectProtocol] = []

    static let resting = NSColor(srgbRed: 0xDC / 255, green: 0xDC / 255, blue: 0xDC / 255, alpha: 1)
    static let active = NSColor(srgbRed: 97 / 255.0, green: 85 / 255.0, blue: 245 / 255.0, alpha: 1)

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        isBordered = false
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        isBordered = false
        wantsLayer = true
    }

    /// 127x29, measured off Device Hub's own button.
    ///
    /// Reported as the intrinsic size rather than set with constraints: this button lives in a
    /// frame-based NSStackView (MirrorView positions the block by hand), which dropped a width
    /// constraint on an arranged subview outright, and padding the title moved the width by about
    /// a point per space -- neither could land on a specific number. The stack does honour
    /// intrinsic content size.
    override var intrinsicContentSize: NSSize { NSSize(width: 127, height: 29) }

    override func layout() {
        super.layout()
        layer?.cornerRadius = bounds.height / 2        // capsule, as Device Hub's is
        apply()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        guard let window else { return }
        // The highlight is entirely a function of key state, so these two notifications are the
        // only thing that drives it.
        for name in [NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification] {
            observers.append(NotificationCenter.default.addObserver(
                forName: name, object: window, queue: .main) { [weak self] _ in self?.apply() })
        }
        apply()
    }

    deinit {
        for o in observers { NotificationCenter.default.removeObserver(o) }
    }

    private func apply() {
        let lit = isFocusedPanel && (window?.isKeyWindow ?? false)
        layer?.backgroundColor = (lit ? Self.active : Self.resting).cgColor
        contentTintColor = lit ? .white : .labelColor      // tints the SF Symbol
        attributedTitle = NSAttributedString(string: title, attributes: [
            .foregroundColor: lit ? NSColor.white : NSColor.labelColor,
            .font: font ?? NSFont.systemFont(ofSize: NSFont.systemFontSize),
        ])
    }
}
