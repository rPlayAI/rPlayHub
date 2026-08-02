//
//  DeviceModel.swift
//  Turning "iPhone14,2" into "iPhone 13 Pro".
//
//  Device Hub's sidebar shows the marketing name under the device name, and the identifier is the
//  only thing on the wire. The map is partial on purpose: an unknown identifier is returned as-is
//  rather than guessed at, because "iPhone19,5" shown verbatim is honest and still identifies the
//  device, whereas inventing a name for it would not.
//

import CoreGraphics
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

    /// Native screen size in pixels, which is NOT the coded frame size.
    ///
    /// The encoder pads the picture up to an alignment boundary -- 1170x2532 of screen inside a
    /// 1184x2576 frame on an iPhone 13 Pro -- and the padding has to be cropped off or it shows
    /// as black bands. cdhost never learns the real size (its screen_w/screen_h are declared,
    /// read, and never assigned), so until it implements getdisplayinfo this table stands in.
    /// It is static per model, so it is right whenever the identifier is known.
    private static let screens: [String: CGSize] = [
        "iPhone14,2": CGSize(width: 1170, height: 2532),   // 13 Pro
        "iPhone14,3": CGSize(width: 1284, height: 2778),   // 13 Pro Max
        "iPhone14,4": CGSize(width: 1080, height: 2340),   // 13 mini
        "iPhone14,5": CGSize(width: 1170, height: 2532),   // 13
        "iPhone14,6": CGSize(width: 750, height: 1334),    // SE 3
        "iPhone14,7": CGSize(width: 1170, height: 2532),   // 14
        "iPhone14,8": CGSize(width: 1284, height: 2778),   // 14 Plus
        "iPhone15,2": CGSize(width: 1179, height: 2556),   // 14 Pro
        "iPhone15,3": CGSize(width: 1290, height: 2796),   // 14 Pro Max
        "iPhone15,4": CGSize(width: 1179, height: 2556),   // 15
        "iPhone15,5": CGSize(width: 1290, height: 2796),   // 15 Plus
        "iPhone16,1": CGSize(width: 1179, height: 2556),   // 15 Pro
        "iPhone16,2": CGSize(width: 1290, height: 2796),   // 15 Pro Max
        "iPhone17,1": CGSize(width: 1206, height: 2622),   // 16 Pro
        "iPhone17,2": CGSize(width: 1320, height: 2868),   // 16 Pro Max
        "iPhone17,3": CGSize(width: 1179, height: 2556),   // 16
        "iPhone17,4": CGSize(width: 1290, height: 2796),   // 16 Plus
        "iPhone13,1": CGSize(width: 1080, height: 2340),   // 12 mini
        "iPhone13,2": CGSize(width: 1170, height: 2532),   // 12
        "iPhone13,3": CGSize(width: 1170, height: 2532),   // 12 Pro
        "iPhone13,4": CGSize(width: 1284, height: 2778),   // 12 Pro Max
        "iPhone12,1": CGSize(width: 828, height: 1792),    // 11
        "iPhone12,3": CGSize(width: 1125, height: 2436),   // 11 Pro
        "iPhone12,5": CGSize(width: 1242, height: 2688),   // 11 Pro Max
        "iPhone12,8": CGSize(width: 750, height: 1334),    // SE 2
    ]

    /// nil when the identifier is unknown, so the caller shows the whole coded frame rather than
    /// cropping to a size that was guessed.
    static func screenSize(for productType: String?) -> CGSize? {
        guard let productType else { return nil }
        return screens[productType]
    }

    /// The marketing name, or the identifier itself when we do not know it.
    static func name(for productType: String?) -> String? {
        guard let productType, !productType.isEmpty else { return nil }
        return names[productType] ?? productType
    }

    /// What sits at the top of the screen and is not part of the picture.
    ///
    /// The device streams the full rectangle including the area behind the notch or island, so
    /// that region arrives as real pixels. Device Hub covers it, which is why its mirror reads as
    /// a physical phone rather than a video of one.
    enum Cutout {
        case none                      // home-button devices: nothing to cover
        case notch                     // hangs off the top edge
        case island                    // floats below the top edge
    }

    static func cutout(for productType: String?) -> Cutout {
        guard let productType else { return .none }
        switch productType {
        case "iPhone12,8", "iPhone14,6":                       // SE 2, SE 3
            return .none
        case "iPhone15,2", "iPhone15,3",                       // 14 Pro, Pro Max
             "iPhone15,4", "iPhone15,5",                       // 15, 15 Plus
             "iPhone16,1", "iPhone16,2",                       // 15 Pro, Pro Max
             "iPhone17,1", "iPhone17,2", "iPhone17,3", "iPhone17,4":
            return .island
        default:
            return productType.hasPrefix("iPhone") ? .notch : .none
        }
    }

    /// Screen corner rounding, as a fraction of screen width. Measured off the real devices:
    /// an iPhone 13 Pro is 47.33pt on a 390pt-wide screen.
    static func cornerFraction(for productType: String?) -> CGFloat {
        switch cutout(for: productType) {
        case .none: return 0
        case .notch, .island: return 47.33 / 390.0
        }
    }

    /// The cutout rectangle, in fractions of the screen. Width and height are fractions of screen
    /// width and height; `top` is how far below the top edge it starts.
    static func cutoutRect(for productType: String?) -> (w: CGFloat, h: CGFloat, top: CGFloat)? {
        switch cutout(for: productType) {
        case .none:   return nil
        case .notch:  return (162.0 / 390.0, 33.0 / 844.0, 0)
        case .island: return (125.0 / 393.0, 37.0 / 852.0, 11.0 / 852.0)
        }
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
