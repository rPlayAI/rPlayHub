//
//  ControlStrip.swift
//  The row of actions under the screen.
//
//  Device Hub puts its device actions in a short icon row directly beneath the live view, not off
//  in the inspector. That placement is the right one: these act on the screen you are looking at,
//  so they belong next to it, and it leaves the right-hand pane free for what it is actually for
//  — describing the device.
//
//  Icon-only, with tooltips. Five buttons with words next to them would be wider than the phone.
//

import AppKit

final class ControlStrip: NSView {
    var onAction: ((ControlPanel.Action) -> Void)?

    private let pinButton = NSButton()
    private let homeButton = NSButton()
    private let rotateButton = NSButton()
    private let shotButton = NSButton()
    private let recordButton = NSButton()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func make(_ button: NSButton, _ symbol: String, _ tip: String, _ tag: Int) {
        button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip)
        button.bezelStyle = .texturedRounded
        button.imagePosition = .imageOnly
        button.toolTip = tip
        button.tag = tag
        button.target = self
        button.action = #selector(hit(_:))
    }

    private func build() {
        make(pinButton, "pin", "Pin Window on Top", 0)
        make(homeButton, "house", "Home", 1)
        make(rotateButton, "rotate.right", "Rotate", 2)
        make(shotButton, "camera", "Take Screenshot", 3)
        make(recordButton, "record.circle", "Record", 4)

        let stack = NSStackView(views: [pinButton, homeButton, rotateButton,
                                        shotButton, recordButton])
        stack.orientation = .horizontal
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
        ])
    }

    @objc private func hit(_ sender: NSButton) {
        switch sender.tag {
        case 0: onAction?(.pin)
        case 1: onAction?(.home)
        case 2: onAction?(.rotate)
        case 3: onAction?(.screenshot)
        default: onAction?(.record)
        }
    }

    func setPinned(_ pinned: Bool) {
        pinButton.image = NSImage(systemSymbolName: pinned ? "pin.fill" : "pin",
                                  accessibilityDescription: "Pin")
        pinButton.toolTip = pinned ? "Unpin Window" : "Pin Window on Top"
    }

    func setRecording(_ recording: Bool) {
        recordButton.image = NSImage(systemSymbolName: recording ? "stop.circle.fill"
                                                                 : "record.circle",
                                     accessibilityDescription: "Record")
        recordButton.contentTintColor = recording ? .systemRed : nil
        recordButton.toolTip = recording ? "Stop Recording" : "Record"
    }
}
