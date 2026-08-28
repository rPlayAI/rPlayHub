//
//  AppsPanel.swift
//  The inspector's Apps tab: what is installed, and Launch / Terminate.
//
//  Backed by the engine's list_apps / launch_app / terminate_app / list_processes, which speak
//  coredevice.appservice -- the same service Xcode's devicectl uses. The list is the one Device
//  Hub shows: user-installed apps plus Apple's own; hidden and internal ones stay out.
//

import AppKit
import UniformTypeIdentifiers

final class AppsPanel: NSView, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    struct App {
        let bundleID: String
        let name: String
        let version: String
        let isFirstParty: Bool
        /// get-task-allow out of the app's Entitlements: true for a development-signed build
        /// (what Xcode installs), false for an App Store or ad-hoc one. Device Hub's "Developer"
        /// filter is this, not merely "not an Apple app" -- isFirstParty alone matched every
        /// third-party app, App Store installs included.
        let isDeveloper: Bool
        let isAppClip: Bool
    }

    /// Set by the owner whenever the engine connection changes.
    var control: ControlClient? {
        didSet {
            guard control !== oldValue else { return }
            // A new connection usually means a different device (the engine re-executes to
            // switch), so the old list is wrong, not stale. Refetch now if the tab is showing;
            // otherwise on its next reveal.
            apps = []
            reloadTable()
            loaded = false
            status.stringValue = control == nil ? "Not connected to the engine." : ""
            if control != nil, !isHidden { refresh() }
        }
    }

    private var apps: [App] = []
    private var loaded = false
    private var loading = false
    private let table = NSTableView()
    private let status = NSTextField(labelWithString: "")
    private let launchButton = NSButton()
    private let killButton = NSButton()
    private let refreshButton = NSButton()
    private let installButton = NSButton()
    private let uninstallButton = NSButton()
    private let filterField = NSSearchField()
    private let categoryPopup = NSPopUpButton()
    /// "No App Clips" etc. -- what Device Hub shows in place of an empty list, confirmed against
    /// the live app (its App Clips category, empty on this device, reads exactly this way).
    private let placeholder = NSTextField(labelWithString: "")

    /// Apps passing the current filter text + category, in the order the table shows them.
    private var filtered: [App] {
        let q = filterField.stringValue.lowercased()
        return apps.filter { a in
            if !q.isEmpty && !a.name.lowercased().contains(q) && !a.bundleID.lowercased().contains(q) {
                return false
            }
            switch categoryPopup.indexOfSelectedItem {
            case 2:  return a.isFirstParty       // Default    (Apple's own)
            case 3:  return a.isAppClip          // App Clips
            case 4:  return a.isDeveloper        // Developer  (get-task-allow, not just 3rd-party)
            default: return true                 // 0 = All Apps (index 1 is the separator)
            }
        }
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
        let col = NSTableColumn(identifier: .init("app"))
        col.title = "App"
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = 34
        table.dataSource = self
        table.delegate = self
        table.doubleAction = #selector(launch)
        table.target = self
        table.usesAlternatingRowBackgroundColors = true
        // The thin grey line between rows Device Hub's list has.
        table.gridStyleMask = .solidHorizontalGridLineMask
        table.gridColor = .separatorColor

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false

        func button(_ b: NSButton, _ title: String, _ symbol: String, _ sel: Selector) {
            b.title = " " + title
            b.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            b.imagePosition = .imageLeading
            b.bezelStyle = .rounded
            b.controlSize = .small
            b.target = self
            b.action = sel
        }
        button(refreshButton, "Refresh", "arrow.clockwise", #selector(refresh))
        button(launchButton, "Launch", "play.fill", #selector(launch))
        button(killButton, "Terminate", "xmark.octagon", #selector(terminate))
        launchButton.isEnabled = false
        killButton.isEnabled = false

        // Device Hub's Apps `+`/`-`: install an .ipa, uninstall the selection.
        installButton.title = "+"
        installButton.bezelStyle = .rounded
        installButton.controlSize = .small
        installButton.target = self
        installButton.action = #selector(install)
        uninstallButton.title = "–"
        uninstallButton.bezelStyle = .rounded
        uninstallButton.controlSize = .small
        uninstallButton.target = self
        uninstallButton.action = #selector(uninstall)
        uninstallButton.isEnabled = false

        // Filter row: search field + category popup. Device Hub pins this at the very bottom of
        // the Apps tab, below the list and the +/- row, not above the list.
        filterField.placeholderString = "Filter"
        filterField.controlSize = .small
        filterField.delegate = self                       // controlTextDidChange -> live filter
        categoryPopup.controlSize = .small
        categoryPopup.addItem(withTitle: "All Apps")
        categoryPopup.menu?.addItem(.separator())
        categoryPopup.addItem(withTitle: "App Clips")
        categoryPopup.addItem(withTitle: "Default")
        categoryPopup.addItem(withTitle: "Developer")
        placeholder.font = .systemFont(ofSize: 12)
        placeholder.textColor = .secondaryLabelColor
        placeholder.alignment = .center
        placeholder.isHidden = true
        placeholder.translatesAutoresizingMaskIntoConstraints = false

        categoryPopup.target = self
        categoryPopup.action = #selector(applyFilter)

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3

        let buttons = NSStackView(views: [installButton, uninstallButton, launchButton, killButton,
                                          NSView(), refreshButton])
        buttons.orientation = .horizontal
        buttons.spacing = 6
        let filterRow = NSStackView(views: [filterField, categoryPopup])
        filterRow.orientation = .horizontal
        filterRow.spacing = 6
        filterField.setContentHuggingPriority(.init(1), for: .horizontal)   // field grows, popup fixed
        // The hairline Device Hub draws above its bottom Filter row.
        let divider = NSBox()
        divider.boxType = .separator
        let stack = NSStackView(views: [scroll, buttons, status, divider, filterRow])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 10, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        addSubview(placeholder)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -20),
            filterRow.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            buttons.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            status.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            divider.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            placeholder.centerXAnchor.constraint(equalTo: scroll.centerXAnchor),
            placeholder.centerYAnchor.constraint(equalTo: scroll.centerYAnchor),
        ])
        // The inspector holds its 260-point width at priority 700; anything in here that resists
        // compression at the default 750 would win and grow the pane across the window, pushing
        // the screen out. Everything yields instead and truncates or scrolls.
        for v in [self, stack, buttons, status, scroll, filterRow, filterField, categoryPopup, placeholder]
                 + buttons.arrangedSubviews {
            v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            v.setContentHuggingPriority(.init(100), for: .horizontal)
        }
        status.stringValue = "Select a device to list its apps."
    }

    /// Fetch on first reveal; the list is a few hundred entries over the tunnel.
    func revealed() {
        if !loaded { refresh() }
    }

    @objc func refresh() {
        guard let control else { status.stringValue = "Not connected to the engine."; return }
        guard !loading else { return }
        loading = true
        status.stringValue = "Listing apps…"
        control.listApps { [weak self] result in
            guard let self else { return }
            self.loading = false
            switch result {
            case .success(let list):
                self.apps = list.compactMap { d in
                    guard let bid = d["bundleIdentifier"] as? String else { return nil }
                    return App(bundleID: bid,
                               name: d["name"] as? String ?? bid,
                               version: d["version"] as? String ?? "",
                               isFirstParty: d["isFirstParty"] as? Bool ?? false,
                               isDeveloper: d["isDeveloper"] as? Bool ?? false,
                               isAppClip: d["isAppClip"] as? Bool ?? false)
                }.sorted { ($0.isFirstParty ? 1 : 0, $0.name.lowercased())
                         < ($1.isFirstParty ? 1 : 0, $1.name.lowercased()) }
                self.loaded = true
                self.reloadTable()
                let third = self.apps.filter { !$0.isFirstParty }.count
                self.status.stringValue = "\(self.apps.count) apps (\(third) installed by you). "
                    + "Double-click to launch."
            case .failure(let e):
                self.status.stringValue = "Could not list apps: \(e)"
            }
        }
    }

    private var selected: App? {
        let f = filtered
        let r = table.selectedRow
        return r >= 0 && r < f.count ? f[r] : nil
    }

    @objc private func applyFilter() { reloadTable() }
    func controlTextDidChange(_ obj: Notification) { reloadTable() }

    /// Reloads the list and shows Device Hub's "No {category}" placeholder when the filter
    /// leaves nothing to show -- confirmed against the live app, whose App Clips category (empty
    /// on the test device) reads exactly this way rather than a blank list.
    private func reloadTable() {
        table.reloadData()
        let f = filtered
        placeholder.isHidden = !f.isEmpty
        guard f.isEmpty else { return }
        let category = categoryPopup.indexOfSelectedItem >= 0
            ? categoryPopup.itemTitle(at: categoryPopup.indexOfSelectedItem) : "Apps"
        placeholder.stringValue = "No \(category == "All Apps" ? "Apps" : category)"
    }

    /// Opens a file picker for an `.ipa`, stages it into /PublicStaging over AFC and installs it
    /// via installation_proxy. Device Hub's Apps `+`.
    @objc private func install() {
        guard let control else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "ipa")].compactMap { $0 }
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        status.stringValue = "Installing \(url.lastPathComponent)…"
        control.installApp(path: url.path) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                self.status.stringValue = "Installed \(url.lastPathComponent)."
                self.refresh()
            case .failure(let e):
                self.status.stringValue = "Install failed: \(e)"
            }
        }
    }

    /// Uninstalls the selected app, after confirming -- this removes the app and its data from
    /// the device. Device Hub's Apps `-`.
    @objc private func uninstall() {
        guard let control, let app = selected else { return }
        let alert = NSAlert()
        alert.messageText = "Uninstall \"\(app.name)\"?"
        alert.informativeText = "This removes the app and its data from the device."
        alert.addButton(withTitle: "Uninstall")
        alert.addButton(withTitle: "Cancel")
        alert.buttons.first?.hasDestructiveAction = true
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        status.stringValue = "Uninstalling \(app.name)…"
        control.uninstallApp(bundleID: app.bundleID) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                self.status.stringValue = "Uninstalled \(app.name)."
                self.refresh()
            case .failure(let e):
                self.status.stringValue = "Uninstall failed: \(e)"
            }
        }
    }

    @objc private func launch() {
        guard let control, let app = selected else { return }
        status.stringValue = "Launching \(app.name)…"
        control.launchApp(app.bundleID) { [weak self] result in
            switch result {
            case .success(let r):
                let token = r["processToken"] as? [String: Any] ?? r
                let pid = (token["processIdentifier"] as? NSNumber)?.intValue ?? 0
                self?.status.stringValue = pid > 0 ? "\(app.name) is running (pid \(pid))."
                                                   : "\(app.name) launched."
            case .failure(let e):
                self?.status.stringValue = "Launch failed: \(e)"
            }
        }
    }

    /// Terminate needs a pid, and the app list carries none, so look the running process up
    /// first. listprocesses reports executable URLs (`file:///Applications/Preferences.app/...`);
    /// match the `.app` bundle directory named after the app, or the bundle id in the path.
    @objc private func terminate() {
        guard let control, let app = selected else { return }
        status.stringValue = "Finding \(app.name)'s process…"
        control.listProcesses { [weak self] result in
            guard let self else { return }
            switch result {
            case .failure(let e):
                self.status.stringValue = "Could not list processes: \(e)"
            case .success(let procs):
                let pid = procs.first { p in
                    let url = p["executableURL"] as? [String: Any]
                    let exe = (url?["relative"] as? String ?? p["executable"] as? String ?? "")
                        .removingPercentEncoding ?? ""
                    return exe.contains("/\(app.name).app/") || exe.contains("/\(app.bundleID)/")
                }.flatMap { ($0["processIdentifier"] as? NSNumber)?.intValue }
                guard let pid else {
                    self.status.stringValue = "\(app.name) is not running."
                    return
                }
                control.terminateApp(pid: pid) { [weak self] r in
                    switch r {
                    case .success: self?.status.stringValue = "Terminated \(app.name) (pid \(pid))."
                    case .failure(let e): self?.status.stringValue = "Terminate failed: \(e)"
                    }
                }
            }
        }
    }

    // MARK: - table

    func numberOfRows(in tableView: NSTableView) -> Int { filtered.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let app = filtered[row]
        let id = NSUserInterfaceItemIdentifier("cell")
        let cell = tableView.makeView(withIdentifier: id, owner: nil) as? AppCell ?? AppCell(id)
        cell.name.stringValue = app.name
        cell.detail.stringValue = app.version.isEmpty ? app.bundleID : "\(app.bundleID) · \(app.version)"
        return cell
    }

    /// Two lines per row, as Device Hub lays its apps out. The fields are properties rather
    /// than looked up through `subviews`: they sit inside a stack view, and a lookup that
    /// missed them crashed the first live run.
    private final class AppCell: NSTableCellView {
        let name = NSTextField(labelWithString: "")
        let detail = NSTextField(labelWithString: "")

        init(_ id: NSUserInterfaceItemIdentifier) {
            super.init(frame: .zero)
            identifier = id
            name.font = .systemFont(ofSize: 12, weight: .medium)
            name.lineBreakMode = .byTruncatingTail
            detail.font = .systemFont(ofSize: 10)
            detail.textColor = .secondaryLabelColor
            detail.lineBreakMode = .byTruncatingMiddle
            let stack = NSStackView(views: [name, detail])
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 1
            stack.translatesAutoresizingMaskIntoConstraints = false
            addSubview(stack)
            NSLayoutConstraint.activate([
                stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
                stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
                stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            ])
            for v in [name, detail] {
                v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            }
        }

        required init?(coder: NSCoder) { nil }
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        let has = selected != nil
        launchButton.isEnabled = has
        killButton.isEnabled = has
        uninstallButton.isEnabled = has
    }
}
