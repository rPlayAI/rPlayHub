//
//  DeviceModel.swift
//  Turning "iPhone14,2" into "iPhone 13 Pro".
//
//  Device Hub's sidebar shows the marketing name under the device name, and the identifier is the
//  only thing on the wire. The map is partial on purpose: an unknown identifier is returned as-is
//  rather than guessed at, because "iPhone19,5" shown verbatim is honest and still identifies the
//  device, whereas inventing a name for it would not.
//

import Foundation

enum DeviceModel {
    private static let names: [String: String] = [
        // iPhone
        "iPhone14,2": "iPhone 13 Pro",
        "iPhone14,3": "iPhone 13 Pro Max",
        "iPhone14,4": "iPhone 13 mini",
        "iPhone14,5": "iPhone 13",
        "iPhone14,6": "iPhone SE (3rd generation)",
        "iPhone14,7": "iPhone 14",
        "iPhone14,8": "iPhone 14 Plus",
        "iPhone15,2": "iPhone 14 Pro",
        "iPhone15,3": "iPhone 14 Pro Max",
        "iPhone15,4": "iPhone 15",
        "iPhone15,5": "iPhone 15 Plus",
        "iPhone16,1": "iPhone 15 Pro",
        "iPhone16,2": "iPhone 15 Pro Max",
        "iPhone17,1": "iPhone 16 Pro",
        "iPhone17,2": "iPhone 16 Pro Max",
        "iPhone17,3": "iPhone 16",
        "iPhone17,4": "iPhone 16 Plus",
        "iPhone17,5": "iPhone 16e",
        "iPhone13,1": "iPhone 12 mini",
        "iPhone13,2": "iPhone 12",
        "iPhone13,3": "iPhone 12 Pro",
        "iPhone13,4": "iPhone 12 Pro Max",
        "iPhone12,1": "iPhone 11",
        "iPhone12,3": "iPhone 11 Pro",
        "iPhone12,5": "iPhone 11 Pro Max",
        "iPhone12,8": "iPhone SE (2nd generation)",
        // iPad
        "iPad14,3": "iPad Pro 11-inch (4th generation)",
        "iPad14,5": "iPad Pro 12.9-inch (6th generation)",
        "iPad13,16": "iPad Air (5th generation)",
        "iPad14,1": "iPad mini (6th generation)",
    ]

    /// The marketing name, or the identifier itself when we do not know it.
    static func name(for productType: String?) -> String? {
        guard let productType, !productType.isEmpty else { return nil }
        return names[productType] ?? productType
    }

    /// The SF Symbol that suits this device class. Device Hub draws a device glyph per row.
    static func symbol(for productType: String?) -> String {
        guard let productType else { return "iphone" }
        if productType.hasPrefix("iPad") { return "ipad" }
        if productType.hasPrefix("Watch") { return "applewatch" }
        if productType.hasPrefix("AppleTV") { return "appletv" }
        return "iphone"
    }
}
