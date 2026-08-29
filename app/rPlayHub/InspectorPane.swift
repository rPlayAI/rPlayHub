//
//  InspectorPane.swift
//  The right-hand pane and its tabs.
//
//  Device Hub's inspector has two levels: a row of 3 ICON tabs (Settings, Report, Info) sitting
//  IN THE WINDOW'S TITLE BAR at the trailing edge -- same row as the traffic lights, not inside
//  the content area -- and under Info, a row of TEXT-named tabs (Info, Apps, Profiles). This has
//  those three plus Controls, Files and Console, which Device Hub has no equivalent for --
//  folded in as extra text sub-tabs alongside Info/Apps/Profiles rather than as their own icon
//  tabs, so the top row stays a faithful 3 icons. Settings and Report need engine methods we
//  don't have yet (device appearance/accessibility, a diagnostics report) and are stubbed until
//  then.
//
//  `iconTabs` is built here (it drives this pane's selection) but AppDelegate lifts it into a
//  toolbar item instead of adding it as a subview -- see buildToolbar() there.
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
    /// Device Hub's Settings tab -- real device appearance/accessibility switches, over
    /// coredevice.configuration (doc/COREDEVICE-ACTIONS.md).
    let settings = SettingsPanel()
    private let reportStub = ComingSoonPanel(title: "Report",
        detail: "A diagnostics report view, matching Device Hub's Report tab.")

    /// Settings, Report, Info -- lives in the window's title bar, not in this view. Exposed so
    /// AppDelegate can put it in a toolbar item. Hand-rolled (see IconTabBar) because a
    /// segmented control paints its selection with the accent colour and Device Hub's is grey.
    let iconTabs = IconTabBar(icons: [("slider.horizontal.3", "Settings"),
                                      ("doc.text", "Report"),
                                      ("info", "Info")])
    /// Second row, under Info only: the text-named tabs.
    private let textTabs = NSSegmentedControl()

    private static let infoIndex = 2   // iconTabs segment showing the text-tab row
    /// Which of the 3 icon segments is active, kept even while the pane is hidden so a re-show
    /// (clicking any icon) restores the last tab rather than always resetting to Info.
    private var activeIcon = infoIndex

    /// The device whose details the Info tab should show.
    var udid: String? {
        didSet { diagnostics.udid = udid }
    }

    /// The engine connection the Apps/Profiles/Files tabs talk through. Console opens its own.
    var control: ControlClient? {
        didSet { apps.control = control; profiles.control = control; files.control = control
                 settings.control = control }
    }

    /// The panes selectable through the second (text) row, in Device Hub's Info/Apps/Profiles
    /// order, then our folded-in extras.
    private var subPanes: [NSView] { [diagnostics, apps, profiles, files, console, controls] }
    private var allPanes: [NSView] { subPanes + [settings, reportStub] }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    /// Clicking anywhere in this pane -- including the empty space below its content -- takes
    /// focus, which is what turns the canvas's View Screen highlight off. Clicks that land on a
    /// control or table row reach that subview first and it claims focus itself; this only
    /// catches the dead space those miss.
    override var acceptsFirstResponder: Bool { true }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        super.mouseDown(with: event)
    }

    private func build() {
        // #FAFAFA, re-sampled off three separate Device Hub captures, which all agree. Only the
        // canvas in the middle is pure white; both side panes are this near-white.
        //
        // An earlier pass recorded #E4E4E4 here as "sampled off the live window" and it was
        // simply wrong -- far too dark, and wrong in the other direction too: Device Hub's lists
        // and grouped rows are #F3F3F2, a touch DARKER than the pane they sit on, where ours had
        // them lighter than it. Measured, not adjusted by eye.
        wantsLayer = true
        layer?.backgroundColor = NSColor(srgbRed: 0xFA / 255, green: 0xFA / 255, blue: 0xFA / 255,
                                         alpha: 1).cgColor

        iconTabs.selected = Self.infoIndex
        iconTabs.onSelect = { [weak self] index in self?.iconTabClicked(index) }

        let subNames = ["Info", "Apps", "Profiles", "Files", "Console", "Controls"]
        textTabs.segmentCount = subNames.count
        for (i, name) in subNames.enumerated() {
            textTabs.setLabel(name, forSegment: i)
        }
        textTabs.segmentDistribution = .fillEqually
        textTabs.segmentStyle = .texturedRounded
        textTabs.selectedSegment = 0
        textTabs.target = self
        textTabs.action = #selector(subTabChanged)
        textTabs.font = .systemFont(ofSize: 11)
        textTabs.translatesAutoresizingMaskIntoConstraints = false

        for v in allPanes {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        addSubview(textTabs)

        // iconTabs is NOT a subview here -- AppDelegate hosts it in the title bar toolbar
        // instead, so textTabs anchors directly to this pane's own top.
        //
        // Width is an explicit constraint tied to this pane, not a leading+trailing pin: with
        // six folded-in sub-tabs (Device Hub's real inspector has three) `.texturedRounded`'s
        // intrinsic content size ran wider than the pane and, pinned by two required edges, that
        // intrinsic size won against the pane's own (lower-priority) resting width -- growing the
        // whole inspector and stealing space from the canvas next to it, not just this control.
        // Deriving the width from the pane instead removes intrinsic size from the fight entirely.
        NSLayoutConstraint.activate([
            textTabs.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            textTabs.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            textTabs.widthAnchor.constraint(equalTo: widthAnchor, constant: -8),
        ])
        for v in allPanes {
            NSLayoutConstraint.activate([
                v.leadingAnchor.constraint(equalTo: leadingAnchor),
                v.trailingAnchor.constraint(equalTo: trailingAnchor),
                v.bottomAnchor.constraint(equalTo: bottomAnchor),
            ])
        }
        // The two stubs sit directly under this pane's top (no text sub-tabs); the sub-panes
        // sit under the text row.
        for v in [settings, reportStub] {
            NSLayoutConstraint.activate([v.topAnchor.constraint(equalTo: topAnchor, constant: 8)])
        }
        for v in subPanes {
            NSLayoutConstraint.activate([v.topAnchor.constraint(equalTo: textTabs.bottomAnchor, constant: 4)])
        }
        applySelection()
    }

    /// Shows or hides this pane, keeping the icon row's selection in sync -- used by the Device
    /// menu's Show/Hide Controls command, which toggles the same visibility a re-click of the
    /// active icon does.
    func setHidden(_ hidden: Bool) {
        isHidden = hidden
        iconTabs.selected = hidden ? nil : activeIcon
        applySelection()
    }

    /// Re-clicking the already-active icon collapses this pane instead of just reselecting it --
    /// Device Hub's Settings/Report/Info icons are also the show/hide toggle for the whole
    /// panel, with no separate dedicated button for it.
    /// Device Hub's behaviour, confirmed against the live app: clicking the tab that is already
    /// active collapses the whole inspector (and no icon shows lit); clicking any icon while
    /// collapsed reopens it on that tab.
    private func iconTabClicked(_ clicked: Int) {
        if clicked == activeIcon && !isHidden {
            isHidden = true
        } else {
            activeIcon = clicked
            isHidden = false
        }
        iconTabs.selected = isHidden ? nil : activeIcon
        applySelection()
    }

    @objc private func subTabChanged() {
        applySelection()
    }

    private func applySelection() {
        let onInfo = !isHidden && activeIcon == Self.infoIndex
        textTabs.isHidden = !onInfo
        settings.isHidden = isHidden || activeIcon != 0
        reportStub.isHidden = isHidden || activeIcon != 1
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
