//
//  AppsPanel.swift
//  The inspector's Apps tab: what is installed, and Launch / Terminate.
//
//  Backed by the engine's list_apps / launch_app / terminate_app / list_processes, which speak
//  coredevice.appservice -- the same service Xcode's devicectl uses. The list is the one Device
//  Hub shows: user-installed apps plus Apple's own; hidden and internal ones stay out.
//

import AppKit

final class AppsPanel: NSView, NSTableViewDataSource, NSTableViewDelegate {
    struct App {
        let bundleID: String
        let name: String
        let version: String
        let isFirstParty: Bool
    }

    /// Set by the owner whenever the engine connection changes.
    var control: ControlClient? {
        didSet {
            guard control !== oldValue else { return }
            // A new connection usually means a different device (the engine re-executes to
            // switch), so the old list is wrong, not stale. Refetch now if the tab is showing;
            // otherwise on its next reveal.
            apps = []
            table.reloadData()
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

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3

        let buttons = NSStackView(views: [launchButton, killButton, NSView(), refreshButton])
        buttons.orientation = .horizontal
        buttons.spacing = 6
        let stack = NSStackView(views: [scroll, buttons, status])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 8
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 10, right: 10)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -20),
            buttons.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            status.widthAnchor.constraint(equalTo: scroll.widthAnchor),
        ])
        // The inspector holds its 260-point width at priority 700; anything in here that resists
        // compression at the default 750 would win and grow the pane across the window, pushing
        // the screen out. Everything yields instead and truncates or scrolls.
        for v in [self, stack, buttons, status, scroll] + buttons.arrangedSubviews {
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
                               isFirstParty: d["isFirstParty"] as? Bool ?? false)
                }.sorted { ($0.isFirstParty ? 1 : 0, $0.name.lowercased())
                         < ($1.isFirstParty ? 1 : 0, $1.name.lowercased()) }
                self.loaded = true
                self.table.reloadData()
                let third = self.apps.filter { !$0.isFirstParty }.count
                self.status.stringValue = "\(self.apps.count) apps (\(third) installed by you). "
                    + "Double-click to launch."
            case .failure(let e):
                self.status.stringValue = "Could not list apps: \(e)"
            }
        }
    }

    private var selected: App? {
        let r = table.selectedRow
        return r >= 0 && r < apps.count ? apps[r] : nil
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

    func numberOfRows(in tableView: NSTableView) -> Int { apps.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let app = apps[row]
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
    }
}
