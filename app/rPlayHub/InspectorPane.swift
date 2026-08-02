//
//  InspectorPane.swift
//  The right-hand pane and its tabs.
//
//  Device Hub's inspector has three tabs across the top; this is the same idea with the two we
//  have content for. It exists so AppDelegate keeps talking to one object: it forwards the
//  control surface it already used, and owns the tab switching itself.
//

import AppKit

final class InspectorPane: NSView {
    let controls = ControlPanel()
    let diagnostics = DiagnosticsPanel()

    private let tabs = NSSegmentedControl()

    /// The device whose details the Diagnostics tab should show.
    var udid: String? {
        didSet { diagnostics.udid = udid }
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        tabs.segmentCount = 2
        tabs.setImage(NSImage(systemSymbolName: "slider.horizontal.3",
                              accessibilityDescription: "Controls"), forSegment: 0)
        tabs.setImage(NSImage(systemSymbolName: "info.circle",
                              accessibilityDescription: "Info"), forSegment: 1)
        tabs.setWidth(0, forSegment: 0)     // 0 = size to fit
        tabs.segmentStyle = .texturedRounded
        tabs.selectedSegment = 0
        tabs.target = self
        tabs.action = #selector(tabChanged)
        tabs.translatesAutoresizingMaskIntoConstraints = false

        for v in [controls as NSView, diagnostics as NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        addSubview(tabs)

        NSLayoutConstraint.activate([
            tabs.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            tabs.centerXAnchor.constraint(equalTo: centerXAnchor),
        ])
        for v in [controls as NSView, diagnostics as NSView] {
            NSLayoutConstraint.activate([
                v.topAnchor.constraint(equalTo: tabs.bottomAnchor, constant: 4),
                v.leadingAnchor.constraint(equalTo: leadingAnchor),
                v.trailingAnchor.constraint(equalTo: trailingAnchor),
                v.bottomAnchor.constraint(equalTo: bottomAnchor),
            ])
        }
        tabChanged()
    }

    @objc private func tabChanged() {
        let showInfo = tabs.selectedSegment == 1
        controls.isHidden = showInfo
        diagnostics.isHidden = !showInfo
        // Fetch on first reveal rather than on every device change: the query opens a lockdown
        // session and takes a moment, and doing it for a tab nobody is looking at is waste.
        if showInfo { diagnostics.refresh() }
    }
}
