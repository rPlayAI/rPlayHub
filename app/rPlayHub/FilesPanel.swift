//
//  FilesPanel.swift
//  The inspector's Files tab: the Media partition and the crash reports, over AFC.
//
//  Backed by the engine's list_dir / read_file / export_crashes. Double-click a folder to enter
//  it, a file to pull it to ~/Downloads/rPlayHub and reveal it. "Export All" copies every crash
//  report there -- what Device Hub's Crashes panel is for.
//

import AppKit

final class FilesPanel: NSView, NSTableViewDataSource, NSTableViewDelegate {
    private struct Entry {
        let name: String
        let size: Int
        let mtime: Date?
        let isDir: Bool
    }

    var control: ControlClient? {
        didSet {
            guard control !== oldValue else { return }
            entries = []
            table.reloadData()
            loaded = false
            status.stringValue = control == nil ? "Not connected to the engine." : ""
            if control != nil, !isHidden { refresh() }
        }
    }

    private var entries: [Entry] = []
    private var path = "/"
    private var loaded = false
    private var loading = false
    private let source = NSSegmentedControl(labels: ["Media", "Crash Reports"], trackingMode: .selectOne,
                                            target: nil, action: nil)
    private let table = NSTableView()
    private let pathLabel = NSTextField(labelWithString: "/")
    private let upButton = NSButton()
    private let exportButton = NSButton()
    private let status = NSTextField(labelWithString: "")

    private var service: String { source.selectedSegment == 1 ? "crash" : "media" }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        build()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        build()
    }

    private func build() {
        source.selectedSegment = 0
        source.controlSize = .small
        source.target = self
        source.action = #selector(sourceChanged)

        let col = NSTableColumn(identifier: .init("file"))
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = 30
        table.dataSource = self
        table.delegate = self
        table.doubleAction = #selector(open)
        table.target = self
        // Consistent with Apps/Profiles: Device Hub's lists are flat, not alternating, all
        // sharing one background color with the tab header row above them.
        table.usesAlternatingRowBackgroundColors = false
        table.style = .plain
        // Same background as the sidebar's device list and Device Hub's own lists -- sampled
        // directly off the live Device Hub window (#E4E4E4), not white.
        let rowBackground = NSColor(srgbRed: 0xE4 / 255, green: 0xE4 / 255, blue: 0xE4 / 255, alpha: 1)
        table.backgroundColor = rowBackground

        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.drawsBackground = true
        scroll.backgroundColor = rowBackground
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
        button(upButton, "Up", "arrow.up", #selector(goUp))
        button(exportButton, "Export All", "square.and.arrow.down", #selector(exportAll))
        exportButton.isHidden = true

        pathLabel.font = .monospacedSystemFont(ofSize: 10, weight: .regular)
        pathLabel.textColor = .secondaryLabelColor
        pathLabel.lineBreakMode = .byTruncatingHead
        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byWordWrapping
        status.maximumNumberOfLines = 3

        let nav = NSStackView(views: [upButton, pathLabel, NSView(), exportButton])
        nav.orientation = .horizontal
        nav.spacing = 6
        let stack = NSStackView(views: [source, nav, scroll, status])
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
            nav.widthAnchor.constraint(equalTo: scroll.widthAnchor),
            status.widthAnchor.constraint(equalTo: scroll.widthAnchor),
        ])
        for v in [self, stack, nav, status, scroll, source, pathLabel, upButton, exportButton] as [NSView] {
            v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            v.setContentHuggingPriority(.init(100), for: .horizontal)
        }
        status.stringValue = "Select a device to browse its files."
    }

    func revealed() {
        if !loaded { refresh() }
    }

    @objc private func sourceChanged() {
        path = "/"
        exportButton.isHidden = service != "crash"
        refresh()
    }

    @objc private func goUp() {
        guard path != "/" else { return }
        var parts = path.split(separator: "/")
        parts.removeLast()
        path = "/" + parts.joined(separator: "/")
        refresh()
    }

    @objc func refresh() {
        guard let control else { status.stringValue = "Not connected to the engine."; return }
        guard !loading else { return }
        loading = true
        pathLabel.stringValue = path
        status.stringValue = "Listing…"
        control.send("list_dir", ["service": service, "path": path]) { [weak self] result in
            guard let self else { return }
            self.loading = false
            switch result {
            case .success(let r):
                let raw = r["entries"] as? [[String: Any]] ?? []
                self.entries = raw.map { e in
                    Entry(name: e["name"] as? String ?? "?",
                          size: (e["size"] as? NSNumber)?.intValue ?? 0,
                          mtime: (e["mtime"] as? NSNumber).map { Date(timeIntervalSince1970: $0.doubleValue) },
                          isDir: e["is_dir"] as? Bool ?? false)
                }.sorted { a, b in
                    if a.isDir != b.isDir { return a.isDir }
                    if self.service == "crash", let x = a.mtime, let y = b.mtime, x != y { return x > y }
                    return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
                }
                self.loaded = true
                self.table.reloadData()
                self.status.stringValue = self.service == "crash"
                    ? "\(self.entries.count) crash reports. Double-click to pull one."
                    : "\(self.entries.count) items. Double-click a folder to enter it, a file to pull it."
            case .failure(let e):
                self.status.stringValue = "Could not list: \(e)"
            }
        }
    }

    private static var downloads: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Downloads/rPlayHub")
    }

    @objc private func open() {
        let r = table.clickedRow
        guard r >= 0, r < entries.count, let control else { return }
        let e = entries[r]
        let full = path == "/" ? "/\(e.name)" : "\(path)/\(e.name)"
        if e.isDir {
            path = full
            refresh()
            return
        }
        status.stringValue = "Pulling \(e.name)…"
        control.send("read_file", ["service": service, "path": full]) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let info):
                guard let b64 = info["data_b64"] as? String, let data = Data(base64Encoded: b64) else {
                    self.status.stringValue = "No data came back for \(e.name)."
                    return
                }
                let dir = Self.downloads
                let url = dir.appendingPathComponent(e.name)
                do {
                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                    try data.write(to: url)
                    NSWorkspace.shared.selectFile(url.path, inFileViewerRootedAtPath: "")
                    self.status.stringValue = "Saved \(e.name) (\(data.count) bytes) to ~/Downloads/rPlayHub."
                } catch {
                    self.status.stringValue = "Could not save: \(error)"
                }
            case .failure(let err):
                self.status.stringValue = "Pull failed: \(err)"
            }
        }
    }

    @objc private func exportAll() {
        guard let control else { return }
        let dir = Self.downloads.appendingPathComponent("crash-reports").path
        status.stringValue = "Exporting crash reports…"
        control.send("export_crashes", ["dir": dir]) { [weak self] result in
            switch result {
            case .success(let r):
                let n = (r["copied"] as? NSNumber)?.intValue ?? 0
                self?.status.stringValue = "Exported \(n) crash reports to ~/Downloads/rPlayHub/crash-reports."
                NSWorkspace.shared.open(URL(fileURLWithPath: dir))
            case .failure(let e):
                self?.status.stringValue = "Export failed: \(e)"
            }
        }
    }

    // MARK: - table

    func numberOfRows(in tableView: NSTableView) -> Int { entries.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let e = entries[row]
        let id = NSUserInterfaceItemIdentifier("cell")
        let cell = tableView.makeView(withIdentifier: id, owner: nil) as? Cell ?? Cell(id)
        cell.icon.image = NSImage(systemSymbolName: e.isDir ? "folder" : "doc", accessibilityDescription: nil)
        cell.title.stringValue = e.name
        var detail: [String] = []
        if !e.isDir { detail.append(ByteCountFormatter.string(fromByteCount: Int64(e.size), countStyle: .file)) }
        if let m = e.mtime { detail.append(Self.short.string(from: m)) }
        cell.detail.stringValue = detail.joined(separator: " · ")
        return cell
    }

    private static let short: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .short
        f.timeStyle = .short
        return f
    }()

    private final class Cell: NSTableCellView {
        let icon = NSImageView()
        let title = NSTextField(labelWithString: "")
        let detail = NSTextField(labelWithString: "")

        init(_ id: NSUserInterfaceItemIdentifier) {
            super.init(frame: .zero)
            identifier = id
            title.font = .systemFont(ofSize: 12)
            title.lineBreakMode = .byTruncatingMiddle
            detail.font = .systemFont(ofSize: 10)
            detail.textColor = .secondaryLabelColor
            icon.contentTintColor = .secondaryLabelColor
            let text = NSStackView(views: [title, detail])
            text.orientation = .vertical
            text.alignment = .leading
            text.spacing = 0
            let row = NSStackView(views: [icon, text])
            row.orientation = .horizontal
            row.spacing = 6
            row.translatesAutoresizingMaskIntoConstraints = false
            addSubview(row)
            NSLayoutConstraint.activate([
                icon.widthAnchor.constraint(equalToConstant: 16),
                row.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
                row.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
                row.centerYAnchor.constraint(equalTo: centerYAnchor),
            ])
            for v in [title, detail, text, row] as [NSView] {
                v.setContentCompressionResistancePriority(.init(100), for: .horizontal)
            }

            // The thin grey line between rows Device Hub's list has -- drawn per-cell rather
            // than via NSTableView.gridStyleMask, which paints grid lines across the table's
            // ENTIRE bounds regardless of actual row count and showed as "row shadows" when a
            // list was empty.
            let divider = NSBox()
            divider.boxType = .separator
            divider.translatesAutoresizingMaskIntoConstraints = false
            addSubview(divider)
            NSLayoutConstraint.activate([
                divider.leadingAnchor.constraint(equalTo: leadingAnchor),
                divider.trailingAnchor.constraint(equalTo: trailingAnchor),
                divider.bottomAnchor.constraint(equalTo: bottomAnchor),
            ])
        }

        required init?(coder: NSCoder) { nil }
    }
}
