//
//  InspectorPane.swift
//  The right-hand pane and its tabs.
//
//  Device Hub's inspector has two levels: a top-right row of 3 ICON tabs (Settings, Report,
//  Info), and under Info, a row of TEXT-named tabs (Info, Apps, Profiles). This has those three
//  plus Controls, Files and Console, which Device Hub has no equivalent for -- folded in as
//  extra text sub-tabs alongside Info/Apps/Profiles rather than as their own icon tabs, so the
//  top row stays a faithful 3 icons. Settings and Report need engine methods we don't have yet
//  (device appearance/accessibility, a diagnostics report) and are stubbed until then.
//
//  It exists so AppDelegate keeps talking to one object: it forwards the control surface it
//  already used, and owns the tab switching itself.
//

import AppKit

final class InspectorPane: NSView {
    let controls = ControlPanel()
    let diagnostics = DiagnosticsPanel()
    let apps = AppsPanel()
    let profiles = ProfilesPanel()
    let files = FilesPanel()
    let console = ConsolePanel()
    private let settingsStub = ComingSoonPanel(title: "Settings",
        detail: "Device appearance and accessibility controls (Appearance, Text Size, Reduce "
              + "Motion, ...) need new engine methods to read/set them.")
    private let reportStub = ComingSoonPanel(title: "Report",
        detail: "A diagnostics report view, matching Device Hub's Report tab.")

    /// Top-right icon row: Settings, Report, Info.
    private let iconTabs = NSSegmentedControl()
    /// Second row, under Info only: the text-named tabs.
    private let textTabs = NSSegmentedControl()

    private static let infoIndex = 2   // iconTabs segment showing the text-tab row

    /// The device whose details the Info tab should show.
    var udid: String? {
        didSet { diagnostics.udid = udid }
    }

    /// The engine connection the Apps/Profiles/Files tabs talk through. Console opens its own.
    var control: ControlClient? {
        didSet { apps.control = control; profiles.control = control; files.control = control }
    }

    /// The panes selectable through the second (text) row, in Device Hub's Info/Apps/Profiles
    /// order, then our folded-in extras.
    private var subPanes: [NSView] { [diagnostics, apps, profiles, files, console, controls] }
    private var allPanes: [NSView] { subPanes + [settingsStub, reportStub] }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        let icons = [("slider.horizontal.3", "Settings"), ("doc.text", "Report"), ("info.circle", "Info")]
        iconTabs.segmentCount = icons.count
        for (i, (symbol, label)) in icons.enumerated() {
            iconTabs.setImage(NSImage(systemSymbolName: symbol, accessibilityDescription: label),
                              forSegment: i)
            iconTabs.setToolTip(label, forSegment: i)
            iconTabs.setWidth(0, forSegment: i)     // 0 = size to fit
        }
        iconTabs.segmentStyle = .texturedRounded
        iconTabs.selectedSegment = Self.infoIndex
        iconTabs.target = self
        iconTabs.action = #selector(iconTabChanged)
        iconTabs.translatesAutoresizingMaskIntoConstraints = false

        let subNames = ["Info", "Apps", "Profiles", "Files", "Console", "Controls"]
        textTabs.segmentCount = subNames.count
        for (i, name) in subNames.enumerated() {
            textTabs.setLabel(name, forSegment: i)
            textTabs.setWidth(0, forSegment: i)
        }
        textTabs.segmentStyle = .texturedRounded
        textTabs.selectedSegment = 0
        textTabs.target = self
        textTabs.action = #selector(subTabChanged)
        textTabs.font = .systemFont(ofSize: 10)
        textTabs.translatesAutoresizingMaskIntoConstraints = false

        for v in allPanes {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        addSubview(iconTabs)
        addSubview(textTabs)

        // Device Hub keeps the icon row at the inspector's top-right, not centered.
        NSLayoutConstraint.activate([
            iconTabs.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            iconTabs.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            textTabs.topAnchor.constraint(equalTo: iconTabs.bottomAnchor, constant: 8),
            // Six folded-in sub-tabs are wider than Device Hub's real three, so this row is
            // pinned to the inspector's full width (not just centered) and left to compress --
            // unlike the icon row above, which always fits.
            textTabs.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            textTabs.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
        ])
        for v in allPanes {
            NSLayoutConstraint.activate([
                v.leadingAnchor.constraint(equalTo: leadingAnchor),
                v.trailingAnchor.constraint(equalTo: trailingAnchor),
                v.bottomAnchor.constraint(equalTo: bottomAnchor),
            ])
        }
        // The two stubs sit directly under the icon row (no text sub-tabs); the sub-panes sit
        // under the text row.
        for v in [settingsStub, reportStub] {
            NSLayoutConstraint.activate([v.topAnchor.constraint(equalTo: iconTabs.bottomAnchor, constant: 8)])
        }
        for v in subPanes {
            NSLayoutConstraint.activate([v.topAnchor.constraint(equalTo: textTabs.bottomAnchor, constant: 4)])
        }
        applySelection()
    }

    @objc private func iconTabChanged() {
        applySelection()
    }

    @objc private func subTabChanged() {
        applySelection()
    }

    private func applySelection() {
        let onInfo = iconTabs.selectedSegment == Self.infoIndex
        textTabs.isHidden = !onInfo
        settingsStub.isHidden = iconTabs.selectedSegment != 0
        reportStub.isHidden = iconTabs.selectedSegment != 1
        let sub = textTabs.selectedSegment
        for (i, v) in subPanes.enumerated() { v.isHidden = !onInfo || i != sub }
        guard onInfo else { return }
        // Fetch on first reveal rather than on every device change: the query opens a lockdown
        // session and takes a moment, and doing it for a tab nobody is looking at is waste.
        if sub == 0 { diagnostics.refresh() }
        if sub == 1 { apps.revealed() }
        if sub == 2 { profiles.revealed() }
        if sub == 3 { files.revealed() }
    }
}

/// A placeholder for a Device Hub tab we haven't wired an engine backend for yet.
private final class ComingSoonPanel: NSView {
    init(title: String, detail: String) {
        super.init(frame: .zero)
        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        let detailLabel = NSTextField(wrappingLabelWithString: detail)
        detailLabel.font = .systemFont(ofSize: 11)
        detailLabel.textColor = .secondaryLabelColor
        let stack = NSStackView(views: [titleLabel, detailLabel])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 10, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])
        for v in [self, stack, titleLabel, detailLabel] as [NSView] {
            v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            v.setContentHuggingPriority(.init(100), for: .horizontal)
        }
    }

    required init?(coder: NSCoder) { nil }
}
