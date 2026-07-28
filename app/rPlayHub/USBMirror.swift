//
//  USBMirror.swift
//  The iPhone's screen as an AVCaptureDevice, over the USB cable.
//
//  This is a second, independent way to get the picture, and on macOS it is the one that looks
//  good. macOS ships a CoreMediaIO DAL plug-in (iOSScreenCapture.plugin) that presents a
//  USB-tethered iPhone as a muxed AVCaptureDevice — the same public path QuickTime Player, OBS
//  and Reflector use. Not a private API; the header has been in the SDK since 10.7.
//
//  Why bother, when the CoreDevice path already works: because that path is capped. displayservice
//  negotiates 1184x2544 at a 6 Mbps ceiling — measured, and confirmed against Apple's own Device
//  Hub session, which garbles identically under a home-screen swipe. That is roughly 0.03 bits per
//  pixel, about 3.5x below what UI content needs, and no combination of offer fields moved it. The
//  capture plug-in is built for screen recording rather than for remote debugging over a possibly
//  wireless link, so it carries no such budget. ~/rplay has mirrored this way for years without
//  the artefacting.
//
//  The two paths are complementary, not competing:
//     USB capture     — the picture, when a cable is present
//     CoreDevice      — touch and HID, device discovery, and video when there is no cable
//
//  Portability: CoreMediaIO is macOS-only, but the thing underneath it is a documented USB
//  protocol, so Linux and Windows can speak it directly rather than through Apple's plug-in. That
//  keeps this consistent with the project's goal of running the same stack elsewhere.
//
import AVFoundation
import CoreMediaIO
import Foundation

final class USBMirror: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    /// A decoded frame, on the capture queue. Same shape VideoDecoder emits, so both video paths
    /// feed the identical display layer.
    var onFrame: ((CVPixelBuffer) -> Void)?
    /// Reported once, when the first frame reveals the real dimensions.
    var onSize: ((CGSize) -> Void)?
    /// Human-readable state for the status line.
    private(set) var status = "not started"

    private var session: AVCaptureSession?
    private let queue = DispatchQueue(label: "rplayhub.usbcapture")
    private var reportedSize = false
    private(set) var frames = 0

    /// Opt in as early as possible, so the plug-in has loaded by the time anything enumerates.
    ///
    /// The DAL plug-in loads ASYNCHRONOUSLY after the opt-in is set. Measured on macOS 26.5:
    /// enumerating immediately returns zero devices, and the tethered iPhone appears about a
    /// second later. Calling this at launch removes the race for the common case; the retry in
    /// the app covers the rest. Without it the USB path never engaged at all — the very first
    /// enumeration always lost, and losing looked exactly like "no cable".
    static func prime() {
        enableScreenCaptureDevices()
    }

    /// Is a USB-tethered iPhone available to capture from right now?
    static func availableDevice() -> AVCaptureDevice? {
        enableScreenCaptureDevices()
        // Muxed only. A video-only iPhone device is Continuity Camera — the phone's camera lens
        // as a webcam, possibly over wifi — which is emphatically not what someone asking to
        // mirror a cabled iPhone wants. With no fallback, an untrusted phone fails cleanly
        // instead of silently showing a camera feed.
        var candidates = AVCaptureDevice.devices(for: .muxed)
        // .external replaced .externalUnknown in macOS 14; the deployment target is older, so ask
        // for whichever the running SDK has rather than raising the minimum for one enum case.
        let externalTypes: [AVCaptureDevice.DeviceType]
        if #available(macOS 14.0, *) {
            externalTypes = [.external]
        } else {
            externalTypes = [.externalUnknown]
        }
        let discovered = AVCaptureDevice.DiscoverySession(
            deviceTypes: externalTypes, mediaType: .muxed, position: .unspecified).devices
        for d in discovered where !candidates.contains(where: { $0.uniqueID == d.uniqueID }) {
            candidates.append(d)
        }
        return candidates.first { looksLikeIPhone($0) }
    }

    private static func looksLikeIPhone(_ d: AVCaptureDevice) -> Bool {
        let name = d.localizedName.lowercased()
        if name.contains("facetime") || name.contains("studio display") { return false }
        return name.contains("iphone") || name.contains("ipad") || d.modelID.contains("iOS")
    }

    /// Opt this process in to CoreMediaIO's screen-capture devices.
    ///
    /// Load-bearing: macOS hides these from non-Apple processes, and without this call every
    /// enumeration returns zero devices no matter what permissions or entitlements are granted.
    /// Must run before any device discovery.
    ///
    /// Deliberately not setting AllowWirelessScreenCaptureDevices: that also enumerates any
    /// iPhone the Mac is paired with over wifi, which can take precedence over the cabled one.
    private static func enableScreenCaptureDevices() {
        var allow: UInt32 = 1
        var address = CMIOObjectPropertyAddress(
            mSelector: CMIOObjectPropertySelector(kCMIOHardwarePropertyAllowScreenCaptureDevices),
            mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
            mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
        CMIOObjectSetPropertyData(CMIOObjectID(kCMIOObjectSystemObject), &address,
                                  0, nil, UInt32(MemoryLayout<UInt32>.size), &allow)
    }

    /// Ask for camera access, prompting the user the first time.
    ///
    /// Required, and easy to leave out: the tethered iPhone arrives through the camera subsystem,
    /// so without an authorised process AVCaptureDeviceInput fails and macOS never prompts —
    /// which looks exactly like "no device" and is why nothing appeared on the first attempt.
    static func requestAuthorization(_ done: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            done(true)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                DispatchQueue.main.async { done(granted) }
            }
        default:
            done(false)      // denied or restricted: only the user can undo this, in Settings
        }
    }

    /// Every capture device macOS is showing us, for the log. "No device" and "the wrong device"
    /// look identical from the outside and need completely different fixes.
    static var muxedDeviceSummary: String {
        enableScreenCaptureDevices()
        let muxed = AVCaptureDevice.devices(for: .muxed)
        let video = AVCaptureDevice.devices(for: .video)
        if muxed.isEmpty && video.isEmpty { return "no capture devices at all" }
        let names = (muxed.map { "muxed:\($0.localizedName)" }
                     + video.map { "video:\($0.localizedName)" }).joined(separator: ", ")
        return "devices = [\(names)]"
    }

    static var authorizationDescription: String {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:    return "granted"
        case .notDetermined: return "not yet asked"
        case .denied:        return "denied — grant it in Settings > Privacy & Security > Camera"
        case .restricted:    return "restricted by policy"
        @unknown default:    return "unknown"
        }
    }

    func start() -> Bool {
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            status = "camera permission \(Self.authorizationDescription)"
            return false
        }
        guard let device = Self.availableDevice() else {
            let muxed = AVCaptureDevice.devices(for: .muxed).count
            status = muxed == 0
                ? "no USB-tethered iPhone yet — cable connected and trusted? (the capture "
                  + "plug-in also takes about a second to load)"
                : "\(muxed) muxed device(s) present but none looked like an iPhone"
            return false
        }
        guard let input = try? AVCaptureDeviceInput(device: device) else {
            status = "could not open \(device.localizedName)"
            return false
        }

        let s = AVCaptureSession()
        s.beginConfiguration()
        guard s.canAddInput(input) else {
            status = "session refused \(device.localizedName)"
            return false
        }
        s.addInput(input)

        let output = AVCaptureVideoDataOutput()
        // BGRA because that is what the display layer shows directly as an IOSurface — the same
        // format the VideoToolbox path produces, so the two video paths converge here.
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String:
                                    kCVPixelFormatType_32BGRA]
        // Dropping a late frame is free here, unlike on the CoreDevice path: every frame from the
        // plug-in is independently displayable, so a gap cannot corrupt what follows.
        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)
        guard s.canAddOutput(output) else {
            status = "session refused a video output"
            return false
        }
        s.addOutput(output)
        s.commitConfiguration()
        s.startRunning()

        session = s
        status = "capturing \(device.localizedName)"
        NSLog("rPlayHub: USB capture started on \(device.localizedName)")
        return true
    }

    func stop() {
        session?.stopRunning()
        session = nil
        status = "stopped"
    }

    func captureOutput(_ output: AVCaptureOutput,
                       didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        guard let picture = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        frames += 1
        if !reportedSize {
            reportedSize = true
            let size = CGSize(width: CVPixelBufferGetWidth(picture),
                              height: CVPixelBufferGetHeight(picture))
            NSLog("rPlayHub: USB capture is \(Int(size.width))x\(Int(size.height))")
            DispatchQueue.main.async { [weak self] in self?.onSize?(size) }
        }
        onFrame?(picture)
    }
}
