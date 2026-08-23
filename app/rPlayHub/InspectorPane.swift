//
//  InspectorPane.swift
//  The right-hand pane and its tabs.
//
//  Device Hub's inspector has Info, Apps and Profiles across the top; this has those three plus
//  Controls and Console. It exists so AppDelegate keeps talking to one object: it forwards the control
//  surface it already used, and owns the tab switching itself.
//

import AppKit

final class InspectorPane: NSView {
    let controls = ControlPanel()
    let diagnostics = DiagnosticsPanel()
    let apps = AppsPanel()
    let profiles = ProfilesPanel()
    let console = ConsolePanel()

    private let tabs = NSSegmentedControl()

    /// The device whose details the Diagnostics tab should show.
    var udid: String? {
        didSet { diagnostics.udid = udid }
    }

    /// The engine connection the Apps tab talks through. Console opens its own.
    var control: ControlClient? {
        didSet { apps.control = control; profiles.control = control }
    }

    private var panes: [NSView] { [controls, diagnostics, apps, profiles, console] }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        let icons = [("slider.horizontal.3", "Controls"), ("info.circle", "Info"),
                     ("square.grid.2x2", "Apps"), ("checkmark.seal", "Profiles"),
                     ("text.alignleft", "Console")]
        tabs.segmentCount = icons.count
        for (i, (symbol, label)) in icons.enumerated() {
            tabs.setImage(NSImage(systemSymbolName: symbol, accessibilityDescription: label),
                          forSegment: i)
            tabs.setToolTip(label, forSegment: i)
            tabs.setWidth(0, forSegment: i)     // 0 = size to fit
        }
        tabs.segmentStyle = .texturedRounded
        tabs.selectedSegment = 0
        tabs.target = self
        tabs.action = #selector(tabChanged)
        tabs.translatesAutoresizingMaskIntoConstraints = false

        for v in panes {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        addSubview(tabs)

        NSLayoutConstraint.activate([
            tabs.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            tabs.centerXAnchor.constraint(equalTo: centerXAnchor),
        ])
        for v in panes {
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
        let sel = tabs.selectedSegment
        for (i, v) in panes.enumerated() { v.isHidden = i != sel }
        // Fetch on first reveal rather than on every device change: the query opens a lockdown
        // session and takes a moment, and doing it for a tab nobody is looking at is waste.
        if sel == 1 { diagnostics.refresh() }
        if sel == 2 { apps.revealed() }
        if sel == 3 { profiles.revealed() }
    }
}
