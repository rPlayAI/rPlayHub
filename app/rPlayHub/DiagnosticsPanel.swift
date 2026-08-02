//
//  DiagnosticsPanel.swift
//  The inspector's ⓘ tab: what the device says about itself.
//
//  Device Hub puts three tabs in its right-hand inspector; this is the equivalent of the last one.
//  It is read-only on purpose -- the actions that change the device (restart, unpair) belong next
//  to each other and behind confirmation, not one click away from a list of serial numbers.
//

import AppKit

final class DiagnosticsPanel: NSView {
    /// The device to describe. Setting it refetches.
    var udid: String? {
        didSet { if udid != oldValue { refresh() } }
    }

    private let stack = NSStackView()
    private let statusLabel = NSTextField(labelWithString: "")
    private let refreshButton = NSButton()
    private var loading = false

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        refreshButton.title = "  Refresh"
        refreshButton.image = NSImage(systemSymbolName: "arrow.clockwise",
                                      accessibilityDescription: "Refresh")
        refreshButton.imagePosition = .imageLeading
        refreshButton.bezelStyle = .rounded
        refreshButton.target = self
        refreshButton.action = #selector(refresh)

        statusLabel.font = .systemFont(ofSize: 11)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.lineBreakMode = .byWordWrapping
        statusLabel.maximumNumberOfLines = 4

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.edgeInsets = NSEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
        stack.translatesAutoresizingMaskIntoConstraints = false

        // Scrolling, because the device section alone is twenty rows and the pane is 260 points.
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        let doc = NSView()
        doc.translatesAutoresizingMaskIntoConstraints = false
        doc.addSubview(stack)
        scroll.documentView = doc
        addSubview(scroll)

        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            doc.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
            doc.trailingAnchor.constraint(equalTo: scroll.contentView.trailingAnchor),
            stack.topAnchor.constraint(equalTo: doc.topAnchor),
            stack.leadingAnchor.constraint(equalTo: doc.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: doc.trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: doc.bottomAnchor),
        ])
        render(nil, message: "No device selected.")
    }

    @objc func refresh() {
        guard !loading else { return }
        loading = true
        render(nil, message: "Reading…")
        DeviceInfo.fetch(udid: udid) { [weak self] result in
            guard let self else { return }
            self.loading = false
            switch result {
            case .success(let info):
                self.render(info, message: nil)
            case .failure(let e):
                self.render(nil, message: e.localizedDescription)
            }
        }
    }

    private func header(_ text: String) -> NSView {
        let l = NSTextField(labelWithString: text)
        l.font = .systemFont(ofSize: 11, weight: .semibold)
        l.textColor = .secondaryLabelColor
        return l
    }

    /// One key/value line. The value is selectable: a UDID or ECID exists to be copied, and a
    /// panel that shows one without letting you take it is a screenshot, not a tool.
    private func row(_ key: String, _ value: String) -> NSView {
        let k = NSTextField(labelWithString: key)
        k.font = .systemFont(ofSize: 11)
        k.textColor = .secondaryLabelColor
        k.setContentHuggingPriority(.defaultLow, for: .horizontal)

        let v = NSTextField(labelWithString: value)
        v.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        v.isSelectable = true
        v.lineBreakMode = .byTruncatingMiddle
        v.alignment = .right
        v.toolTip = value
        v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let h = NSStackView(views: [k, v])
        h.orientation = .horizontal
        h.distribution = .fill
        h.spacing = 8
        h.translatesAutoresizingMaskIntoConstraints = false
        h.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -28).isActive = true
        return h
    }

    private func render(_ info: DeviceInfo?, message: String?) {
        stack.arrangedSubviews.forEach {
            stack.removeArrangedSubview($0)
            $0.removeFromSuperview()
        }
        stack.addArrangedSubview(refreshButton)

        if let message {
            statusLabel.stringValue = message
            stack.addArrangedSubview(statusLabel)
            return
        }
        guard let info else { return }

        for section in info.sections {
            stack.addArrangedSubview(header(section.title))
            for (k, v) in section.rows { stack.addArrangedSubview(row(k, v)) }
        }
        if !info.unavailable.isEmpty {
            stack.addArrangedSubview(header("Unavailable"))
            let l = NSTextField(labelWithString: info.unavailable.joined(separator: "\n"))
            l.font = .systemFont(ofSize: 10)
            l.textColor = .tertiaryLabelColor
            l.lineBreakMode = .byWordWrapping
            l.maximumNumberOfLines = 0
            stack.addArrangedSubview(l)
        }
    }
}
