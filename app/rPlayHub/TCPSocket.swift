//
//  TCPSocket.swift
//  A minimal blocking TCP client over Darwin sockets.
//
//  Deliberately not Network.framework: rplay hit a build failure with the Swift `Network`
//  module on this SDK, and both of our sockets want a plain blocking reader thread anyway.
//

import Darwin
import Foundation

enum SocketError: Error, CustomStringConvertible {
    case create(Int32)
    case connect(String, UInt16, Int32)
    case closed
    case write(Int32)

    var description: String {
        switch self {
        case .create(let e):            return "socket() failed: \(String(cString: strerror(e)))"
        case .connect(let h, let p, let e): return "connect \(h):\(p) failed: \(String(cString: strerror(e)))"
        case .closed:                   return "connection closed by peer"
        case .write(let e):             return "write failed: \(String(cString: strerror(e)))"
        }
    }
}

final class TCPSocket {
    private(set) var fd: Int32 = -1
    let host: String
    let port: UInt16

    init(host: String, port: UInt16) {
        self.host = host
        self.port = port
    }

    var isOpen: Bool { fd >= 0 }

    func connect(timeout: TimeInterval = 5) throws {
        let s = socket(AF_INET, SOCK_STREAM, 0)
        guard s >= 0 else { throw SocketError.create(errno) }

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = port.bigEndian
        addr.sin_addr.s_addr = inet_addr(host)

        let rc = withUnsafePointer(to: &addr) { ptr -> Int32 in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sa in
                Darwin.connect(s, sa, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard rc == 0 else {
            let e = errno
            close(s)
            throw SocketError.connect(host, port, e)
        }

        // Keep reads from blocking forever so a reader thread can notice shutdown.
        var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var one: Int32 = 1
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))

        fd = s
    }

    /// One read. Returns nil on timeout, throws on EOF or error.
    func read(max: Int = 1 << 16) throws -> Data? {
        guard fd >= 0 else { throw SocketError.closed }
        var buf = [UInt8](repeating: 0, count: max)
        let n = buf.withUnsafeMutableBytes { Darwin.read(fd, $0.baseAddress, max) }
        if n > 0 { return Data(buf[0..<n]) }
        if n == 0 { throw SocketError.closed }
        if errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR { return nil }
        throw SocketError.closed
    }

    func writeAll(_ data: Data) throws {
        guard fd >= 0 else { throw SocketError.closed }
        try data.withUnsafeBytes { raw in
            var remaining = raw.count
            var p = raw.baseAddress
            while remaining > 0 {
                let w = Darwin.write(fd, p, remaining)
                if w <= 0 {
                    if errno == EINTR { continue }
                    throw SocketError.write(errno)
                }
                remaining -= w
                p = p?.advanced(by: w)
            }
        }
    }

    func shutdownAndClose() {
        guard fd >= 0 else { return }
        Darwin.shutdown(fd, SHUT_RDWR)
        close(fd)
        fd = -1
    }
}
