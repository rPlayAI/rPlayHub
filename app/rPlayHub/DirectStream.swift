//
//  DirectStream.swift
//  Video straight from the device, with nothing in between.
//
//  The app asks the daemon where the tunnel is, then talks to the phone itself: it negotiates the
//  media stream, binds its own UDP port, and receives RTP directly. The daemon's only remaining
//  job is the part that needs root — creating the utun — plus telling us the addresses.
//
//  This replaces reading Annex-B from the daemon over loopback, and the reason is not the extra
//  hop. A proxy makes the daemon a producer and the app a consumer, and that coupling is unsafe
//  for a real-time source that cannot retransmit: a viewer reading slowly applies backpressure
//  that reaches the RTP thread, packets are dropped, and because the device sends one IDR per
//  session the corruption is permanent — with nothing pointing at the viewer as the cause. Owning
//  the socket removes the second consumer entirely rather than mitigating it.
//
//  Everything below the socket is the same C the daemon uses, compiled into this app: the offer,
//  RemoteXPC, depacketization and RTCP. One verified copy, not a reimplementation.
//
import Foundation

final class DirectStream {
    /// Every NAL, already framed with a start code, on the RTP thread.
    var onNAL: ((Data) -> Void)?
    /// Reported once when the stream is up.
    var onStarted: ((String) -> Void)?

    private var media: OpaquePointer?

    /// Where the daemon says the device is. Re-queried every connect: the tunnel gets a fresh
    /// ULA prefix each session, so these are never valid across a reconnect.
    struct Tunnel {
        let deviceAddr: String
        let ourAddr: String
        let displayPort: Int
        let hidPort: Int
        let screenshotPort: Int
    }

    static func fetchTunnel(_ control: ControlClient,
                            completion: @escaping (Tunnel?) -> Void) {
        control.send("tunnel_info") { result in
            guard case .success(let r) = result,
                  let dev = r["device_addr"] as? String, !dev.isEmpty,
                  let ours = r["our_addr"] as? String, !ours.isEmpty,
                  let svcs = r["services"] as? [String: Any] else {
                completion(nil)
                return
            }
            completion(Tunnel(deviceAddr: dev,
                              ourAddr: ours,
                              displayPort: (svcs["displayservice"] as? Int) ?? 0,
                              hidPort: (svcs["hid"] as? Int) ?? 0,
                              screenshotPort: (svcs["screenshot"] as? Int) ?? 0))
        }
    }

    func start(_ tunnel: Tunnel) -> Bool {
        guard tunnel.displayPort != 0 else {
            AppBuild.log("direct stream: the device did not offer displayservice")
            return false
        }

        // One identity for this receiver, used for the offer's field 5.1 AND every RTCP packet.
        // The device echoes it back as RemoteSSRC and silently discards feedback from any other
        // source — when those two drifted apart, LTR acks, PLI and FIR were all thrown away and
        // each failure looked like a separate protocol mystery.
        let ssrc = UInt32.random(in: 1...UInt32.max - 1)

        var cfg = media_config()
        let dev = strdup(tunnel.deviceAddr)
        let ours = strdup(tunnel.ourAddr)
        defer { free(dev); free(ours) }
        cfg.device_addr = UnsafePointer(dev)
        cfg.our_addr = UnsafePointer(ours)
        cfg.display_port = tunnel.displayPort
        cfg.ssrc = ssrc
        cfg.keyframe_every_s = Self.keyframeInterval

        let ctx = Unmanaged.passUnretained(self).toOpaque()
        media = media_start(&cfg, { ctx, annexb, len, _, _ in
            guard let ctx, let annexb else { return }
            let me = Unmanaged<DirectStream>.fromOpaque(ctx).takeUnretainedValue()
            me.onNAL?(Data(bytes: annexb, count: len))
        }, ctx)

        guard media != nil else {
            AppBuild.log("direct stream: media_start failed")
            return false
        }
        AppBuild.log("direct stream: receiving from \(tunnel.deviceAddr) with no proxy")
        DispatchQueue.main.async { [weak self] in self?.onStarted?(tunnel.deviceAddr) }
        return true
    }

    func stop() {
        if let media { media_stop(media) }
        media = nil
    }

    /// How often to ask for a fresh keyframe. The device sends exactly one IDR unprompted, so
    /// without asking, any corruption stays on screen for the rest of the session.
    private static var keyframeInterval: Double {
        if let s = ProcessInfo.processInfo.environment["RPLAY_KEYFRAME_EVERY_S"],
           let v = Double(s) { return v }
        return 3.0
    }

    var stats: (packets: UInt64, nals: UInt64, keyframes: UInt64,
                lost: UInt64, ltrAcked: UInt64, mbps: Double) {
        var p: UInt64 = 0, n: UInt64 = 0, k: UInt64 = 0, l: UInt64 = 0, a: UInt64 = 0
        var mbps: Double = 0
        media_stats(media, &p, &n, &k, &l, &a, &mbps)
        return (p, n, k, l, a, mbps)
    }
}
