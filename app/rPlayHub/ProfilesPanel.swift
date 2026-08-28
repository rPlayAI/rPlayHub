//
//  ProfilesPanel.swift
//  The inspector's Profiles tab: provisioning and configuration profiles on the device.
//
//  Device Hub's third inspector tab. Backed by the engine's list_profiles, which reads
//  provisioning profiles from misagent and configuration profiles from MCInstall.
//

import AppKit
import UniformTypeIdentifiers

final class ProfilesPanel: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private enum Kind { case provisioning, configuration }

    private struct Row {
        let title: String
        let detail: String
        let isHeader: Bool
        /// nil for header/"None" rows -- nothing to select or remove.
        let kind: Kind?
        let id: String?
    }

    var control: ControlClient? {
        didSet {
            guard control !== oldValue else { return }
            rows = []
            table.reloadData()
            loaded = false
            status.stringValue = control == nil ? "Not connected to the engine." : ""
            if control != nil, !isHidden { refresh() }
        }
    }

    private var rows: [Row] = []
    private var loaded = false
    private var loading = false
    private let table = NSTableView()
    private let status = NSTextField(labelWithString: "")
    private let refreshButton = NSButton()
    private let installButton = NSButton()
    private let removeButton = NSButton()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        let col = NSTableColumn(identifier: .init("profile"))
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = 34
        table.dataSource = self
        table.delegate = self
        table.usesAlternatingRowBackgroundColors = true

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false

        refreshButton.title = " Refresh"
        refreshButton.image = NSImage(systemSymbolName: "arrow.clockwise", accessibilityDescription: "Refresh")
        refreshButton.imagePosition = .imageLeading
        refreshButton.bezelStyle = .rounded
        refreshButton.controlSize = .small
        refreshButton.target = self
        refreshButton.action = #selector(refresh)

        // Device Hub's Profiles `+`/`-`: install a provisioning/configuration profile, remove
        // the selected one.
        installButton.title = "+"
        installButton.bezelStyle = .rounded
        installButton.controlSize = .small
        installButton.target = self
        installButton.action = #selector(install)
        removeButton.title = "–"
        removeButton.bezelStyle = .rounded
        removeButton.controlSize = .small
        removeButton.target = self
        removeButton.action = #selector(remove)
        removeButton.isEnabled = false

        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3

        let buttons = NSStackView(views: [installButton, removeButton, NSView(), refreshButton])
        buttons.orientation = .horizontal
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
        // Same reason as the other panels: the inspector's width is held at priority 700.
        for v in [self, stack, buttons, status, scroll, refreshButton, installButton, removeButton] as [NSView] {
            v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            v.setContentHuggingPriority(.init(100), for: .horizontal)
        }
        status.stringValue = "Select a device to list its profiles."
    }

    func revealed() {
        if !loaded { refresh() }
    }

    @objc func refresh() {
        guard let control else { status.stringValue = "Not connected to the engine."; return }
        guard !loading else { return }
        loading = true
        status.stringValue = "Listing profiles…"
        control.listProfiles { [weak self] result in
            guard let self else { return }
            self.loading = false
            switch result {
            case .success(let r):
                let prov = r["provisioning"] as? [[String: Any]] ?? []
                let conf = r["configuration"] as? [[String: Any]] ?? []
                var rows: [Row] = []
                rows.append(Row(title: "Provisioning Profiles", detail: "", isHeader: true, kind: nil, id: nil))
                if prov.isEmpty { rows.append(Row(title: "None", detail: "", isHeader: false, kind: nil, id: nil)) }
                for p in prov {
                    let name = p["name"] as? String ?? p["uuid"] as? String ?? "?"
                    var parts: [String] = []
                    if let t = p["team"] as? String, !t.isEmpty { parts.append(t) }
                    if let e = p["expires"] as? String, let d = Self.iso.date(from: e) {
                        parts.append((d < Date() ? "expired " : "expires ") + Self.short.string(from: d))
                    }
                    if let n = p["devices"] as? Int, n > 0 { parts.append("\(n) devices") }
                    rows.append(Row(title: name, detail: parts.joined(separator: " · "), isHeader: false,
                                     kind: .provisioning, id: p["uuid"] as? String))
                }
                rows.append(Row(title: "Configuration Profiles", detail: "", isHeader: true, kind: nil, id: nil))
                if conf.isEmpty { rows.append(Row(title: "None", detail: "", isHeader: false, kind: nil, id: nil)) }
                for c in conf {
                    let name = c["name"] as? String ?? c["identifier"] as? String ?? "?"
                    var parts: [String] = []
                    if let o = c["organization"] as? String, !o.isEmpty { parts.append(o) }
                    if let i = c["identifier"] as? String { parts.append(i) }
                    rows.append(Row(title: name, detail: parts.joined(separator: " · "), isHeader: false,
                                     kind: .configuration, id: c["identifier"] as? String))
                }
                self.rows = rows
                self.loaded = true
                self.table.reloadData()
                self.status.stringValue = "\(prov.count) provisioning, \(conf.count) configuration"
                    + ((r["error"] as? String).map { " · \($0)" } ?? "")
            case .failure(let e):
                self.status.stringValue = "Could not list profiles: \(e)"
            }
        }
    }

    /// Opens a file picker for a `.mobileprovision`/`.mobileconfig` and installs it via
    /// misagent/MCInstall (the engine dispatches on extension). Device Hub's Profiles `+`.
    @objc private func install() {
        guard let control else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = ["mobileprovision", "mobileconfig"]
            .compactMap { UTType(filenameExtension: $0) }
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        status.stringValue = "Installing \(url.lastPathComponent)…"
        control.installProfile(path: url.path) { [weak self] result in
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

    private var selectedRow: Row? {
        let r = table.selectedRow
        return r >= 0 && r < rows.count ? rows[r] : nil
    }

    /// Removes the selected profile, after confirming. Device Hub's Profiles `-`.
    @objc private func remove() {
        guard let control, let row = selectedRow, let kind = row.kind, let id = row.id else { return }
        let alert = NSAlert()
        alert.messageText = "Remove \"\(row.title)\"?"
        alert.addButton(withTitle: "Remove")
        alert.addButton(withTitle: "Cancel")
        alert.buttons.first?.hasDestructiveAction = true
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        status.stringValue = "Removing \(row.title)…"
        let completion: (Result<[String: Any], Error>) -> Void = { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                self.status.stringValue = "Removed \(row.title)."
                self.refresh()
            case .failure(let e):
                self.status.stringValue = "Remove failed: \(e)"
            }
        }
        switch kind {
        case .provisioning:  control.removeProvisioningProfile(uuid: id, completion: completion)
        case .configuration: control.removeConfigurationProfile(identifier: id, completion: completion)
        }
    }

    private static let iso: ISO8601DateFormatter = ISO8601DateFormatter()
    private static let short: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .none
        return f
    }()

    // MARK: - table

    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }

    func tableView(_ tableView: NSTableView, heightOfRow row: Int) -> CGFloat {
        rows[row].isHeader ? 22 : 34
    }

    func tableView(_ tableView: NSTableView, isGroupRow row: Int) -> Bool { rows[row].isHeader }

    /// Only rows with a removable id (a real profile, not a header or "None" placeholder).
    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool { rows[row].id != nil }

    func tableViewSelectionDidChange(_ notification: Notification) {
        removeButton.isEnabled = selectedRow != nil
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let r = rows[row]
        let id = NSUserInterfaceItemIdentifier(r.isHeader ? "header" : "cell")
        let cell = tableView.makeView(withIdentifier: id, owner: nil) as? Cell ?? Cell(id, header: r.isHeader)
        cell.title.stringValue = r.title
        cell.detail.stringValue = r.detail
        cell.detail.isHidden = r.detail.isEmpty
        return cell
    }

    private final class Cell: NSTableCellView {
        let title = NSTextField(labelWithString: "")
        let detail = NSTextField(labelWithString: "")

        init(_ id: NSUserInterfaceItemIdentifier, header: Bool) {
            super.init(frame: .zero)
            identifier = id
            title.font = header ? .systemFont(ofSize: 11, weight: .semibold) : .systemFont(ofSize: 12, weight: .medium)
            title.textColor = header ? .secondaryLabelColor : .labelColor
            title.lineBreakMode = .byTruncatingTail
            detail.font = .systemFont(ofSize: 10)
            detail.textColor = .secondaryLabelColor
            detail.lineBreakMode = .byTruncatingMiddle
            let stack = NSStackView(views: [title, detail])
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
            for v in [title, detail] {
                v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            }
        }

        required init?(coder: NSCoder) { nil }
    }
}
