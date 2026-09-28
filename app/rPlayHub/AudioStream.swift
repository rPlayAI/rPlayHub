//
//  AudioStream.swift
//  The phone's sound, played on the Mac.
//
//  Device Hub 27 plays the device's audio, and it turned out to be one more media stream on the
//  same service as the screen: startmediastream with type "audio". The engine negotiates it and
//  forwards each frame on 127.0.0.1:9878 as <u16 BE length><u32 BE RTP timestamp><frame>.
//
//  Each frame is one AAC-ELD access unit: 48 kHz, stereo, 480 samples (10 ms). A 4-byte frame is
//  the encoder's silence frame, sent continuously while nothing plays, and decodes like any other.
//  Decoding is AudioToolbox's own AAC-ELD decoder, so nothing here is a codec.
//
import AVFoundation
import Foundation

final class AudioStream {
    private let socket: TCPSocket
    private var thread: Thread?
    private var stopping = false

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private let pcmFormat = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
    private var eldFormat: AVAudioFormat?
    private var converter: AVAudioConverter?

    /// Buffers handed to the player and not yet played. The phone's clock and the Mac's never
    /// agree exactly, so without a ceiling the difference piles up as ever-growing delay.
    private let queueLock = NSLock()
    private var queued = 0
    private var started = false
    /// 30 ms of cushion before playback starts, 200 ms before frames are dropped to catch up.
    private static let startAfter = 3
    private static let dropAbove = 20

    /// Called on the main queue when the connection ends, with a reason.
    var onDisconnect: ((String) -> Void)?

    private(set) var framesReceived = 0
    private(set) var framesDropped = 0
    /// Frames the decoder refused. Should stay at zero; anything else means the format moved.
    private(set) var framesUndecodable = 0

    init(host: String = "127.0.0.1", port: UInt16 = 9878) {
        socket = TCPSocket(host: host, port: port)
    }

    func start() throws {
        try setUpDecoder()
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: pcmFormat)
        try engine.start()
        try socket.connect()
        stopping = false
        let t = Thread { [weak self] in self?.readLoop() }
        t.name = "rplayhub.audio"
        thread = t
        t.start()
    }

    func stop() {
        stopping = true
        socket.shutdownAndClose()
        player.stop()
        engine.stop()
    }

    // MARK: - decode

    /// The decoder config, as AudioToolbox wants it: an MPEG-4 ES descriptor wrapping the
    /// AudioSpecificConfig. The ASC is ER AAC-ELD (object type 39), 48 kHz, two channels,
    /// 480-sample frames, no resilience tools, no SBR -- f8 e6 50 00. The wrapper is copied from
    /// what AudioToolbox's own AAC-ELD encoder emits for the same format; a bare ASC is refused.
    private static let magicCookie: [UInt8] = [
        0x03, 0x80, 0x80, 0x80, 0x24, 0x00, 0x00, 0x00,
        0x04, 0x80, 0x80, 0x80, 0x16, 0x40, 0x14, 0x00, 0x18, 0x00,
        0x00, 0x01, 0xf4, 0x00, 0x00, 0x01, 0xf4, 0x00,
        0x05, 0x80, 0x80, 0x80, 0x04, 0xf8, 0xe6, 0x50, 0x00,
        0x06, 0x80, 0x80, 0x80, 0x01, 0x02,
    ]

    private func setUpDecoder() throws {
        var asbd = AudioStreamBasicDescription(
            mSampleRate: 48000, mFormatID: kAudioFormatMPEG4AAC_ELD, mFormatFlags: 0,
            mBytesPerPacket: 0, mFramesPerPacket: 480, mBytesPerFrame: 0,
            mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
        guard let fmt = AVAudioFormat(streamDescription: &asbd) else {
            throw NSError(domain: "rPlayHub.audio", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "no AAC-ELD format"])
        }
        fmt.magicCookie = Data(Self.magicCookie)
        guard let conv = AVAudioConverter(from: fmt, to: pcmFormat) else {
            throw NSError(domain: "rPlayHub.audio", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "no AAC-ELD decoder"])
        }
        eldFormat = fmt
        converter = conv
    }

    private func decode(_ frame: Data) -> AVAudioPCMBuffer? {
        guard let fmt = eldFormat, let conv = converter else { return nil }
        let packet = AVAudioCompressedBuffer(format: fmt, packetCapacity: 1,
                                             maximumPacketSize: max(frame.count, 1))
        frame.withUnsafeBytes { packet.data.copyMemory(from: $0.baseAddress!, byteCount: frame.count) }
        packet.byteLength = UInt32(frame.count)
        packet.packetCount = 1
        packet.packetDescriptions?.pointee = AudioStreamPacketDescription(
            mStartOffset: 0, mVariableFramesInPacket: 0, mDataByteSize: UInt32(frame.count))

        guard let out = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: 480) else { return nil }
        var given = false
        var error: NSError?
        let status = conv.convert(to: out, error: &error) { _, inputStatus in
            if given { inputStatus.pointee = .noDataNow; return nil }
            given = true
            inputStatus.pointee = .haveData
            return packet
        }
        if status == .error || out.frameLength == 0 { return nil }
        return out
    }

    // MARK: - play

    private func play(_ buffer: AVAudioPCMBuffer) {
        queueLock.lock()
        if queued > Self.dropAbove {
            // Behind by more than 200 ms: the phone is producing faster than the Mac plays.
            // Skipping a frame is a 10 ms blip; letting the backlog grow is permanent lag.
            queueLock.unlock()
            framesDropped += 1
            return
        }
        queued += 1
        let begin = !started && queued >= Self.startAfter
        if begin { started = true }
        queueLock.unlock()

        player.scheduleBuffer(buffer) { [weak self] in
            guard let self else { return }
            self.queueLock.lock()
            self.queued -= 1
            // Ran dry: rebuild the cushion before resuming, rather than stuttering frame by frame.
            if self.queued == 0 { self.started = false }
            self.queueLock.unlock()
        }
        if begin { player.play() }
    }

    // MARK: - read

    private func readLoop() {
        var buf = Data()
        while !stopping {
            do {
                guard let chunk = try socket.read() else { continue }   // timeout, retry
                buf.append(chunk)
                var off = buf.startIndex
                while buf.endIndex - off >= 6 {
                    let len = Int(buf[off]) << 8 | Int(buf[off + 1])
                    guard buf.endIndex - off >= 6 + len else { break }
                    let frame = buf.subdata(in: (off + 6)..<(off + 6 + len))
                    off += 6 + len
                    framesReceived += 1
                    if let pcm = decode(frame) { play(pcm) } else { framesUndecodable += 1 }
                }
                buf.removeSubrange(buf.startIndex..<off)
            } catch {
                if !stopping {
                    let reason = "\(error)"
                    DispatchQueue.main.async { [weak self] in self?.onDisconnect?(reason) }
                }
                return
            }
        }
    }
}
