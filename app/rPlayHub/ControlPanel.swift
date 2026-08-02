//
//  ControlPanel.swift
//  The right-hand pane, where Device Hub puts its inspector.
//
//  Device Hub's version holds simulator settings (Appearance, Text Size, VoiceOver…) which we have
//  no equivalent for. Ours holds the controls that are actually ours — Pin, Home, Screenshot,
//  Record — plus live stream health, because "is video flowing and are we getting keyframes" is the
//  question that comes up every time something looks wrong.
//
//  Icons are SF Symbols, not copied artwork: system-provided, and it is what Device Hub uses.
//

import AppKit

final class ControlPanel: NSView {
    enum Action {
        case pin
        case home
        case rotate
        case screenshot
        case record
    }

    var onAction: ((Action) -> Void)?

    private let pinButton = NSButton()
    private let homeButton = NSButton()
    private let rotateButton = NSButton()
    private let shotButton = NSButton()
    private let recordButton = NSButton()
    private let statusLabel = NSTextField(labelWithString: "")
    private let healthLabel = NSTextField(labelWithString: "")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private static func symbol(_ name: String, fallback: String) -> NSImage? {
        NSImage(systemSymbolName: name, accessibilityDescription: fallback)
    }

    private func makeButton(_ title: String, _ symbolName: String,
                            _ action: Action, _ target: NSButton) {
        target.title = "  " + title
        target.image = Self.symbol(symbolName, fallback: title)
        target.imagePosition = .imageLeading
        target.alignment = .left
        target.bezelStyle = .rounded
        target.target = self
        target.tag = {
            switch action {
            case .pin: return 0
            case .home: return 1
            case .rotate: return 2
            case .screenshot: return 3
            case .record: return 4
            }
        }()
        target.action = #selector(buttonHit(_:))
    }

    private func build() {
        let header = NSTextField(labelWithString: "Controls")
        header.font = .systemFont(ofSize: 11, weight: .semibold)
        header.textColor = .secondaryLabelColor

        makeButton("Pin Window on Top", "pin", .pin, pinButton)
        makeButton("Home", "house", .home, homeButton)
        makeButton("Rotate", "rotate.right", .rotate, rotateButton)
        makeButton("Screenshot", "camera", .screenshot, shotButton)
        makeButton("Record", "record.circle", .record, recordButton)

        let statusHeader = NSTextField(labelWithString: "Stream")
        statusHeader.font = .systemFont(ofSize: 11, weight: .semibold)
        statusHeader.textColor = .secondaryLabelColor

        statusLabel.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        statusLabel.textColor = .labelColor
        statusLabel.lineBreakMode = .byWordWrapping
        statusLabel.maximumNumberOfLines = 3

        healthLabel.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        healthLabel.textColor = .secondaryLabelColor
        healthLabel.lineBreakMode = .byWordWrapping
        healthLabel.maximumNumberOfLines = 6

        let stack = NSStackView(views: [
            header, pinButton, homeButton, rotateButton, shotButton, recordButton,
            statusHeader, statusLabel, healthLabel,
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.edgeInsets = NSEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])
        for b in [pinButton, homeButton, rotateButton, shotButton, recordButton] {
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -28).isActive = true
        }
    }

    @objc private func buttonHit(_ sender: NSButton) {
        switch sender.tag {
        case 0: onAction?(.pin)
        case 1: onAction?(.home)
        case 2: onAction?(.rotate)
        case 3: onAction?(.screenshot)
        default: onAction?(.record)
        }
    }

    // MARK: - state shown back to the user

    func setPinned(_ pinned: Bool) {
        pinButton.title = pinned ? "  Unpin Window" : "  Pin Window on Top"
        pinButton.image = Self.symbol(pinned ? "pin.fill" : "pin", fallback: "Pin")
    }

    func setRecording(_ recording: Bool) {
        recordButton.title = recording ? "  Stop Recording" : "  Record"
        recordButton.image = Self.symbol(recording ? "stop.circle.fill" : "record.circle",
                                         fallback: "Record")
        recordButton.contentTintColor = recording ? .systemRed : nil
    }

    func setStatus(_ text: String) { statusLabel.stringValue = text }
    func setHealth(_ text: String) { healthLabel.stringValue = text }
}
