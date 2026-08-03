//
//  DeviceSidebar.swift
//  The left-hand device list, in the shape of Apple's Device Hub.
//
//  The engine is single-session today, so this usually shows one device. It is built for a list
//  anyway because the `adb`-shaped multi-device server is the direction of travel, and a sidebar
//  retrofitted onto a single-device assumption is worse than one that always had rows.
//

import AppKit

struct DeviceRow {
    let id: String
    let name: String          // "iPhone13"
    var detail: String        // "iPhone 13 Pro" — the model, as Device Hub shows it
    let version: String       // "27.0", right-aligned like Device Hub
    let connected: Bool
    let udid: String
    /// Raw identifier, kept so the row can pick a matching glyph.
    var productType: String?
    /// Simulators share the list with real devices, as they do in Device Hub, but almost every
    /// action differs -- there is no tunnel, no pairing, and booting is ours to do.
    var isSimulator = false

    /// A simulator row. `connected` doubles as "booted", which is the state that matters: a
    /// shutdown simulator can be listed but not looked at.
    init(simulator s: Simulator) {
        udid = s.udid
        id = s.udid
        name = s.name
        detail = "Simulator"
        version = s.runtime
        connected = s.isBooted
        productType = nil
        isSimulator = true
    }

    init(json: [String: Any]) {
        udid = json["udid"] as? String ?? json["id"] as? String ?? "?"
        id = json["id"] as? String ?? udid
        name = (json["name"] as? String) ?? (json["product_type"] as? String) ?? "iPhone"
        // Accept either spelling: mirror.py says os_version, cdhost historically said
        // product_version. Reading only one meant the sidebar showed nothing for one engine.
        version = (json["os_version"] as? String)
            ?? (json["product_version"] as? String)
            ?? "?"
        // Device Hub shows the marketing name here ("iPhone 13 Pro"), not the identifier and not
        // the transport. When the engine reports no product type at all the row would read "?",
        // so fall back to the device class rather than a question mark.
        productType = json["product_type"] as? String
        detail = DeviceModel.name(for: productType)
            ?? (json["device_class"] as? String)
            ?? (json["transport"] as? String)
            ?? ""

        connected = json["connected"] as? Bool ?? true
    }
}

final class DeviceSidebar: NSView {
    private let scroll = NSScrollView()
    private let table = NSTableView()
    private let search = NSSearchField()
    private let sectionLabel = NSTextField(labelWithString: "Available")
    private var allRows: [DeviceRow] = []
    private var rows: [DeviceRow] = []
    /// True while `update` is repopulating, so restored selections are not read as user clicks.
    private var reloading = false

    /// Called when the selection changes. The engine only serves one device today, so this is
    /// wired up but does not yet switch streams.
    var onSelect: ((DeviceRow) -> Void)?

    /// Right-click actions, keyed by the identifiers in `Command`. The same handlers back the
    /// toolbar, so there is one implementation per action rather than two.
    var onCommand: ((Command, DeviceRow?) -> Void)?

    enum Command: String {
        case openInNewTab
        case openInNewWindow
        case screenshot
        case record
        case home
        case rotate
        case pin
        case copyUDID
        case reconnect
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
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("device"))
        column.width = 220
        column.minWidth = 120
        column.resizingMask = [.autoresizingMask]
        table.addTableColumn(column)
        table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        table.headerView = nil
        table.rowHeight = 44
        table.style = .sourceList          // the standard sidebar look
        table.dataSource = self
        table.delegate = self
        table.selectionHighlightStyle = .regular

        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false

        search.placeholderString = "Search"
        search.sendsSearchStringImmediately = true
        search.target = self
        search.action = #selector(searchChanged)

        sectionLabel.font = .systemFont(ofSize: 11, weight: .semibold)
        sectionLabel.textColor = .secondaryLabelColor

        for v in [search, sectionLabel, scroll] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        NSLayoutConstraint.activate([
            search.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            search.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            search.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),

            sectionLabel.topAnchor.constraint(equalTo: search.bottomAnchor, constant: 10),
            sectionLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),

            scroll.topAnchor.constraint(equalTo: sectionLabel.bottomAnchor, constant: 4),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])

        buildContextMenu()
    }

    private func buildContextMenu() {
        let menu = NSMenu()
        func add(_ title: String, _ command: Command) {
            let item = NSMenuItem(title: title, action: #selector(contextAction(_:)),
                                  keyEquivalent: "")
            item.target = self
            item.representedObject = command.rawValue
            menu.addItem(item)
        }
        add("Open in New Tab", .openInNewTab)
        add("Open in New Window", .openInNewWindow)
        menu.addItem(.separator())
        add("Take Screenshot", .screenshot)
        add("Start / Stop Recording", .record)
        add("Press Home", .home)
        menu.addItem(.separator())
        add("Pin Window on Top", .pin)
        menu.addItem(.separator())
        add("Copy UDID", .copyUDID)
        add("Reconnect", .reconnect)
        // menu(for:) would be the modern hook, but a menu on the table gives us right-click on
        // rows for free, including the clicked-row semantics below.
        table.menu = menu
    }

    @objc private func contextAction(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String,
              let command = Command(rawValue: raw) else { return }
        // Act on the row that was right-clicked, which is not necessarily the selected one.
        let clicked = table.clickedRow
        let device = (clicked >= 0 && clicked < rows.count) ? rows[clicked] : selectedRow()
        onCommand?(command, device)
    }

    @objc private func searchChanged() { applyFilter() }

    private func applyFilter() {
        let q = search.stringValue.trimmingCharacters(in: .whitespaces).lowercased()
        let previous = selectedRow()?.udid
        rows = q.isEmpty ? allRows : allRows.filter {
            $0.name.lowercased().contains(q) || $0.detail.lowercased().contains(q)
                || $0.udid.lowercased().contains(q)
        }
        table.reloadData()
        if let previous, let i = rows.firstIndex(where: { $0.udid == previous }) {
            table.selectRowIndexes([i], byExtendingSelection: false)
        } else if !rows.isEmpty, table.selectedRow < 0 {
            table.selectRowIndexes([0], byExtendingSelection: false)
        }
    }

    func update(_ devices: [DeviceRow]) {
        let previous = selectedRow()?.udid
        allRows = devices
        rows = devices
        // Restoring the selection after a reload fires the same delegate a click does, and the
        // list refreshes on a timer -- so without this the app asked the daemon to rebind every
        // refresh, on a device nobody chose. Selecting a device restarts the daemon, so that is a
        // reboot loop, not a wasted call.
        reloading = true
        defer { reloading = false }
        table.reloadData()
        // Keep the selection on the same device across refreshes; otherwise select the first.
        if let previous, let i = rows.firstIndex(where: { $0.udid == previous }) {
            table.selectRowIndexes([i], byExtendingSelection: false)
        } else if !rows.isEmpty, table.selectedRow < 0 {
            table.selectRowIndexes([0], byExtendingSelection: false)
        }
    }

    /// Fill in a row's subtitle once lockdown has told us what the device actually is. The
    /// engine's device list carries no product type, so the sidebar starts without one.
    func setModel(_ productType: String, forUDID udid: String) {
        func patch(_ list: inout [DeviceRow]) {
            for i in list.indices where list[i].udid == udid {
                list[i].productType = productType
                list[i].detail = DeviceModel.name(for: productType) ?? list[i].detail
            }
        }
        patch(&allRows)
        patch(&rows)
        table.reloadData()
    }

    func selectedRow() -> DeviceRow? {
        let i = table.selectedRow
        return (i >= 0 && i < rows.count) ? rows[i] : nil
    }
}

extension DeviceSidebar: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int { rows.isEmpty ? 1 : rows.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?,
                   row: Int) -> NSView? {
        let cell = NSTableCellView()

        let title = NSTextField(labelWithString: "")
        let subtitle = NSTextField(labelWithString: "")
        title.font = .systemFont(ofSize: 13, weight: .medium)
        subtitle.font = .systemFont(ofSize: 11)
        subtitle.textColor = .secondaryLabelColor

        let icon = NSImageView()
        icon.imageScaling = .scaleProportionallyUpOrDown
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 18).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 22).isActive = true

        if rows.isEmpty {
            // An empty list is a state worth naming, not a blank panel.
            title.stringValue = "No device"
            subtitle.stringValue = "waiting for the engine"
            icon.image = NSImage(systemSymbolName: "iphone", accessibilityDescription: nil)
            icon.contentTintColor = .tertiaryLabelColor
        } else {
            let d = rows[row]
            title.stringValue = d.name
            subtitle.stringValue = d.detail
            let glyph = d.isSimulator
                ? (d.name.contains("iPad") ? "ipad" : "iphone")
                : DeviceModel.symbol(for: d.productType)
            icon.image = NSImage(systemSymbolName: glyph, accessibilityDescription: nil)
            // Booted simulators get the accent like a connected phone; shutdown ones stay grey,
            // which is how Device Hub separates "you can look at this" from "you cannot".
            icon.contentTintColor = d.connected ? .controlAccentColor : .tertiaryLabelColor
        }

        let text = NSStackView(views: [title, subtitle])
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 1

        let version = NSTextField(labelWithString: rows.isEmpty ? "" : rows[row].version)
        version.font = .systemFont(ofSize: 11)
        version.textColor = .secondaryLabelColor
        version.alignment = .right

        title.lineBreakMode = .byTruncatingTail
        subtitle.lineBreakMode = .byTruncatingTail
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        subtitle.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        version.setContentCompressionResistancePriority(.required, for: .horizontal)

        let spacer = NSView()
        let stack = NSStackView(views: [icon, text, spacer, version])
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 6
        stack.edgeInsets = NSEdgeInsets(top: 4, left: 6, bottom: 4, right: 6)
        stack.translatesAutoresizingMaskIntoConstraints = false

        cell.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: cell.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: cell.trailingAnchor),
            stack.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !reloading else { return }      // programmatic, not a choice the user made
        if let d = selectedRow() { onSelect?(d) }
    }
}
